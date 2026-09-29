#include "world/vrs_region_arrays.hpp"

#include <algorithm>
#include <numeric>
#include <stdexcept>

namespace swegca::world {

std::size_t RegionTermArrays::size() const noexcept {
    return offsets.empty() ? 0 : offsets.size() - 1;
}

std::span<const std::int64_t> RegionTermArrays::operator[](
    std::int64_t index) const {
    if (index < 0) index += static_cast<std::int64_t>(size());
    if (index < 0 || static_cast<std::size_t>(index) >= size())
        throw std::out_of_range("region index outside source");
    const auto lo = offsets[static_cast<std::size_t>(index)];
    const auto hi = offsets[static_cast<std::size_t>(index) + 1];
    return std::span<const std::int64_t>(nodes).subspan(
        static_cast<std::size_t>(lo), static_cast<std::size_t>(hi - lo));
}

RegionTermArrays reverse_memberships(const std::vector<std::int64_t>& offsets,
                                     const std::vector<std::int64_t>& groups,
                                     const std::int64_t count) {
    if (offsets.empty() || count < 0)
        throw std::invalid_argument("invalid region membership arrays");
    std::vector<std::int64_t> source_nodes;
    for (std::size_t node = 0; node + 1 < offsets.size(); ++node) {
        const auto width = offsets[node + 1] - offsets[node];
        if (width < 0) throw std::invalid_argument("invalid region membership offsets");
        source_nodes.insert(source_nodes.end(), static_cast<std::size_t>(width),
                            static_cast<std::int64_t>(node));
    }
    if (source_nodes.size() != groups.size())
        throw std::invalid_argument("region membership size mismatch");
    std::vector<std::size_t> order(groups.size());
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(), [&](const auto left, const auto right) {
        return groups[left] < groups[right];
    });
    std::vector<std::int64_t> reverse_offsets(static_cast<std::size_t>(count) + 1, 0);
    for (const auto group : groups) {
        if (group < 0 || group >= count)
            throw std::invalid_argument("region membership group outside range");
        ++reverse_offsets[static_cast<std::size_t>(group) + 1];
    }
    std::partial_sum(reverse_offsets.begin(), reverse_offsets.end(), reverse_offsets.begin());
    std::vector<std::int64_t> nodes;
    nodes.reserve(source_nodes.size());
    for (const auto index : order) nodes.push_back(source_nodes[index]);
    return {std::move(reverse_offsets), std::move(nodes)};
}

}  // namespace swegca::world
