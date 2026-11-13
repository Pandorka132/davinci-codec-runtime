#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace davinci::codec {

constexpr std::uint32_t fourcc(char a, char b, char c, char d) noexcept {
    return (static_cast<std::uint32_t>(static_cast<unsigned char>(a)) << 24U) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(b)) << 16U) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(c)) << 8U) |
           static_cast<std::uint32_t>(static_cast<unsigned char>(d));
}

enum class UnavailableAction : std::uint8_t {
    continue_to_next,
    stop
};

struct BackendCandidate {
    std::string_view provider_id;
    std::string_view condition_id;
    UnavailableAction on_unavailable;
};

struct BackendPlan {
    std::span<const BackendCandidate> candidates;
};

enum class ProviderAttempt : std::uint8_t {
    created,
    unavailable
};

enum class BackendDispatchStatus : std::uint8_t {
    created,
    no_candidate,
    stopped
};

struct BackendDispatchResult {
    BackendDispatchStatus status;
    std::string_view provider_id;
    std::size_t candidate_index;
};

template <typename ConditionEvaluator, typename ProviderInvoker>
constexpr BackendDispatchResult dispatch_backend_plan(
    const BackendPlan& plan,
    ConditionEvaluator&& condition_enabled,
    ProviderInvoker&& invoke_provider) {
    for (std::size_t i = 0; i < plan.candidates.size(); ++i) {
        const auto& candidate = plan.candidates[i];
        if (!candidate.condition_id.empty() && !condition_enabled(candidate.condition_id)) {
            continue;
        }

        const auto attempt = invoke_provider(candidate.provider_id);
        if (attempt == ProviderAttempt::created) {
            return {BackendDispatchStatus::created, candidate.provider_id, i};
        }
        if (attempt != ProviderAttempt::unavailable) {
            return {BackendDispatchStatus::stopped, candidate.provider_id, i};
        }
        if (candidate.on_unavailable == UnavailableAction::stop) {
            return {BackendDispatchStatus::stopped, candidate.provider_id, i};
        }
    }

    return {BackendDispatchStatus::no_candidate, {}, plan.candidates.size()};
}

struct CodecDefinition {
    std::string_view id;
    std::span<const std::uint32_t> fourcc_aliases;
    std::optional<std::int32_t> ffmpeg_codec_id;
    std::optional<BackendPlan> decoder;
    std::optional<BackendPlan> encoder;
};

enum class CodecOperation : std::uint8_t {
    decode,
    encode
};

enum class BackendOwnership : std::uint8_t {
    resolve_owned,
    runtime_owned
};

struct BackendProviderDefinition {
    std::string_view id;
    CodecOperation operation;
    BackendOwnership ownership;
    bool requires_ffmpeg_codec_id;
};

struct RuntimeConditionDefinition {
    std::string_view id;
};

enum class DefinitionError : std::uint8_t {
    none,
    empty_codec_id,
    duplicate_codec_id,
    missing_fourcc,
    duplicate_fourcc,
    empty_backend_plan,
    empty_provider_id,
    unknown_provider,
    missing_ffmpeg_codec_id,
    invalid_ffmpeg_codec_id,
    no_operations,
    duplicate_provider_id,
    empty_condition_id,
    duplicate_condition_id,
    unknown_condition,
    invalid_unavailable_action,
    invalid_backend_ownership,
    invalid_backend_operation,
    provider_operation_mismatch
};

struct ValidationResult {
    DefinitionError error;
    std::size_t definition_index;
    std::size_t item_index;

    constexpr explicit operator bool() const noexcept {
        return error == DefinitionError::none;
    }
};

constexpr const BackendProviderDefinition* find_provider(
    std::span<const BackendProviderDefinition> providers,
    std::string_view id) noexcept {
    for (const auto& provider : providers) {
        if (provider.id == id) {
            return &provider;
        }
    }
    return nullptr;
}

