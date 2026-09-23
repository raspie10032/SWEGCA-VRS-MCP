#pragma once

#include "swegca_vrs/allocation.hpp"
#include "swegca_vrs/core_sha256.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <utility>

// Bounded digest tree shared by experience blobs and Main-owned state
// tensors. This layer knows neither record addresses nor journal kinds.
// SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:23-60
namespace swegca::vrs::part_tree {

inline constexpr std::size_t part_bytes = 8u * 1024u * 1024u;
inline constexpr std::size_t inline_top_bytes = 2u * 1024u * 1024u;
inline constexpr std::uint64_t digests_per_part = part_bytes / digest256_width;
inline constexpr std::uint64_t max_top_digests = inline_top_bytes / digest256_width;
static_assert(part_bytes % digest256_width == 0);

struct PartLevels {
    std::array<std::uint64_t, 4> counts{};
    std::uint8_t depth = 0;
};

// SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:23-60
constexpr std::uint64_t ceil_div(std::uint64_t value, std::uint64_t by) noexcept {
    return value == 0 ? 0 : (value - 1) / by + 1;
}

// SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:23-60
constexpr PartLevels part_levels(std::uint64_t size) noexcept {
    PartLevels out;
    auto count = ceil_div(size, part_bytes);
    out.counts[out.depth++] = count;
    while (count > max_top_digests) {
        count = ceil_div(count, digests_per_part);
        out.counts[out.depth++] = count;
    }
    return out;
}
static_assert(ceil_div(ceil_div(ceil_div(std::numeric_limits<std::uint64_t>::max(), part_bytes),
                                digests_per_part),
                       digests_per_part) <= max_top_digests,
              "three part levels hold any u64 size");

// `owned.back()` starts as the level-0 digest list. Each emitted part views
// a list kept in `owned`; moving an inner vector retains its byte allocation.
// `emit` records each (digest, bytes) without assigning a record kind or
// address. The caller owns the level-0 input and all publication decisions.
// SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:23-60
template <class Owned, class Emit>
std::span<const std::byte> append_upper_levels(const AllocationContext& memory,
                                               const PartLevels& levels, Owned& owned, Emit&& emit) {
    std::span<const std::byte> below = owned.back();
    for (std::uint8_t level = 1; level < levels.depth; ++level) {
        using Bytes = typename Owned::value_type;
        Bytes upper(memory.allocator<std::byte>());
        upper.reserve(static_cast<std::size_t>(levels.counts[level] * digest256_width));
        for (std::uint64_t at = 0; at < levels.counts[level]; ++at) {
            const auto offset = at * part_bytes;
            const auto slice = below.subspan(static_cast<std::size_t>(offset),
                                             static_cast<std::size_t>(std::min<std::uint64_t>(
                                                 part_bytes, below.size() - offset)));
            const auto digest = Sha256::of(slice);
            upper.insert(upper.end(), digest.begin(), digest.end());
            emit(digest, slice);
        }
        owned.push_back(std::move(upper));
        below = owned.back();
    }
    return below;
}

}  // namespace swegca::vrs::part_tree
