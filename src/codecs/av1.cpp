#include "builtins.hpp"

#include <array>

namespace davinci::codec {
namespace {

constexpr auto av1_fourcc = fourcc('a', 'v', '0', '1');
constexpr std::int32_t av1_ffmpeg_codec_id = 226;

constexpr std::array av1_aliases{av1_fourcc};

constexpr std::array av1_decoder_candidates{
    BackendCandidate{
        "resolve.nvidia.decoder",
        "resolve.av1.hardware_decode_allowed",
        UnavailableAction::continue_to_next
    },
    BackendCandidate{
        "resolve.generic_ffmpeg.decoder",
        {},
        UnavailableAction::continue_to_next
    },
    BackendCandidate{
        "resolve.aom.av1.decoder",
        {},
        UnavailableAction::continue_to_next
    }
};

constexpr std::array av1_definitions{
    CodecDefinition{
        "av1",
        av1_aliases,
        av1_ffmpeg_codec_id,
        BackendPlan{av1_decoder_candidates},
        std::nullopt
    }
};

}

std::span<const CodecDefinition> builtin_codec_definitions() noexcept {
    return av1_definitions;
}

}
