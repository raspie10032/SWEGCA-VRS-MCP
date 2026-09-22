#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <utility>

namespace swegca::vrs {

// A node or edge page contains 256 contiguous numerical records. A page ID
// therefore uses 24 bits for the author's u32 record address space. The
// three-level directory has 256 entries per level; updating one page copies
// only its root, middle, and leaf. Pinned readers retain old roots while Main
// prepares a successor. Zero is reserved for an absent physical page.
// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_delta.py@7536139:23-27
class NativeGraphPageMap {
public:
    static constexpr std::uint32_t records_per_page = 256;
    static constexpr std::uint32_t maximum_page_id = (1u << 24) - 1;

    // SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_delta.py@7536139:84-94
    [[nodiscard]] std::optional<std::uint64_t> offset(
        std::uint32_t page_id) const;

    // Updates must be strictly increasing by page ID. All offsets identify
    // already written, immutable physical pages; publication is separate.
    // SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_delta.py@7536139:23-27
    [[nodiscard]] NativeGraphPageMap with_updates(
        std::span<const std::pair<std::uint32_t, std::uint64_t>> updates) const;

    // Streaming checkpoint preparation does not materialize a second map.
    // SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_delta.py@7536139:84-94
    void visit(const std::function<void(std::uint32_t, std::uint64_t)>& emit) const;

    // SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_delta.py@7536139:84-94
    [[nodiscard]] std::uint64_t page_count() const { return page_count_; }

private:
    using Leaf = std::array<std::uint64_t, 256>;
    struct Middle {
        std::array<std::shared_ptr<const Leaf>, 256> leaves{};
    };
    struct Root {
        std::array<std::shared_ptr<const Middle>, 256> middles{};
    };

    std::shared_ptr<const Root> root_;
    std::uint64_t page_count_ = 0;
};

}  // namespace swegca::vrs
