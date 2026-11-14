#define _GNU_SOURCE 1
#include "session_controller_api.h"

#include "bootstrap_api.h"
#include "build_artifact_hashes.hpp"
#include "common/install_authorization.h"
extern "C" {
#include "common/sha256.h"
#include "common/target_identity.h"
}

#include <array>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <link.h>
#include <limits.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace {

constexpr std::uintptr_t resolve_callback_entry = 0x05e3deb0U;
constexpr std::uintptr_t resolve_config_getter = 0x00ef4cf0U;
constexpr std::uintptr_t resolve_software_only_helper = 0x05e444f0U;
constexpr std::uintptr_t resolve_nvidia_factory = 0x05f2cb30U;
constexpr std::uintptr_t resolve_generic_ffmpeg_factory = 0x05eeef00U;
constexpr std::uintptr_t resolve_aom_factory = 0x05f90830U;
constexpr std::uintptr_t resolve_rebalancer = 0x00ecdba0U;
constexpr std::uintptr_t resolve_rebalancer_call = 0x05c87387U;

constexpr std::array<std::uint8_t, 32> expected_resolve_sha256 = {
    0xd0, 0x83, 0xcf, 0x9f, 0x1e, 0xb8, 0x75, 0xbd,
    0xb9, 0x28, 0xdb, 0x54, 0xb2, 0x04, 0x25, 0xb4,
    0x11, 0x1b, 0x53, 0xa1, 0x6c, 0xae, 0x72, 0xa1,
    0xae, 0xf1, 0xc3, 0xee, 0xb6, 0x03, 0xa1, 0x21
};

constexpr std::uint8_t hex_nibble(char value) noexcept {
    if (value >= '0' && value <= '9')
        return static_cast<std::uint8_t>(value - '0');
    if (value >= 'a' && value <= 'f')
        return static_cast<std::uint8_t>(value - 'a' + 10);
    return static_cast<std::uint8_t>(value - 'A' + 10);
}

template <std::size_t N>
constexpr std::array<std::uint8_t, 32> parse_sha256(
    const char (&value)[N]) noexcept {
    static_assert(N == 65U);
    std::array<std::uint8_t, 32> result{};
    for (std::size_t index = 0; index < result.size(); ++index) {
        result[index] = static_cast<std::uint8_t>(
            (hex_nibble(value[index * 2U]) << 4U) |
            hex_nibble(value[index * 2U + 1U]));
    }
    return result;
}

constexpr auto expected_bootstrap_sha256 =
    parse_sha256(DAVINCI_EXPECTED_BOOTSTRAP_SHA256);
constexpr auto expected_runtime_sha256 =
    parse_sha256(DAVINCI_EXPECTED_RUNTIME_SHA256);
constexpr auto expected_avutil_sha256 =
    parse_sha256(DAVINCI_EXPECTED_AVUTIL_SHA256);
constexpr auto expected_avcodec_sha256 =
    parse_sha256(DAVINCI_EXPECTED_AVCODEC_SHA256);

using BootstrapAbiFn = std::uint32_t (*)(void);
using BootstrapValidateFn = int (*)(const PreflightAttestation *);
using BootstrapPrepareFn = int (*)(
    const DavinciAv1TransactionRequestV1 *,
    DavinciAv1TransactionStatusV1 *);
using BootstrapExecuteFn = int (*)(
    const char *, size_t, const DavinciAv1TransactionBindingsV1 *,
    DavinciAv1TransactionExecutionV1 *);
using BootstrapRollbackFn = int (*)(
    const char *, size_t, const DavinciAv1TransactionBindingsV1 *,
    DavinciAv1TransactionExecutionV1 *);
using RuntimeInitializeFn = int (*)(const char *);
using RuntimeShutdownFn = int (*)(void);
using RuntimeReadyFn = int (*)(void);
using RuntimeVersionFn = unsigned int (*)(void);
using CallbackAbiFn = std::uint32_t (*)(void);
using CallbackConfigureFn = int (*)(const DavinciAv1CallbackBindingsV1 *);

struct BootstrapApi {
    BootstrapAbiFn abi_version{};
    BootstrapValidateFn validate_session{};
    BootstrapValidateFn validate_resolve_state{};
    BootstrapPrepareFn prepare_transaction{};
    BootstrapExecuteFn execute_transaction{};
    BootstrapRollbackFn rollback_transaction{};
};

struct RuntimeApi {
    RuntimeInitializeFn initialize{};
    RuntimeShutdownFn shutdown{};
    RuntimeReadyFn ready{};
    RuntimeVersionFn private_avcodec_version{};
    CallbackAbiFn callback_abi_version{};
    CallbackConfigureFn configure_callback{};
    void *callback_target{};
    std::array<void *, DAVINCI_AV1_WRAPPER_COUNT> wrappers{};
};

std::atomic<std::uint32_t> g_state{
    DAVINCI_AV1_SESSION_CONTROLLER_EMPTY
};
int g_last_result = DAVINCI_AV1_SESSION_CONTROLLER_NOT_READY;
void *g_bootstrap_handle = nullptr;
void *g_runtime_handle = nullptr;
BootstrapApi g_bootstrap{};
RuntimeApi g_runtime{};
std::string g_project_root;
std::string g_resolve_path;
std::string g_bootstrap_path;
std::string g_runtime_path;
std::string g_private_ffmpeg_root;
DavinciAv1CallbackBindingsV1 g_callback_bindings{};
DavinciAv1TransactionBindingsV1 g_transaction_bindings{};
DavinciAv1TransactionExecutionV1 g_transaction_execution{};

