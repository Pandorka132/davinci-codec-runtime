#include <davinci_codec_runtime/codec_definition.hpp>

#include <array>
#include <cstdlib>
#include <cstdint>

using namespace davinci::codec;

void require(bool condition) {
    if (!condition) {
        std::abort();
    }
}

int main() {
    static_assert(fourcc('a', 'v', '0', '1') == 0x61763031U);

    const std::array aliases{fourcc('t', 'e', 's', 't')};
    const std::array decode_candidates{
        BackendCandidate{"test.hardware", "test.hardware_enabled", UnavailableAction::continue_to_next},
        BackendCandidate{"test.ffmpeg", "", UnavailableAction::continue_to_next},
        BackendCandidate{"test.software", "", UnavailableAction::stop}
    };
    const std::array providers{
        BackendProviderDefinition{"test.hardware", CodecOperation::decode, BackendOwnership::runtime_owned, false},
        BackendProviderDefinition{"test.ffmpeg", CodecOperation::decode, BackendOwnership::runtime_owned, true},
        BackendProviderDefinition{"test.software", CodecOperation::decode, BackendOwnership::resolve_owned, false}
    };
    const std::array conditions{RuntimeConditionDefinition{"test.hardware_enabled"}};
    const std::array definitions{
        CodecDefinition{"test", aliases, 226, BackendPlan{decode_candidates}, std::nullopt}
    };

    const auto valid = validate_definitions(definitions, providers, conditions);
    require(static_cast<bool>(valid));
    require(valid.error == DefinitionError::none);
    require(find_by_fourcc(definitions, fourcc('t', 'e', 's', 't')) == &definitions[0]);
    require(find_by_fourcc(definitions, fourcc('n', 'o', 'p', 'e')) == nullptr);

    const std::array missing_ffmpeg_id{
        CodecDefinition{"test", aliases, std::nullopt, BackendPlan{decode_candidates}, std::nullopt}
    };
    require(validate_definitions(missing_ffmpeg_id, providers, conditions).error == DefinitionError::missing_ffmpeg_codec_id);

    const std::array software_candidates{
        BackendCandidate{"test.software", "", UnavailableAction::stop}
    };
    const std::array duplicate_aliases{
        CodecDefinition{"test", aliases, 226, BackendPlan{software_candidates}, std::nullopt},
        CodecDefinition{"other", aliases, std::nullopt, BackendPlan{software_candidates}, std::nullopt}
    };
    require(validate_definitions(duplicate_aliases, providers, conditions).error == DefinitionError::duplicate_fourcc);

    const std::array unknown_provider_candidates{
        BackendCandidate{"missing.provider", "", UnavailableAction::stop}
    };
    const std::array unknown_provider_definitions{
        CodecDefinition{"test", aliases, 226, BackendPlan{unknown_provider_candidates}, std::nullopt}
    };
    require(validate_definitions(unknown_provider_definitions, providers, conditions).error == DefinitionError::unknown_provider);

    const std::array unknown_condition_candidates{
        BackendCandidate{"test.software", "not_registered", UnavailableAction::stop}
    };
    const std::array unknown_condition_definitions{
        CodecDefinition{"test", aliases, 226, BackendPlan{unknown_condition_candidates}, std::nullopt}
    };
    require(validate_definitions(unknown_condition_definitions, providers, conditions).error == DefinitionError::unknown_condition);

    const std::array no_operation_definitions{
        CodecDefinition{"test", aliases, 226, std::nullopt, std::nullopt}
    };
    require(validate_definitions(no_operation_definitions, providers, conditions).error == DefinitionError::no_operations);

    const std::array second_aliases{fourcc('t', 'e', 's', '2')};
    const std::array duplicate_ids{
        CodecDefinition{"same", aliases, std::nullopt, BackendPlan{software_candidates}, std::nullopt},
        CodecDefinition{"same", second_aliases, std::nullopt, BackendPlan{software_candidates}, std::nullopt}
    };
    require(validate_definitions(duplicate_ids, providers, conditions).error == DefinitionError::duplicate_codec_id);

    const std::array duplicate_providers{
        BackendProviderDefinition{"duplicate", CodecOperation::decode, BackendOwnership::runtime_owned, false},
        BackendProviderDefinition{"duplicate", CodecOperation::decode, BackendOwnership::resolve_owned, false}
    };
    require(validate_definitions(definitions, duplicate_providers, conditions).error == DefinitionError::duplicate_provider_id);

    const std::array duplicate_conditions{
        RuntimeConditionDefinition{"duplicate"},
        RuntimeConditionDefinition{"duplicate"}
    };
    require(validate_definitions(definitions, providers, duplicate_conditions).error == DefinitionError::duplicate_condition_id);

    const std::array empty_plan_definitions{
        CodecDefinition{"test", aliases, std::nullopt, BackendPlan{std::span<const BackendCandidate>{}}, std::nullopt}
    };
    require(validate_definitions(empty_plan_definitions, providers, conditions).error == DefinitionError::empty_backend_plan);

    const std::array invalid_ffmpeg_id_definitions{
        CodecDefinition{"test", aliases, 0, BackendPlan{software_candidates}, std::nullopt}
    };
    require(validate_definitions(invalid_ffmpeg_id_definitions, providers, conditions).error == DefinitionError::invalid_ffmpeg_codec_id);

    const std::array invalid_ownership_providers{
        BackendProviderDefinition{"invalid", CodecOperation::decode, static_cast<BackendOwnership>(99), false}
    };
    require(validate_definitions(definitions, invalid_ownership_providers, conditions).error == DefinitionError::invalid_backend_ownership);

    const std::array invalid_action_candidates{
        BackendCandidate{"test.software", "", static_cast<UnavailableAction>(99)}
    };
    const std::array invalid_action_definitions{
        CodecDefinition{"test", aliases, std::nullopt, BackendPlan{invalid_action_candidates}, std::nullopt}
    };
    require(validate_definitions(invalid_action_definitions, providers, conditions).error == DefinitionError::invalid_unavailable_action);

    const std::array encoder_only_candidates{
        BackendCandidate{"test.encoder", {}, UnavailableAction::stop}
    };
    const std::array encoder_only_providers{
        BackendProviderDefinition{"test.encoder", CodecOperation::encode, BackendOwnership::runtime_owned, false}
    };
    const std::array decode_using_encoder_provider{
        CodecDefinition{"test", aliases, std::nullopt, BackendPlan{encoder_only_candidates}, std::nullopt}
    };
    require(validate_definitions(decode_using_encoder_provider, encoder_only_providers, conditions).error == DefinitionError::provider_operation_mismatch);

    const std::array invalid_operation_providers{
        BackendProviderDefinition{"invalid", static_cast<CodecOperation>(99), BackendOwnership::runtime_owned, false}
    };
    require(validate_definitions(definitions, invalid_operation_providers, conditions).error == DefinitionError::invalid_backend_operation);

}
