#pragma once

#include <davinci_codec_runtime/codec_definition.hpp>

namespace davinci::codec {

std::span<const CodecDefinition> builtin_codec_definitions() noexcept;
std::span<const BackendProviderDefinition> builtin_backend_providers() noexcept;
std::span<const RuntimeConditionDefinition> builtin_runtime_conditions() noexcept;

}