constexpr ValidationResult validate_definitions(
    std::span<const CodecDefinition> definitions,
    std::span<const BackendProviderDefinition> providers,
    std::span<const RuntimeConditionDefinition> conditions = {}) noexcept {
    for (std::size_t i = 0; i < providers.size(); ++i) {
        if (providers[i].id.empty()) {
            return {DefinitionError::empty_provider_id, 0, i};
        }
        if (providers[i].operation != CodecOperation::decode &&
            providers[i].operation != CodecOperation::encode) {
            return {DefinitionError::invalid_backend_operation, 0, i};
        }
        if (providers[i].ownership != BackendOwnership::resolve_owned &&
            providers[i].ownership != BackendOwnership::runtime_owned) {
            return {DefinitionError::invalid_backend_ownership, 0, i};
        }
        for (std::size_t previous = 0; previous < i; ++previous) {
            if (providers[previous].id == providers[i].id) {
                return {DefinitionError::duplicate_provider_id, 0, i};
            }
        }
    }
    for (std::size_t i = 0; i < conditions.size(); ++i) {
        if (conditions[i].id.empty()) {
            return {DefinitionError::empty_condition_id, 0, i};
        }
        for (std::size_t previous = 0; previous < i; ++previous) {
            if (conditions[previous].id == conditions[i].id) {
                return {DefinitionError::duplicate_condition_id, 0, i};
            }
        }
    }
    for (std::size_t i = 0; i < definitions.size(); ++i) {
        const auto& definition = definitions[i];
        if (definition.id.empty()) {
            return {DefinitionError::empty_codec_id, i, 0};
        }
        for (std::size_t previous = 0; previous < i; ++previous) {
            if (definitions[previous].id == definition.id) {
                return {DefinitionError::duplicate_codec_id, i, previous};
            }
        }
        if (definition.fourcc_aliases.empty()) {
            return {DefinitionError::missing_fourcc, i, 0};
        }
        if (!definition.decoder && !definition.encoder) {
            return {DefinitionError::no_operations, i, 0};
        }
        if (definition.ffmpeg_codec_id && *definition.ffmpeg_codec_id <= 0) {
            return {DefinitionError::invalid_ffmpeg_codec_id, i, 0};
        }
        for (std::size_t alias = 0; alias < definition.fourcc_aliases.size(); ++alias) {
            for (std::size_t previous_definition = 0; previous_definition <= i; ++previous_definition) {
                const auto& previous_aliases = definitions[previous_definition].fourcc_aliases;
                const std::size_t limit = previous_definition == i ? alias : previous_aliases.size();
                for (std::size_t previous_alias = 0; previous_alias < limit; ++previous_alias) {
                    if (previous_aliases[previous_alias] == definition.fourcc_aliases[alias]) {
                        return {DefinitionError::duplicate_fourcc, i, alias};
                    }
                }
            }
        }

        const auto validate_plan = [&](const std::optional<BackendPlan>& plan, CodecOperation operation) constexpr -> ValidationResult {
            if (!plan) {
                return {DefinitionError::none, i, 0};
            }
            if (plan->candidates.empty()) {
                return {DefinitionError::empty_backend_plan, i, 0};
            }
            for (std::size_t candidate_index = 0; candidate_index < plan->candidates.size(); ++candidate_index) {
                const auto& candidate = plan->candidates[candidate_index];
                if (candidate.on_unavailable != UnavailableAction::continue_to_next &&
                    candidate.on_unavailable != UnavailableAction::stop) {
                    return {DefinitionError::invalid_unavailable_action, i, candidate_index};
                }
                if (candidate.provider_id.empty()) {
                    return {DefinitionError::empty_provider_id, i, candidate_index};
                }
                const auto* provider = find_provider(providers, candidate.provider_id);
                if (provider == nullptr) {
                    return {DefinitionError::unknown_provider, i, candidate_index};
                }
                if (provider->operation != operation) {
                    return {DefinitionError::provider_operation_mismatch, i, candidate_index};
                }
                if (provider->requires_ffmpeg_codec_id && !definition.ffmpeg_codec_id) {
                    return {DefinitionError::missing_ffmpeg_codec_id, i, candidate_index};
                }
                if (!candidate.condition_id.empty()) {
                    bool found_condition = false;
                    for (const auto& condition : conditions) {
                        found_condition = found_condition || condition.id == candidate.condition_id;
                    }
                    if (!found_condition) {
                        return {DefinitionError::unknown_condition, i, candidate_index};
                    }
                }
            }
            return {DefinitionError::none, i, 0};
        };

        const auto decoder_result = validate_plan(definition.decoder, CodecOperation::decode);
        if (!decoder_result) {
            return decoder_result;
        }
        const auto encoder_result = validate_plan(definition.encoder, CodecOperation::encode);
        if (!encoder_result) {
            return encoder_result;
        }
    }
    return {DefinitionError::none, 0, 0};
}

constexpr const CodecDefinition* find_by_fourcc(
    std::span<const CodecDefinition> definitions,
    std::uint32_t tag) noexcept {
    for (const auto& definition : definitions) {
        for (const auto alias : definition.fourcc_aliases) {
            if (alias == tag) {
                return &definition;
            }
        }
    }
    return nullptr;
}

}
