#include "builtins.hpp"

#include <array>

namespace davinci::codec {
namespace {

constexpr std::array providers{
    BackendProviderDefinition{"resolve.nvidia.decoder", CodecOperation::decode, BackendOwnership::resolve_owned, false},
    BackendProviderDefinition{"resolve.generic_ffmpeg.decoder", CodecOperation::decode, BackendOwnership::resolve_owned, true},
    BackendProviderDefinition{"resolve.aom.av1.decoder", CodecOperation::decode, BackendOwnership::resolve_owned, false}
};

}

std::span<const BackendProviderDefinition> builtin_backend_providers() noexcept {
    return providers;
}

}