constexpr std::array<const char *, DAVINCI_AV1_WRAPPER_COUNT> wrapper_symbols = {
    "davinci_avcodec_find_decoder",
    "davinci_avcodec_alloc_context3",
    "davinci_avcodec_open2",
    "davinci_avcodec_close",
    "davinci_avcodec_free_context",
    "davinci_avcodec_send_packet",
    "davinci_avcodec_receive_frame",
    "davinci_avcodec_flush_buffers"
};

bool mapping_has_protection(const void *address, std::size_t length,
                            const char *required) noexcept {
    if (address == nullptr || length == 0U) return false;
    const auto start = reinterpret_cast<std::uintptr_t>(address);
    if (length > UINTPTR_MAX - start) return false;
    const auto end = start + length;

    FILE *maps = std::fopen("/proc/self/maps", "r");
    if (maps == nullptr) return false;
    char line[PATH_MAX + 256];
    auto cursor = start;
    while (std::fgets(line, sizeof(line), maps) != nullptr && cursor < end) {
        unsigned long long map_start = 0U;
        unsigned long long map_end = 0U;
        char permissions[5]{};
        if (std::sscanf(line, "%llx-%llx %4s",
                        &map_start, &map_end, permissions) != 3)
            continue;
        if (cursor < map_start || cursor >= map_end) continue;
        bool ok = true;
        for (const char *p = required; *p != 0; ++p) {
            if (*p == 'r' && permissions[0] != 'r') ok = false;
            if (*p == 'w' && permissions[1] != 'w') ok = false;
            if (*p == 'x' && permissions[2] != 'x') ok = false;
        }
        if (!ok) {
            std::fclose(maps);
            return false;
        }
        cursor = static_cast<std::uintptr_t>(
            map_end < end ? map_end : end);
    }
    std::fclose(maps);
    return cursor == end;
}

bool readable(const void *address, std::size_t length) noexcept {
    return mapping_has_protection(address, length, "r");
}

bool writable(const void *address, std::size_t length) noexcept {
    return mapping_has_protection(address, length, "w");
}

bool executable(const void *address) noexcept {
    return mapping_has_protection(address, 1U, "x");
}

bool sha256_file(const std::string &path,
                 std::array<std::uint8_t, 32> *digest) noexcept {
    if (digest == nullptr) return false;
    FILE *file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) return false;

    DavinciSha256 context;
    davinci_sha256_init(&context);
    std::array<std::uint8_t, 64U * 1024U> buffer{};
    bool success = true;
    for (;;) {
        const std::size_t count =
            std::fread(buffer.data(), 1U, buffer.size(), file);
        if (count != 0U)
            davinci_sha256_update(&context, buffer.data(), count);
        if (count < buffer.size()) {
            if (std::ferror(file) != 0) success = false;
            break;
        }
    }
    if (std::fclose(file) != 0) success = false;
    if (!success) return false;
    davinci_sha256_final(&context, digest->data());
    return true;
}

bool file_hash_matches(
    const std::string &path,
    const std::array<std::uint8_t, 32> &expected) noexcept {
    std::array<std::uint8_t, 32> actual{};
    return sha256_file(path, &actual) && actual == expected;
}

bool canonical_existing_file(const char *input,
                             std::string *output) noexcept {
    if (input == nullptr || input[0] == 0 || output == nullptr) return false;
    char resolved[PATH_MAX]{};
    if (realpath(input, resolved) == nullptr) return false;
    struct stat info {};
    if (stat(resolved, &info) != 0 || !S_ISREG(info.st_mode)) return false;
    try {
        *output = resolved;
    } catch (...) {
        return false;
    }
    return true;
}

bool canonical_existing_directory(const char *input,
                                  std::string *output) noexcept {
    if (input == nullptr || input[0] == 0 || output == nullptr) return false;
    char resolved[PATH_MAX]{};
    if (realpath(input, resolved) == nullptr) return false;
    struct stat info {};
    if (stat(resolved, &info) != 0 || !S_ISDIR(info.st_mode)) return false;
    try {
        *output = resolved;
    } catch (...) {
        return false;
    }
    return true;
}

bool append_path(const std::string &root, const char *relative,
                 std::string *output) noexcept {
    if (output == nullptr) return false;
    try {
        *output = root;
        if (output->empty() || output->back() != '/') output->push_back('/');
        output->append(relative);
    } catch (...) {
        return false;
    }
    return true;
}

bool owner_matches(void *address, const std::string &expected_path) noexcept {
    if (address == nullptr || !executable(address)) return false;
    Dl_info information{};
    if (dladdr(address, &information) == 0 ||
        information.dli_fname == nullptr)
        return false;
    char resolved[PATH_MAX]{};
    if (realpath(information.dli_fname, resolved) == nullptr) return false;
    return expected_path == resolved;
}

bool owner_ends_with(void *address, const char *suffix) noexcept {
    if (address == nullptr || suffix == nullptr || !executable(address))
        return false;
    Dl_info information{};
    if (dladdr(address, &information) == 0 ||
        information.dli_fname == nullptr)
        return false;
    const std::size_t path_length = std::strlen(information.dli_fname);
    const std::size_t suffix_length = std::strlen(suffix);
    return path_length >= suffix_length &&
           std::strcmp(information.dli_fname + path_length - suffix_length,
                       suffix) == 0;
}

template <typename Function>
Function lookup_function(void *handle, const char *name) noexcept {
    if (handle == nullptr || name == nullptr) return nullptr;
    dlerror();
    void *address = dlsym(handle, name);
    if (dlerror() != nullptr) return nullptr;
    return reinterpret_cast<Function>(address);
}

