#pragma once

#include "json.hpp"

namespace swegca::vrs {

// SWEGCA: src/swegca_vrs2/store.py@7536139:90-118
[[nodiscard]] Json observation(const Json& arguments);

}  // namespace swegca::vrs
