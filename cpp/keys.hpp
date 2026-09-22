#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace swegca::vrs {

// SWEGCA: src/swegca_vrs2/store.py@7536139:72-79
[[nodiscard]] std::vector<std::string> lexical_keys(std::string_view text);

// SWEGCA: src/swegca_vrs2/engine/mosaic_memory_activation.py@7536139:22-23
[[nodiscard]] std::string normalize_cue(std::string_view cue);

}  // namespace swegca::vrs
