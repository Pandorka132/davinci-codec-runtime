#include <davinci_codec_runtime/api.hpp>

#include <atomic>
#include <cstdint>

extern "C" void davinci_codec_runtime_set_av1_software_only(
    int software_only) noexcept __attribute__((weak));

namespace {

using ResolveConfigGetter = void *(*)();
using ResolveSoftwareOnlyHelper = std::uint8_t (*)(void *);
using ResolveAv1Factory = void *(*)(int, void *);

struct ResolveTargets {
    ResolveConfigGetter config_getter;
    ResolveSoftwareOnlyHelper software_only_helper;
    ResolveAv1Factory nvidia_factory;
    ResolveAv1Factory generic_ffmpeg_factory;
    ResolveAv1Factory aom_factory;
};

constexpr std::uint32_t configuration_empty = 0U;
constexpr std::uint32_t configuration_writing = 1U;
constexpr std::uint32_t configuration_ready = 2U;

ResolveTargets g_resolve_targets{};
std::atomic<std::uint32_t> g_configuration_state{configuration_empty};

bool valid_bindings(const DavinciAv1CallbackBindingsV1 *bindings) noexcept {
    return bindings != nullptr &&
           bindings->abi_version == DAVINCI_AV1_CALLBACK_ABI_VERSION &&
           bindings->struct_size == sizeof(DavinciAv1CallbackBindingsV1) &&
           bindings->reserved == 0U &&
           bindings->resolve_config_getter != 0U &&
           bindings->resolve_software_only_helper != 0U &&
           bindings->resolve_nvidia_factory != 0U &&
           bindings->resolve_generic_ffmpeg_factory != 0U &&
           bindings->resolve_aom_factory != 0U;
}

} // namespace

extern "C" DAVINCI_CODEC_RUNTIME_API std::uint32_t
davinci_codec_runtime_av1_callback_abi_version() noexcept {
    return DAVINCI_AV1_CALLBACK_ABI_VERSION;
}

extern "C" DAVINCI_CODEC_RUNTIME_API int
davinci_codec_runtime_configure_av1_callback(
    const DavinciAv1CallbackBindingsV1 *bindings) noexcept {
    if (!valid_bindings(bindings)) return -1;

    std::uint32_t expected = configuration_empty;
    if (!g_configuration_state.compare_exchange_strong(
            expected, configuration_writing, std::memory_order_acq_rel,
            std::memory_order_acquire))
        return -2;

    g_resolve_targets = {
        reinterpret_cast<ResolveConfigGetter>(
            bindings->resolve_config_getter),
        reinterpret_cast<ResolveSoftwareOnlyHelper>(
            bindings->resolve_software_only_helper),
        reinterpret_cast<ResolveAv1Factory>(
            bindings->resolve_nvidia_factory),
        reinterpret_cast<ResolveAv1Factory>(
            bindings->resolve_generic_ffmpeg_factory),
        reinterpret_cast<ResolveAv1Factory>(
            bindings->resolve_aom_factory)
    };

    g_configuration_state.store(configuration_ready,
                                 std::memory_order_release);
    return 0;
}

extern "C" DAVINCI_CODEC_RUNTIME_API void *
davinci_resolve_av1_decoder_callback(int operation, void *properties) {
    if (operation != 1) return nullptr;
    if (g_configuration_state.load(std::memory_order_acquire) !=
        configuration_ready)
        return nullptr;

    // Preserve Resolve's own lazy singleton prelude and cached exact-"1" test.
    void *configuration = g_resolve_targets.config_getter();
    const bool software_only =
        g_resolve_targets.software_only_helper(configuration) != 0U;
    if (davinci_codec_runtime_set_av1_software_only != nullptr)
        davinci_codec_runtime_set_av1_software_only(software_only ? 1 : 0);

    if (!software_only) {
        if (void *nvidia =
                g_resolve_targets.nvidia_factory(1, properties))
            return nvidia;
    }

    if (void *custom =
            g_resolve_targets.generic_ffmpeg_factory(1, properties))
        return custom;

    return g_resolve_targets.aom_factory(1, properties);
}
