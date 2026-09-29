#pragma once

#include "world/memory_activation.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace swegca::world {

inline constexpr std::string_view hot_memory_step_source_sha256 =
    "d1343c6d390785a6644c4deb6d29b435d6476a32d33997dc4281a91bc69d024f";

[[nodiscard]] MemoryStep make_hot_memory_step(
    std::string phase, JsonValue::Object observation,
    std::vector<std::string> relations, std::string judgment,
    std::string outcome, std::vector<std::string> evidence_refs);

}  // namespace swegca::world
