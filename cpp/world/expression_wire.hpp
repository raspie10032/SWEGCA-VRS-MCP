#pragma once

#include "world/cognitive_state.hpp"

#include <string_view>

namespace swegca::world {

inline constexpr std::string_view expression_wire_source_sha256 =
    "9861068a637e57918c81785bbcc8810da0603a3de6c447ff1f09339819c91380";

[[nodiscard]] JsonValue compact_expression(
    const JsonValue& packet, bool plain_answer = false,
    bool column_arrays = false);

}  // namespace swegca::world