void *lookup_object(void *handle, const char *name) noexcept {
    if (handle == nullptr || name == nullptr) return nullptr;
    dlerror();
    void *address = dlsym(handle, name);
    if (dlerror() != nullptr) return nullptr;
    return address;
}

bool valid_config(const DavinciAv1SessionControllerConfigV1 *config) noexcept {
    if (config == nullptr || !readable(config, sizeof(*config)) ||
        (reinterpret_cast<std::uintptr_t>(config) %
         alignof(DavinciAv1SessionControllerConfigV1)) != 0U)
        return false;
    if (config->magic != DAVINCI_AV1_SESSION_CONTROLLER_CONFIG_MAGIC ||
        config->version != DAVINCI_AV1_SESSION_CONTROLLER_ABI_VERSION ||
        config->struct_size != sizeof(*config) ||
        config->flags != 0U || config->reserved != 0U)
        return false;
    for (const auto byte : config->reserved_tail)
        if (byte != 0U) return false;
    return std::memchr(config->project_root, 0,
                       sizeof(config->project_root)) != nullptr &&
           std::memchr(config->resolve_executable, 0,
                       sizeof(config->resolve_executable)) != nullptr;
}

bool compute_rel32_target(std::uintptr_t instruction, std::uintptr_t *target) {
    if (target == nullptr || !executable(reinterpret_cast<void *>(instruction)))
        return false;
    const std::uint8_t *bytes =
        reinterpret_cast<const std::uint8_t *>(instruction);
    if (!readable(bytes, 5U) || bytes[0] != 0xe8U) return false;
    std::int32_t relative = 0;
    std::memcpy(&relative, bytes + 1U, sizeof(relative));
    const std::uintptr_t end = instruction + 5U;
    if (relative >= 0) {
        const auto displacement = static_cast<std::uintptr_t>(relative);
        if (displacement > UINTPTR_MAX - end) return false;
        *target = end + displacement;
    } else {
        const auto magnitude = static_cast<std::uintptr_t>(
            -(static_cast<std::int64_t>(relative)));
        if (magnitude > end) return false;
        *target = end - magnitude;
    }
    return true;
}

int fail(int result) noexcept {
    g_last_result = result;
    g_state.store(DAVINCI_AV1_SESSION_CONTROLLER_FAILED,
                  std::memory_order_release);
    return result;
}

void unload_partial() noexcept {
    if (g_runtime_handle != nullptr) {
        if (g_runtime.shutdown != nullptr &&
            g_runtime.ready != nullptr && g_runtime.ready() != 0)
            (void)g_runtime.shutdown();
        (void)dlclose(g_runtime_handle);
        g_runtime_handle = nullptr;
    }
    if (g_bootstrap_handle != nullptr) {
        (void)dlclose(g_bootstrap_handle);
        g_bootstrap_handle = nullptr;
    }
    g_runtime = {};
    g_bootstrap = {};
}

int load_bootstrap_api() noexcept {
    g_bootstrap.abi_version = lookup_function<BootstrapAbiFn>(
        g_bootstrap_handle, "davinci_av1_bootstrap_abi_version");
    g_bootstrap.validate_session = lookup_function<BootstrapValidateFn>(
        g_bootstrap_handle, "davinci_av1_bootstrap_validate_session");
    g_bootstrap.validate_resolve_state =
        lookup_function<BootstrapValidateFn>(
            g_bootstrap_handle, "davinci_av1_bootstrap_validate_resolve_state");
    g_bootstrap.prepare_transaction = lookup_function<BootstrapPrepareFn>(
        g_bootstrap_handle, "davinci_av1_bootstrap_prepare_transaction");
    g_bootstrap.execute_transaction = lookup_function<BootstrapExecuteFn>(
        g_bootstrap_handle,
        "davinci_av1_bootstrap_execute_prepared_transaction");
    g_bootstrap.rollback_transaction = lookup_function<BootstrapRollbackFn>(
        g_bootstrap_handle,
        "davinci_av1_bootstrap_rollback_prepared_transaction");
    if (g_bootstrap.abi_version == nullptr ||
        g_bootstrap.validate_session == nullptr ||
        g_bootstrap.validate_resolve_state == nullptr ||
        g_bootstrap.prepare_transaction == nullptr ||
        g_bootstrap.execute_transaction == nullptr ||
        g_bootstrap.rollback_transaction == nullptr)
        return DAVINCI_AV1_SESSION_CONTROLLER_SYMBOL_MISSING;
    if (g_bootstrap.abi_version() != DAVINCI_AV1_BOOTSTRAP_ABI_VERSION)
        return DAVINCI_AV1_SESSION_CONTROLLER_ARTIFACT_MISMATCH;
    const std::array<void *, 6> bootstrap_functions = {
        reinterpret_cast<void *>(g_bootstrap.abi_version),
        reinterpret_cast<void *>(g_bootstrap.validate_session),
        reinterpret_cast<void *>(g_bootstrap.validate_resolve_state),
        reinterpret_cast<void *>(g_bootstrap.prepare_transaction),
        reinterpret_cast<void *>(g_bootstrap.execute_transaction),
        reinterpret_cast<void *>(g_bootstrap.rollback_transaction)
    };
    for (void *function : bootstrap_functions) {
        if (!owner_matches(function, g_bootstrap_path))
            return DAVINCI_AV1_SESSION_CONTROLLER_TARGET_MISMATCH;
    }
    return DAVINCI_AV1_SESSION_CONTROLLER_OK;
}

