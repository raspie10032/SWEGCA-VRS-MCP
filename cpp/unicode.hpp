#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace swegca::vrs {

// SWEGCA: src/swegca_vrs2/store.py@7536139:72-87
[[nodiscard]] std::vector<std::uint32_t> decode_utf8(std::string_view bytes);

// SWEGCA: src/swegca_vrs2/store.py@7536139:72-79
void append_utf8(std::string& target, std::uint32_t point);

// SWEGCA: src/swegca_vrs2/store.py@7536139:82-87
[[nodiscard]] bool python_space(std::uint32_t point);

// Python KeyError(str(identifier)) exposes repr(identifier), including quote
// selection and Unicode 15 nonprintable escapes.
[[nodiscard]] std::string python_key_error_text(std::string_view identifier);

}  // namespace swegca::vrs
