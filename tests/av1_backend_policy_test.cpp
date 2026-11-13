#include "builtins.hpp"

#include <array>
#include <cstdlib>
#include <string_view>
#include <utility>

using namespace davinci::codec;

void require(bool condition) {
    if (!condition) {
        std::abort();
    }
}

int main() {
    const auto definitions = builtin_codec_definitions();
    const auto providers = builtin_backend_providers();
    const auto conditions = builtin_runtime_conditions();

    require(static_cast<bool>(validate_definitions(definitions, providers, conditions)));
    require(definitions.size() == 1);
    require(providers.size() == 3);
    require(conditions.size() == 1);
    require(providers[0].ownership == BackendOwnership::resolve_owned);
    require(providers[1].requires_ffmpeg_codec_id);
    require(conditions[0].id == "resolve.av1.hardware_decode_allowed");

    const auto& av1 = definitions[0];
    require(av1.id == "av1");
    require(av1.fourcc_aliases.size() == 1);
    require(av1.fourcc_aliases[0] == fourcc('a', 'v', '0', '1'));
    require(find_by_fourcc(definitions, fourcc('a', 'v', '0', '1')) == &av1);
    require(av1.ffmpeg_codec_id == 226);
    require(av1.encoder == std::nullopt);

    const auto& candidates = av1.decoder->candidates;
    require(candidates.size() == 3);
    require(candidates[0].provider_id == "resolve.nvidia.decoder");
    require(candidates[0].condition_id == "resolve.av1.hardware_decode_allowed");
    require(candidates[1].provider_id == "resolve.generic_ffmpeg.decoder");
    require(candidates[1].condition_id.empty());
    require(candidates[2].provider_id == "resolve.aom.av1.decoder");
    require(candidates[2].condition_id.empty());

    const auto run_plan = [&](std::string_view software_setting, std::string_view available_provider,
                              std::array<std::string_view, 3>& attempts) {
        std::size_t attempt_count = 0;
        const auto result = dispatch_backend_plan(
            *av1.decoder,
            [&](std::string_view condition_id) {
                require(condition_id == "resolve.av1.hardware_decode_allowed");
                return software_setting != "1";
            },
            [&](std::string_view provider_id) {
                attempts[attempt_count++] = provider_id;
                return provider_id == available_provider ? ProviderAttempt::created : ProviderAttempt::unavailable;
            });
        return std::pair{result, attempt_count};
    };

    std::array<std::string_view, 3> attempts{};
    auto [hardware_result, hardware_attempts] = run_plan("0", "resolve.nvidia.decoder", attempts);
    require(hardware_result.status == BackendDispatchStatus::created);
    require(hardware_result.provider_id == "resolve.nvidia.decoder");
    require(hardware_attempts == 1);

    attempts = {};
    auto [unset_result, unset_attempts] = run_plan("", "resolve.nvidia.decoder", attempts);
    require(unset_result.status == BackendDispatchStatus::created);
    require(unset_attempts == 1);

    attempts = {};
    auto [noncanonical_value_result, noncanonical_value_attempts] = run_plan("true", "resolve.nvidia.decoder", attempts);
    require(noncanonical_value_result.status == BackendDispatchStatus::created);
    require(noncanonical_value_attempts == 1);

    attempts = {};
    auto [custom_result, custom_attempts] = run_plan("0", "resolve.generic_ffmpeg.decoder", attempts);
    require(custom_result.status == BackendDispatchStatus::created);
    require(custom_result.provider_id == "resolve.generic_ffmpeg.decoder");
    require(custom_attempts == 2);
    require(attempts[0] == "resolve.nvidia.decoder");
    require(attempts[1] == "resolve.generic_ffmpeg.decoder");

    attempts = {};
    auto [aom_fallback_result, aom_fallback_attempts] = run_plan("0", "resolve.aom.av1.decoder", attempts);
    require(aom_fallback_result.status == BackendDispatchStatus::created);
    require(aom_fallback_result.provider_id == "resolve.aom.av1.decoder");
    require(aom_fallback_attempts == 3);
    require(attempts[0] == "resolve.nvidia.decoder");
    require(attempts[1] == "resolve.generic_ffmpeg.decoder");
    require(attempts[2] == "resolve.aom.av1.decoder");

    attempts = {};
    auto [forced_software_result, forced_software_attempts] = run_plan("1", "resolve.generic_ffmpeg.decoder", attempts);
    require(forced_software_result.status == BackendDispatchStatus::created);
    require(forced_software_result.provider_id == "resolve.generic_ffmpeg.decoder");
    require(forced_software_attempts == 1);
    require(attempts[0] == "resolve.generic_ffmpeg.decoder");

    attempts = {};
    auto [forced_software_fallback_result, forced_software_fallback_attempts] = run_plan("1", "resolve.aom.av1.decoder", attempts);
    require(forced_software_fallback_result.status == BackendDispatchStatus::created);
    require(forced_software_fallback_result.provider_id == "resolve.aom.av1.decoder");
    require(forced_software_fallback_attempts == 2);
    require(attempts[0] == "resolve.generic_ffmpeg.decoder");
    require(attempts[1] == "resolve.aom.av1.decoder");

    attempts = {};
    auto [normal_exhausted_result, normal_exhausted_attempts] = run_plan("0", "unavailable", attempts);
    require(normal_exhausted_result.status == BackendDispatchStatus::no_candidate);
    require(normal_exhausted_result.provider_id.empty());
    require(normal_exhausted_attempts == 3);
    require(attempts[0] == "resolve.nvidia.decoder");
    require(attempts[1] == "resolve.generic_ffmpeg.decoder");
    require(attempts[2] == "resolve.aom.av1.decoder");

    attempts = {};
    auto [software_exhausted_result, software_exhausted_attempts] = run_plan("1", "unavailable", attempts);
    require(software_exhausted_result.status == BackendDispatchStatus::no_candidate);
    require(software_exhausted_result.provider_id.empty());
    require(software_exhausted_attempts == 2);
    require(attempts[0] == "resolve.generic_ffmpeg.decoder");
    require(attempts[1] == "resolve.aom.av1.decoder");

    const auto invalid_status = dispatch_backend_plan(
        *av1.decoder,
        [](std::string_view) { return true; },
        [](std::string_view) { return static_cast<ProviderAttempt>(99); });
    require(invalid_status.status == BackendDispatchStatus::stopped);
    require(invalid_status.provider_id == "resolve.nvidia.decoder");
}
