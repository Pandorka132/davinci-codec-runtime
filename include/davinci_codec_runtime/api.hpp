#pragma once

#include <cstddef>
#include <cstdint>

#if defined(__GNUC__)
#define DAVINCI_CODEC_RUNTIME_API __attribute__((visibility("default")))
#else
#define DAVINCI_CODEC_RUNTIME_API
#endif

#define DAVINCI_AV1_CALLBACK_ABI_VERSION 1U

struct DavinciAv1CallbackBindingsV1 {
    std::uint32_t abi_version;
    std::uint32_t struct_size;
    std::uintptr_t resolve_config_getter;
    std::uintptr_t resolve_software_only_helper;
    std::uintptr_t resolve_nvidia_factory;
    std::uintptr_t resolve_generic_ffmpeg_factory;
    std::uintptr_t resolve_aom_factory;
    std::uint64_t reserved;
};

static_assert(sizeof(DavinciAv1CallbackBindingsV1) == 56U);
static_assert(offsetof(DavinciAv1CallbackBindingsV1, resolve_config_getter) == 8U);
static_assert(offsetof(DavinciAv1CallbackBindingsV1, resolve_aom_factory) == 40U);

extern "C" {
DAVINCI_CODEC_RUNTIME_API unsigned int davinci_codec_runtime_abi_version() noexcept;
DAVINCI_CODEC_RUNTIME_API const char* davinci_codec_runtime_name() noexcept;

DAVINCI_CODEC_RUNTIME_API std::uint32_t
davinci_codec_runtime_av1_callback_abi_version() noexcept;

/* Returns 0 on success, -1 for invalid input, and -2 after prior configuration.
 * Resolve's target function addresses are validated by the session controller;
 * this API only copies the immutable bindings before the callback is published. */
DAVINCI_CODEC_RUNTIME_API int davinci_codec_runtime_configure_av1_callback(
    const DavinciAv1CallbackBindingsV1* bindings) noexcept;

/* Selects the software-only AV1 provider path when Resolve's setting requests it. */
DAVINCI_CODEC_RUNTIME_API void davinci_codec_runtime_set_av1_software_only(
    int software_only) noexcept;

/* Replacement for Resolve's AV1 decoder factory callback ABI. */
DAVINCI_CODEC_RUNTIME_API void* davinci_resolve_av1_decoder_callback(
    int operation, void* properties);
}
