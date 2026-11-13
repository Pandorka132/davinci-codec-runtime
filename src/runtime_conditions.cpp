#include "builtins.hpp"

#include <array>

namespace davinci::codec {
namespace {

constexpr std::array conditions{
    RuntimeConditionDefinition{"resolve.av1.hardware_decode_allowed"}
};

}

std::span<const RuntimeConditionDefinition> builtin_runtime_conditions() noexcept {
    return conditions;
}

}
