#pragma once

#include "event_vrs_inputs.hpp"
#include "native_graph_page_map.hpp"
#include "native_region_binding_page.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

namespace swegca::vrs {

// One immutable page-map root for an exact numerical Graph snapshot. Every
// Graph node has a record; pending region work is represented by present=false
// at the value level rather than by a missing physical page.
// SWEGCA: src/swegca_vrs2/store.py@c06092a:487-510
struct NativeRegionBindingState {
    std::string journal_generation;
    std::string graph_snapshot_id;
    std::uint64_t node_count = 0;
    NativeGraphPageMap pages;
};

// Fixed-allocation direct-mapped cache for immutable region binding pages.
// The caller assigns a share of the global 4 GB budget explicitly.
// SWEGCA: user@2026-09-22:89-92
class NativeRegionBindingPageCache {
public:
    // SWEGCA: user@2026-09-22:89-92
    explicit NativeRegionBindingPageCache(std::uint64_t byte_budget);
    NativeRegionBindingPageCache(const NativeRegionBindingPageCache&) = delete;
    NativeRegionBindingPageCache& operator=(
        const NativeRegionBindingPageCache&) = delete;

    // SWEGCA: src/swegca_vrs2/store.py@c06092a:297-310
    [[nodiscard]] RegionBindingRecord record(
        std::shared_ptr<const NativeRegionBindingPageFile> file,
        std::uint64_t offset, std::uint32_t page_id,
        std::uint32_t record);

    // SWEGCA: user@2026-09-22:89-92
    [[nodiscard]] std::uint64_t resident_bytes() const {
        return resident_bytes_;
    }
    // SWEGCA: user@2026-09-22:89-92
    [[nodiscard]] std::uint64_t slot_count() const { return slot_count_; }

private:
    struct Slot {
        bool occupied = false;
        std::uint32_t page_id = 0;
        std::uint64_t offset = 0;
        std::shared_ptr<const NativeRegionBindingPageFile> file;
        NativeRegionBindingPage page;
    };

    struct Shard {
        std::mutex mutex;
        std::unique_ptr<Slot[]> slots;
        std::size_t count = 0;
    };

    static constexpr std::size_t shard_limit = 16;
    [[nodiscard]] std::pair<Shard*, std::size_t> locate(
        const NativeRegionBindingPageFile* file,
        std::uint64_t offset, std::uint32_t page_id);

    std::array<Shard, shard_limit> shards_;
    std::size_t active_shards_ = 0;
    std::uint64_t resident_bytes_ = 0;
    std::uint64_t slot_count_ = 0;
};

// Source-bound read view for the complete node binding array. It deliberately
// does not implement GraphRegionDirectory yet: component -> topology selection
// must come from the separately durable region manifest.
// SWEGCA: src/swegca_vrs2/store.py@c06092a:512-577
class NativeRegionBindingView {
public:
    NativeRegionBindingView(
        NativeRegionBindingState state,
        std::shared_ptr<const NativeRegionBindingPageFile> file,
        std::shared_ptr<NativeRegionBindingPageCache> cache);

    // SWEGCA: src/swegca_vrs2/store.py@c06092a:512-577
    void require_source(const EventVrsInputView& source) const;

    // Returns no value only for a source node explicitly pending region work.
    // SWEGCA: src/swegca_vrs2/store.py@c06092a:602-608
    [[nodiscard]] std::optional<RegionBindingRecord> binding(
        std::uint32_t node) const;

    // SWEGCA: src/swegca_vrs2/store.py@c06092a:512-577
    [[nodiscard]] const NativeRegionBindingState& state() const {
        return state_;
    }

private:
    void validate_complete_state() const;

    NativeRegionBindingState state_;
    std::shared_ptr<const NativeRegionBindingPageFile> file_;
    std::shared_ptr<NativeRegionBindingPageCache> cache_;
};

}  // namespace swegca::vrs
