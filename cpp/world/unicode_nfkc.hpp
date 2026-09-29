#pragma once

#include <string>
#include <string_view>

namespace swegca::world {

// Exact Unicode 16.0.0 NFKC used by the pinned Python 3.14 source runtime.
// Invalid UTF-8 is rejected.
[[nodiscard]] std::string normalize_nfkc(std::string_view input);

[[nodiscard]] constexpr std::string_view unicode_nfkc_version() noexcept {
    return "16.0.0";
}

}  // namespace swegca::world
