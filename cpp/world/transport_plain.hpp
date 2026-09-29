#pragma once

#include "world/cognitive_state.hpp"

#include <string_view>

namespace swegca::world {

inline constexpr std::string_view transport_plain_source_sha256 =
    "a8f6acac8852023d3567428ff5848bebf266ddcde552d1574ffab78d8444437f";

[[nodiscard]] JsonValue transport_plain(const JsonValue& value);

}  // namespace swegca::world