int load_runtime_api() noexcept {
    g_runtime.initialize = lookup_function<RuntimeInitializeFn>(
        g_runtime_handle, "davinci_codec_runtime_initialize_ffmpeg");
    g_runtime.shutdown = lookup_function<RuntimeShutdownFn>(
        g_runtime_handle, "davinci_codec_runtime_shutdown_ffmpeg");
    g_runtime.ready = lookup_function<RuntimeReadyFn>(
        g_runtime_handle, "davinci_codec_runtime_ffmpeg_ready");
    g_runtime.private_avcodec_version = lookup_function<RuntimeVersionFn>(
        g_runtime_handle, "davinci_codec_runtime_private_avcodec_version");
    g_runtime.callback_abi_version = lookup_function<CallbackAbiFn>(
        g_runtime_handle, "davinci_codec_runtime_av1_callback_abi_version");
    g_runtime.configure_callback = lookup_function<CallbackConfigureFn>(
        g_runtime_handle, "davinci_codec_runtime_configure_av1_callback");
    g_runtime.callback_target = lookup_object(
        g_runtime_handle, "davinci_resolve_av1_decoder_callback");

    for (std::size_t index = 0; index < wrapper_symbols.size(); ++index)
        g_runtime.wrappers[index] =
            lookup_object(g_runtime_handle, wrapper_symbols[index]);

    if (g_runtime.initialize == nullptr || g_runtime.shutdown == nullptr ||
        g_runtime.ready == nullptr ||
        g_runtime.private_avcodec_version == nullptr ||
        g_runtime.callback_abi_version == nullptr ||
        g_runtime.configure_callback == nullptr ||
        g_runtime.callback_target == nullptr)
        return DAVINCI_AV1_SESSION_CONTROLLER_SYMBOL_MISSING;
    for (void *wrapper : g_runtime.wrappers)
        if (wrapper == nullptr)
            return DAVINCI_AV1_SESSION_CONTROLLER_SYMBOL_MISSING;
    if (g_runtime.callback_abi_version() !=
        DAVINCI_AV1_CALLBACK_ABI_VERSION)
        return DAVINCI_AV1_SESSION_CONTROLLER_ARTIFACT_MISMATCH;
    const std::array<void *, 7> runtime_functions = {
        reinterpret_cast<void *>(g_runtime.initialize),
        reinterpret_cast<void *>(g_runtime.shutdown),
        reinterpret_cast<void *>(g_runtime.ready),
        reinterpret_cast<void *>(g_runtime.private_avcodec_version),
        reinterpret_cast<void *>(g_runtime.callback_abi_version),
        reinterpret_cast<void *>(g_runtime.configure_callback),
        g_runtime.callback_target
    };
    for (void *function : runtime_functions) {
        if (!owner_matches(function, g_runtime_path))
            return DAVINCI_AV1_SESSION_CONTROLLER_TARGET_MISMATCH;
    }
    return DAVINCI_AV1_SESSION_CONTROLLER_OK;
}

int validate_resolve_targets() noexcept {
    struct Target {
        std::uintptr_t address;
        const char *name;
    };
    constexpr std::array<Target, 5> targets{{
        {resolve_config_getter, "Resolve configuration singleton getter"},
        {resolve_software_only_helper, "Resolve cached software-only helper"},
        {resolve_nvidia_factory, "Resolve NVIDIA decoder factory"},
        {resolve_generic_ffmpeg_factory, "Resolve generic FFmpeg factory"},
        {resolve_aom_factory, "Resolve AOM AV1 factory"}
    }};
    for (const auto &target : targets) {
        if (!owner_matches(reinterpret_cast<void *>(target.address),
                           g_resolve_path))
            return DAVINCI_AV1_SESSION_CONTROLLER_TARGET_MISMATCH;
    }

    const auto *callback =
        reinterpret_cast<const std::uint8_t *>(resolve_callback_entry);
    constexpr std::array<std::uint8_t, 8> expected_callback = {
        0x53, 0x83, 0xff, 0x01, 0x75, 0x28, 0x48, 0x89
    };
    if (!readable(callback, expected_callback.size()) ||
        std::memcmp(callback, expected_callback.data(),
                    expected_callback.size()) != 0)
        return DAVINCI_AV1_SESSION_CONTROLLER_TARGET_MISMATCH;

    std::uintptr_t rebalancer = 0U;
    if (!compute_rel32_target(resolve_rebalancer_call, &rebalancer) ||
        rebalancer != resolve_rebalancer ||
        !owner_matches(reinterpret_cast<void *>(resolve_rebalancer),
                       g_resolve_path))
        return DAVINCI_AV1_SESSION_CONTROLLER_TARGET_MISMATCH;
    return DAVINCI_AV1_SESSION_CONTROLLER_OK;
}

int validate_runtime_targets() noexcept {
    for (void *wrapper : g_runtime.wrappers)
        if (!owner_matches(wrapper, g_runtime_path))
            return DAVINCI_AV1_SESSION_CONTROLLER_TARGET_MISMATCH;
    if (!owner_matches(g_runtime.callback_target, g_runtime_path))
        return DAVINCI_AV1_SESSION_CONTROLLER_TARGET_MISMATCH;
    return DAVINCI_AV1_SESSION_CONTROLLER_OK;
}

