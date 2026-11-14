#include <davinci_codec_runtime/api.hpp>

#include <cstdlib>
#include <cstdint>
#include <string>

namespace {

constexpr std::uintptr_t expected_configuration = 0x12345000U;
std::string g_calls;
bool g_software_only = false;
void *g_nvidia_result = nullptr;
void *g_generic_result = nullptr;
void *g_aom_result = nullptr;
void *g_last_properties = nullptr;
int g_nvidia_operation = 0;
int g_generic_operation = 0;
int g_aom_operation = 0;
int g_provider_software_only = -1;

void require(bool condition) {
    if (!condition) std::abort();
}

void *fake_config_getter() {
    g_calls += 'G';
    return reinterpret_cast<void *>(expected_configuration);
}

std::uint8_t fake_software_only_helper(void *configuration) {
    g_calls += 'S';
    require(reinterpret_cast<std::uintptr_t>(configuration) ==
            expected_configuration);
    return g_software_only ? 1U : 0U;
}

void *fake_nvidia_factory(int operation, void *properties) {
    g_calls += 'N';
    g_nvidia_operation = operation;
    g_last_properties = properties;
    return g_nvidia_result;
}

void *fake_generic_factory(int operation, void *properties) {
    g_calls += 'F';
    g_generic_operation = operation;
    g_last_properties = properties;
    return g_generic_result;
}

void *fake_aom_factory(int operation, void *properties) {
    g_calls += 'A';
    g_aom_operation = operation;
    g_last_properties = properties;
    return g_aom_result;
}

std::uintptr_t address_of(auto function) {
    return reinterpret_cast<std::uintptr_t>(function);
}

void reset_case() {
    g_calls.clear();
    g_software_only = false;
    g_nvidia_result = nullptr;
    g_generic_result = nullptr;
    g_aom_result = nullptr;
    g_last_properties = nullptr;
    g_nvidia_operation = 0;
    g_generic_operation = 0;
    g_aom_operation = 0;
    g_provider_software_only = -1;
}

} // namespace

extern "C" void davinci_codec_runtime_set_av1_software_only(
    int software_only) noexcept {
    g_provider_software_only = software_only;
}

int main() {
    require(davinci_codec_runtime_av1_callback_abi_version() ==
            DAVINCI_AV1_CALLBACK_ABI_VERSION);

    void *const properties = reinterpret_cast<void *>(0x76543000U);
    require(davinci_resolve_av1_decoder_callback(1, properties) == nullptr);

    DavinciAv1CallbackBindingsV1 invalid{};
    invalid.abi_version = DAVINCI_AV1_CALLBACK_ABI_VERSION;
    invalid.struct_size = sizeof(invalid);
    require(davinci_codec_runtime_configure_av1_callback(&invalid) == -1);

    DavinciAv1CallbackBindingsV1 bindings{};
    bindings.abi_version = DAVINCI_AV1_CALLBACK_ABI_VERSION;
    bindings.struct_size = sizeof(bindings);
    bindings.resolve_config_getter = address_of(&fake_config_getter);
    bindings.resolve_software_only_helper =
        address_of(&fake_software_only_helper);
    bindings.resolve_nvidia_factory = address_of(&fake_nvidia_factory);
    bindings.resolve_generic_ffmpeg_factory = address_of(&fake_generic_factory);
    bindings.resolve_aom_factory = address_of(&fake_aom_factory);

    require(davinci_codec_runtime_configure_av1_callback(&bindings) == 0);
    require(davinci_codec_runtime_configure_av1_callback(&bindings) == -2);

    // Other operations must remain null and must not invoke Resolve helpers.
    reset_case();
    require(davinci_resolve_av1_decoder_callback(0, properties) == nullptr);
    require(g_calls.empty());

    // Normal mode: NVIDIA wins; generic FFmpeg and AOM are not called.
    reset_case();
    g_nvidia_result = reinterpret_cast<void *>(0x1000U);
    require(davinci_resolve_av1_decoder_callback(1, properties) ==
            g_nvidia_result);
    require(g_calls == "GSN");
    require(g_provider_software_only == 0);
    require(g_nvidia_operation == 1);
    require(g_last_properties == properties);

    // NVIDIA null -> custom generic FFmpeg; AOM remains last.
    reset_case();
    g_generic_result = reinterpret_cast<void *>(0x2000U);
    require(davinci_resolve_av1_decoder_callback(1, properties) ==
            g_generic_result);
    require(g_calls == "GSNF");
    require(g_provider_software_only == 0);
    require(g_nvidia_operation == 1);
    require(g_generic_operation == 1);
    require(g_last_properties == properties);

    // Normal mode exhausted through the exact AOM fallback.
    reset_case();
    g_aom_result = reinterpret_cast<void *>(0x3000U);
    require(davinci_resolve_av1_decoder_callback(1, properties) ==
            g_aom_result);
    require(g_calls == "GSNFA");
    require(g_aom_operation == 1);

    // Exact software-only setting skips NVIDIA and uses the custom decoder.
    reset_case();
    g_software_only = true;
    g_generic_result = reinterpret_cast<void *>(0x4000U);
    require(davinci_resolve_av1_decoder_callback(1, properties) ==
            g_generic_result);
    require(g_calls == "GSF");
    require(g_provider_software_only == 1);
    require(g_generic_operation == 1);

    // Software-only path falls back to AOM only after custom returns null.
    reset_case();
    g_software_only = true;
    g_aom_result = reinterpret_cast<void *>(0x5000U);
    require(davinci_resolve_av1_decoder_callback(1, properties) ==
            g_aom_result);
    require(g_calls == "GSFA");
    require(g_provider_software_only == 1);
    require(g_nvidia_operation == 0);
    require(g_generic_operation == 1);
    require(g_aom_operation == 1);

    // No object means null; there is no post-factory retry policy here.
    reset_case();
    require(davinci_resolve_av1_decoder_callback(1, properties) == nullptr);
    require(g_calls == "GSNFA");

    return 0;
}
