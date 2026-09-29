#pragma once

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace swegca::world {

inline constexpr std::string_view vrs_region_arrays_source_sha256 =
    "46b20ec5b36e02e39d8b4d93c94620ebad0193b38fe51d66e3103c8c7b1e6503";

struct RegionTermArrays final {
    std::vector<std::int64_t> offsets;
    std::vector<std::int64_t> nodes;

    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] std::span<const std::int64_t> operator[](
        std::int64_t index) const;
};

[[nodiscard]] RegionTermArrays reverse_memberships(
    const std::vector<std::int64_t>& offsets,
    const std::vector<std::int64_t>& groups,
    std::int64_t count);

}  // namespace swegca::world