int initialize_impl(const DavinciAv1SessionControllerConfigV1 *config) noexcept {
    if (!valid_config(config))
        return DAVINCI_AV1_SESSION_CONTROLLER_INVALID_ARGUMENT;

    if (!canonical_existing_directory(config->project_root, &g_project_root) ||
        !canonical_existing_file(config->resolve_executable, &g_resolve_path) ||
        g_resolve_path != "/opt/resolve/bin/resolve")
        return DAVINCI_AV1_SESSION_CONTROLLER_PATH_INVALID;

    std::string candidate;
    if (!file_hash_matches(g_resolve_path, expected_resolve_sha256))
        return DAVINCI_AV1_SESSION_CONTROLLER_ARTIFACT_MISMATCH;

    if (!append_path(g_project_root,
                     "build/clang-libcxx/"
                     "libdavinci_av1_bootstrap.so.0.1.0", &candidate) ||
        !canonical_existing_file(candidate.c_str(), &g_bootstrap_path) ||
        !file_hash_matches(g_bootstrap_path, expected_bootstrap_sha256))
        return DAVINCI_AV1_SESSION_CONTROLLER_ARTIFACT_MISMATCH;

    if (!append_path(g_project_root,
                     "build/clang-libcxx/"
                     "libdavinci_codec_runtime.so.0.1.0", &candidate) ||
        !canonical_existing_file(candidate.c_str(), &g_runtime_path) ||
        !file_hash_matches(g_runtime_path, expected_runtime_sha256))
        return DAVINCI_AV1_SESSION_CONTROLLER_ARTIFACT_MISMATCH;

    if (!append_path(g_project_root,
                     "build/private-ffmpeg", &g_private_ffmpeg_root) ||
        !append_path(g_private_ffmpeg_root,
                     "lib/libavutil.so.58", &candidate) ||
        !file_hash_matches(candidate, expected_avutil_sha256))
        return DAVINCI_AV1_SESSION_CONTROLLER_ARTIFACT_MISMATCH;
    if (!append_path(g_private_ffmpeg_root,
                     "lib/libavcodec.so.60", &candidate) ||
        !file_hash_matches(candidate, expected_avcodec_sha256))
        return DAVINCI_AV1_SESSION_CONTROLLER_ARTIFACT_MISMATCH;

    dlerror();
    g_bootstrap_handle =
        dlopen(g_bootstrap_path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (g_bootstrap_handle == nullptr)
        return DAVINCI_AV1_SESSION_CONTROLLER_DLOPEN_FAILED;
    if (!owner_matches(
            lookup_object(g_bootstrap_handle,
                          "davinci_av1_bootstrap_abi_version"),
            g_bootstrap_path))
        return DAVINCI_AV1_SESSION_CONTROLLER_TARGET_MISMATCH;

    const int bootstrap_status = load_bootstrap_api();
    if (bootstrap_status != DAVINCI_AV1_SESSION_CONTROLLER_OK)
        return bootstrap_status;

    dlerror();
    g_runtime_handle =
        dlopen(g_runtime_path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (g_runtime_handle == nullptr)
        return DAVINCI_AV1_SESSION_CONTROLLER_DLOPEN_FAILED;
    if (!owner_matches(
            lookup_object(g_runtime_handle,
                          "davinci_codec_runtime_abi_version"),
            g_runtime_path))
        return DAVINCI_AV1_SESSION_CONTROLLER_TARGET_MISMATCH;

    const int runtime_status = load_runtime_api();
    if (runtime_status != DAVINCI_AV1_SESSION_CONTROLLER_OK)
        return runtime_status;

    const int resolve_status = validate_resolve_targets();
    if (resolve_status != DAVINCI_AV1_SESSION_CONTROLLER_OK)
        return resolve_status;
    const int runtime_target_status = validate_runtime_targets();
    if (runtime_target_status != DAVINCI_AV1_SESSION_CONTROLLER_OK)
        return runtime_target_status;

    if (g_runtime.initialize(g_private_ffmpeg_root.c_str()) != 0 ||
        g_runtime.ready() == 0 ||
        g_runtime.private_avcodec_version() != 3940198U)
        return DAVINCI_AV1_SESSION_CONTROLLER_PROVIDER_FAILED;

    g_callback_bindings = {};
    g_callback_bindings.abi_version = DAVINCI_AV1_CALLBACK_ABI_VERSION;
    g_callback_bindings.struct_size = sizeof(g_callback_bindings);
    g_callback_bindings.resolve_config_getter = resolve_config_getter;
    g_callback_bindings.resolve_software_only_helper =
        resolve_software_only_helper;
    g_callback_bindings.resolve_nvidia_factory = resolve_nvidia_factory;
    g_callback_bindings.resolve_generic_ffmpeg_factory =
        resolve_generic_ffmpeg_factory;
    g_callback_bindings.resolve_aom_factory = resolve_aom_factory;

    if (g_runtime.configure_callback(&g_callback_bindings) != 0)
        return DAVINCI_AV1_SESSION_CONTROLLER_CALLBACK_FAILED;

    return DAVINCI_AV1_SESSION_CONTROLLER_OK;
}

bool valid_attestation(const PreflightAttestation *attestation) noexcept {
    if (attestation == nullptr || !readable(attestation, sizeof(*attestation)) ||
        (reinterpret_cast<std::uintptr_t>(attestation) %
         alignof(PreflightAttestation)) != 0U)
        return false;
    if (attestation->magic != PREFLIGHT_MAGIC ||
        attestation->version != PREFLIGHT_VERSION ||
        attestation->struct_size != sizeof(*attestation) ||
        attestation->total_checks != PREFLIGHT_EXPECTED_CHECKS ||
        attestation->passed_checks != PREFLIGHT_EXPECTED_CHECKS ||
        attestation->failures != 0U ||
        attestation->reserved != 0U ||
        attestation->flags != PREFLIGHT_REQUIRED_FLAGS)
        return false;
    if (attestation->session.process_id !=
            static_cast<std::uint64_t>(getpid()) ||
        attestation->session.stopped_thread_count != 1U ||
        attestation->session.generic_map != 0x29537cf0U ||
        attestation->session.av1_callback != resolve_callback_entry ||
        attestation->session.reserved != 0U ||
        attestation->session.operator_new == 0U ||
        attestation->session.sized_delete == 0U)
        return false;
    if (!owner_ends_with(
            reinterpret_cast<void *>(attestation->session.operator_new),
            "libtbbmalloc_proxy.so.2") ||
        !owner_ends_with(
            reinterpret_cast<void *>(attestation->session.sized_delete),
            "libcodexhdedecoder.so.5"))
        return false;
    return true;
}

bool build_transaction_bindings(
    const PreflightAttestation *attestation,
    DavinciAv1TransactionBindingsV1 *bindings) noexcept {
    if (attestation == nullptr || bindings == nullptr ||
        g_runtime.callback_target == nullptr ||
        !owner_matches(g_runtime.callback_target, g_runtime_path))
        return false;

    DavinciAv1TransactionBindingsV1 result{};
    result.allocator_target = attestation->session.operator_new;
    result.sized_delete_target = attestation->session.sized_delete;
    result.rebalancer_target = resolve_rebalancer;
    result.callback_entry = resolve_callback_entry;
    result.callback_target =
        reinterpret_cast<std::uintptr_t>(g_runtime.callback_target);
    for (std::size_t index = 0; index < g_runtime.wrappers.size(); ++index) {
        if (g_runtime.wrappers[index] == nullptr ||
            !owner_matches(g_runtime.wrappers[index], g_runtime_path))
            return false;
        result.wrapper_targets[index] =
            reinterpret_cast<std::uintptr_t>(g_runtime.wrappers[index]);
    }
    *bindings = result;
    return true;
}

bool authorized_request(const char *authorization,
                        std::size_t authorization_size) noexcept {
    return authorization != nullptr &&
           authorization_size ==
               sizeof(DAVINCI_AV1_INSTALL_AUTHORIZATION_TEXT) - 1U &&
           readable(authorization, authorization_size) &&
           davinci_av1_install_authorization_is_valid(
               authorization, authorization_size) != 0;
}

bool current_inferior_is_single_threaded() noexcept {
    std::uint32_t thread_count = 0U;
    return davinci_count_self_threads(&thread_count) == 0 &&
           thread_count == 1U;
}

} // namespace

extern "C" DAVINCI_AV1_SESSION_CONTROLLER_API std::uint32_t
davinci_av1_session_controller_abi_version(void) {
    return DAVINCI_AV1_SESSION_CONTROLLER_ABI_VERSION;
}

extern "C" DAVINCI_AV1_SESSION_CONTROLLER_API int
davinci_av1_session_controller_initialize(
    const DavinciAv1SessionControllerConfigV1 *config) {
    std::uint32_t expected = DAVINCI_AV1_SESSION_CONTROLLER_EMPTY;
    if (!g_state.compare_exchange_strong(
            expected, DAVINCI_AV1_SESSION_CONTROLLER_LOADING,
            std::memory_order_acq_rel, std::memory_order_acquire))
        return DAVINCI_AV1_SESSION_CONTROLLER_ALREADY_USED;

    const int result = initialize_impl(config);
    if (result != DAVINCI_AV1_SESSION_CONTROLLER_OK) {
        unload_partial();
        return fail(result);
    }
    g_last_result = DAVINCI_AV1_SESSION_CONTROLLER_OK;
    g_state.store(DAVINCI_AV1_SESSION_CONTROLLER_BOUND,
                  std::memory_order_release);
    return DAVINCI_AV1_SESSION_CONTROLLER_OK;
}

extern "C" DAVINCI_AV1_SESSION_CONTROLLER_API int
davinci_av1_session_controller_get_status(
    DavinciAv1SessionControllerStatusV1 *status) {
    if (status == nullptr || !writable(status, sizeof(*status)) ||
        (reinterpret_cast<std::uintptr_t>(status) %
         alignof(DavinciAv1SessionControllerStatusV1)) != 0U)
        return DAVINCI_AV1_SESSION_CONTROLLER_BUFFER_INVALID;

    DavinciAv1SessionControllerStatusV1 result{};
    result.magic = DAVINCI_AV1_SESSION_CONTROLLER_STATUS_MAGIC;
    result.version = DAVINCI_AV1_SESSION_CONTROLLER_ABI_VERSION;
    result.struct_size = sizeof(result);
    result.result = g_last_result;
    result.state = g_state.load(std::memory_order_acquire);
    result.provider_ready =
        g_runtime.ready != nullptr ? static_cast<std::uint32_t>(
            g_runtime.ready() != 0) : 0U;
    result.callback_configured =
        result.state >= DAVINCI_AV1_SESSION_CONTROLLER_BOUND &&
        result.state != DAVINCI_AV1_SESSION_CONTROLLER_FAILED ? 1U : 0U;
    result.private_avcodec_version =
        g_runtime.private_avcodec_version != nullptr
            ? g_runtime.private_avcodec_version() : 0U;
    std::memcpy(status, &result, sizeof(result));
    return DAVINCI_AV1_SESSION_CONTROLLER_OK;
}

extern "C" DAVINCI_AV1_SESSION_CONTROLLER_API int
davinci_av1_session_controller_get_plan(
    const PreflightAttestation *attestation,
    DavinciAv1SessionControllerPlanV1 *plan) {
    if (plan == nullptr || !writable(plan, sizeof(*plan)) ||
        (reinterpret_cast<std::uintptr_t>(plan) %
         alignof(DavinciAv1SessionControllerPlanV1)) != 0U)
        return DAVINCI_AV1_SESSION_CONTROLLER_BUFFER_INVALID;
    if (g_state.load(std::memory_order_acquire) !=
            DAVINCI_AV1_SESSION_CONTROLLER_BOUND &&
        g_state.load(std::memory_order_acquire) !=
            DAVINCI_AV1_SESSION_CONTROLLER_PREFLIGHT_READY)
        return DAVINCI_AV1_SESSION_CONTROLLER_NOT_READY;
    if (!valid_attestation(attestation))
        return DAVINCI_AV1_SESSION_CONTROLLER_ATTESTATION_INVALID;

    DavinciAv1SessionControllerPlanV1 result{};
    result.magic = DAVINCI_AV1_SESSION_CONTROLLER_PLAN_MAGIC;
    result.version = DAVINCI_AV1_SESSION_CONTROLLER_ABI_VERSION;
    result.struct_size = sizeof(result);
    result.state = g_state.load(std::memory_order_acquire);

    if (!build_transaction_bindings(
            attestation, &result.transaction_bindings))
        return DAVINCI_AV1_SESSION_CONTROLLER_TARGET_MISMATCH;
    result.callback_bindings = g_callback_bindings;

    std::memcpy(plan, &result, sizeof(result));
    return DAVINCI_AV1_SESSION_CONTROLLER_OK;
}

extern "C" DAVINCI_AV1_SESSION_CONTROLLER_API int
davinci_av1_session_controller_prepare(
    const PreflightAttestation *attestation,
    DavinciAv1TransactionStatusV1 *status) {
    if (status == nullptr || !writable(status, sizeof(*status)) ||
        (reinterpret_cast<std::uintptr_t>(status) %
         alignof(DavinciAv1TransactionStatusV1)) != 0U)
        return DAVINCI_AV1_SESSION_CONTROLLER_BUFFER_INVALID;
    std::uint32_t expected_state = DAVINCI_AV1_SESSION_CONTROLLER_BOUND;
    if (!g_state.compare_exchange_strong(
            expected_state, DAVINCI_AV1_SESSION_CONTROLLER_LOADING,
            std::memory_order_acq_rel, std::memory_order_acquire))
        return DAVINCI_AV1_SESSION_CONTROLLER_NOT_READY;
    if (!valid_attestation(attestation)) {
        g_last_result = DAVINCI_AV1_SESSION_CONTROLLER_ATTESTATION_INVALID;
        return fail(DAVINCI_AV1_SESSION_CONTROLLER_ATTESTATION_INVALID);
    }

    const int session_status = g_bootstrap.validate_session(attestation);
    if (session_status != DAVINCI_AV1_BOOTSTRAP_OK) {
        g_last_result = DAVINCI_AV1_SESSION_CONTROLLER_BOOTSTRAP_REJECTED;
        return fail(DAVINCI_AV1_SESSION_CONTROLLER_BOOTSTRAP_REJECTED);
    }
    const int resolve_status =
        g_bootstrap.validate_resolve_state(attestation);
    if (resolve_status != DAVINCI_AV1_BOOTSTRAP_OK) {
        g_last_result = DAVINCI_AV1_SESSION_CONTROLLER_BOOTSTRAP_REJECTED;
        return fail(DAVINCI_AV1_SESSION_CONTROLLER_BOOTSTRAP_REJECTED);
    }

    DavinciAv1TransactionBindingsV1 transaction_bindings{};
    if (!build_transaction_bindings(attestation, &transaction_bindings)) {
        g_last_result = DAVINCI_AV1_SESSION_CONTROLLER_TARGET_MISMATCH;
        return fail(DAVINCI_AV1_SESSION_CONTROLLER_TARGET_MISMATCH);
    }

    DavinciAv1TransactionRequestV1 request{};
    request.magic = DAVINCI_AV1_TRANSACTION_REQUEST_MAGIC;
    request.version = DAVINCI_AV1_TRANSACTION_API_VERSION;
    request.struct_size = sizeof(request);
    request.attestation = attestation;
    std::memcpy(request.session_nonce, attestation->session.session_nonce,
                sizeof(request.session_nonce));

    DavinciAv1TransactionStatusV1 local_status{};
    const int prepare_status =
        g_bootstrap.prepare_transaction(&request, &local_status);
    std::memcpy(status, &local_status, sizeof(local_status));
    if (prepare_status != DAVINCI_AV1_BOOTSTRAP_OK) {
        g_last_result = DAVINCI_AV1_SESSION_CONTROLLER_BOOTSTRAP_REJECTED;
        return fail(DAVINCI_AV1_SESSION_CONTROLLER_BOOTSTRAP_REJECTED);
    }

    g_transaction_bindings = transaction_bindings;
    std::memset(&g_transaction_execution, 0, sizeof(g_transaction_execution));
    g_last_result = DAVINCI_AV1_SESSION_CONTROLLER_OK;
    g_state.store(DAVINCI_AV1_SESSION_CONTROLLER_PREFLIGHT_READY,
                  std::memory_order_release);
    return DAVINCI_AV1_SESSION_CONTROLLER_OK;
}


extern "C" DAVINCI_AV1_SESSION_CONTROLLER_API int
davinci_av1_session_controller_execute(
    const char *authorization, std::size_t authorization_size,
    DavinciAv1TransactionExecutionV1 *execution) {
    if (!authorized_request(authorization, authorization_size))
        return DAVINCI_AV1_SESSION_CONTROLLER_AUTHORIZATION_REQUIRED;
    if (execution == nullptr || !writable(execution, sizeof(*execution)) ||
        (reinterpret_cast<std::uintptr_t>(execution) %
         alignof(DavinciAv1TransactionExecutionV1)) != 0U)
        return DAVINCI_AV1_SESSION_CONTROLLER_BUFFER_INVALID;
    if (g_state.load(std::memory_order_acquire) !=
        DAVINCI_AV1_SESSION_CONTROLLER_PREFLIGHT_READY)
        return DAVINCI_AV1_SESSION_CONTROLLER_NOT_READY;
    if (!current_inferior_is_single_threaded()) {
        g_last_result = DAVINCI_AV1_SESSION_CONTROLLER_THREAD_MISMATCH;
        return DAVINCI_AV1_SESSION_CONTROLLER_THREAD_MISMATCH;
    }

    std::uint32_t expected_state =
        DAVINCI_AV1_SESSION_CONTROLLER_PREFLIGHT_READY;
    if (!g_state.compare_exchange_strong(
            expected_state, DAVINCI_AV1_SESSION_CONTROLLER_EXECUTING,
            std::memory_order_acq_rel, std::memory_order_acquire))
        return DAVINCI_AV1_SESSION_CONTROLLER_NOT_READY;

    DavinciAv1TransactionExecutionV1 local_execution{};
    const int bootstrap_result = g_bootstrap.execute_transaction(
        authorization, authorization_size, &g_transaction_bindings,
        &local_execution);
    g_transaction_execution = local_execution;
    std::memcpy(execution, &local_execution, sizeof(local_execution));

    std::uint32_t final_state = DAVINCI_AV1_SESSION_CONTROLLER_FAILED;
    if (bootstrap_result == DAVINCI_AV1_EXECUTION_OK &&
        local_execution.state == DAVINCI_AV1_EXECUTION_COMMITTED) {
        final_state = DAVINCI_AV1_SESSION_CONTROLLER_COMMITTED;
        g_last_result = DAVINCI_AV1_SESSION_CONTROLLER_OK;
    } else if (local_execution.state == DAVINCI_AV1_EXECUTION_ROLLED_BACK) {
        final_state = DAVINCI_AV1_SESSION_CONTROLLER_ROLLED_BACK;
        g_last_result = DAVINCI_AV1_SESSION_CONTROLLER_EXECUTION_FAILED;
    } else if (local_execution.state == DAVINCI_AV1_EXECUTION_FAIL_STOP) {
        final_state = DAVINCI_AV1_SESSION_CONTROLLER_FAIL_STOP;
        g_last_result = DAVINCI_AV1_SESSION_CONTROLLER_EXECUTION_FAILED;
    } else if (local_execution.state == DAVINCI_AV1_EXECUTION_COMMITTED) {
        final_state = DAVINCI_AV1_SESSION_CONTROLLER_FAIL_STOP;
        g_last_result = DAVINCI_AV1_SESSION_CONTROLLER_EXECUTION_FAILED;
    } else {
        g_last_result = DAVINCI_AV1_SESSION_CONTROLLER_EXECUTION_FAILED;
    }

    g_state.store(final_state, std::memory_order_release);
    return final_state == DAVINCI_AV1_SESSION_CONTROLLER_COMMITTED
        ? DAVINCI_AV1_SESSION_CONTROLLER_OK
        : DAVINCI_AV1_SESSION_CONTROLLER_EXECUTION_FAILED;
}

extern "C" DAVINCI_AV1_SESSION_CONTROLLER_API int
davinci_av1_session_controller_rollback(
    const char *authorization, std::size_t authorization_size,
    DavinciAv1TransactionExecutionV1 *execution) {
    if (!authorized_request(authorization, authorization_size))
        return DAVINCI_AV1_SESSION_CONTROLLER_AUTHORIZATION_REQUIRED;
    if (execution == nullptr || !writable(execution, sizeof(*execution)) ||
        (reinterpret_cast<std::uintptr_t>(execution) %
         alignof(DavinciAv1TransactionExecutionV1)) != 0U)
        return DAVINCI_AV1_SESSION_CONTROLLER_BUFFER_INVALID;
    if (g_state.load(std::memory_order_acquire) !=
        DAVINCI_AV1_SESSION_CONTROLLER_COMMITTED)
        return DAVINCI_AV1_SESSION_CONTROLLER_NOT_READY;
    if (!current_inferior_is_single_threaded()) {
        g_last_result = DAVINCI_AV1_SESSION_CONTROLLER_THREAD_MISMATCH;
        return DAVINCI_AV1_SESSION_CONTROLLER_THREAD_MISMATCH;
    }

    std::uint32_t expected_state = DAVINCI_AV1_SESSION_CONTROLLER_COMMITTED;
    if (!g_state.compare_exchange_strong(
            expected_state, DAVINCI_AV1_SESSION_CONTROLLER_EXECUTING,
            std::memory_order_acq_rel, std::memory_order_acquire))
        return DAVINCI_AV1_SESSION_CONTROLLER_NOT_READY;

    const int bootstrap_result = g_bootstrap.rollback_transaction(
        authorization, authorization_size, &g_transaction_bindings,
        &g_transaction_execution);
    std::memcpy(execution, &g_transaction_execution, sizeof(*execution));

    if (bootstrap_result == DAVINCI_AV1_EXECUTION_OK &&
        g_transaction_execution.state == DAVINCI_AV1_EXECUTION_ROLLED_BACK) {
        g_last_result = DAVINCI_AV1_SESSION_CONTROLLER_OK;
        g_state.store(DAVINCI_AV1_SESSION_CONTROLLER_ROLLED_BACK,
                      std::memory_order_release);
        return DAVINCI_AV1_SESSION_CONTROLLER_OK;
    }

    g_last_result = DAVINCI_AV1_SESSION_CONTROLLER_ROLLBACK_FAILED;
    g_state.store(DAVINCI_AV1_SESSION_CONTROLLER_FAIL_STOP,
                  std::memory_order_release);
    return DAVINCI_AV1_SESSION_CONTROLLER_ROLLBACK_FAILED;
}
