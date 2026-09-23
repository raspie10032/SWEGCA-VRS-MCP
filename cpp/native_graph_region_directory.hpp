#pragma once

#include "graph_regions.hpp"
#include "native_region_binding_view.hpp"
#include "native_region_manifest.hpp"
#include "native_region_topology_catalog.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>
#include <utility>

namespace swegca::vrs {

// Fixed-allocation direct cache of already cold-validated immutable topology
// handles. It never changes component, local, region or membership values.
// SWEGCA: user@2026-09-22:89-92
class NativeRegionTopologyCache {
public:
    // byte_budget fixes the slot array allocation. The outer Main owner must
    // separately account for opened topology objects and allocator overhead.
    // SWEGCA: user@2026-09-22:89-92
    explicit NativeRegionTopologyCache(std::uint64_t byte_budget);
    NativeRegionTopologyCache(const NativeRegionTopologyCache&) = delete;
    NativeRegionTopologyCache& operator=(
        const NativeRegionTopologyCache&) = delete;

    // SWEGCA: src/swegca_vrs2/store.py@c06092a:599-608
    [[nodiscard]] std::shared_ptr<const NativeRegionTopologyFile> topology(
        std::shared_ptr<const NativeRegionTopologyCatalog> catalog,
        std::uint64_t offset, std::uint32_t component,
        std::string_view graph_snapshot_id);

    // SWEGCA: user@2026-09-22:89-92
    [[nodiscard]] std::uint64_t allocated_slot_bytes() const {
        return allocated_slot_bytes_;
    }
    // SWEGCA: user@2026-09-22:89-92
    [[nodiscard]] std::uint64_t slot_count() const { return slot_count_; }

private:
    struct Slot {
        bool occupied = false;
        std::uint32_t component = 0;
        std::uint64_t offset = 0;
        std::shared_ptr<const NativeRegionTopologyCatalog> catalog;
        std::shared_ptr<const NativeRegionTopologyFile> topology;
    };
    struct Shard {
        std::mutex mutex;
        std::unique_ptr<Slot[]> slots;
        std::size_t count = 0;
    };

    static constexpr std::size_t shard_limit = 16;
    [[nodiscard]] std::pair<Shard*, std::size_t> locate(
        const NativeRegionTopologyCatalog* catalog,
        std::uint64_t offset, std::uint32_t component);

    std::array<Shard, shard_limit> shards_;
    std::size_t active_shards_ = 0;
    std::uint64_t allocated_slot_bytes_ = 0;
    std::uint64_t slot_count_ = 0;
};

// Concrete source- and memory-bound region directory opened only from a
// recovered or freshly appended manifest cursor. Construction verifies every
// present node's exact component-local term address before the view escapes.
// SWEGCA: src/swegca_vrs2/store.py@c06092a:599-608
class NativeGraphRegionDirectory final : public GraphRegionDirectory {
public:
    [[nodiscard]] static std::shared_ptr<const NativeGraphRegionDirectory> open(
        NativeRegionManifestCursor publication,
        std::shared_ptr<const NativeRegionBindingPageFile> binding_file,
        std::shared_ptr<NativeRegionBindingPageCache> binding_cache,
        std::shared_ptr<const NativeRegionTopologyCatalog> catalog,
        std::shared_ptr<NativeRegionTopologyCache> topology_cache);

    // SWEGCA: src/swegca_vrs2/store.py@7536139:282-291
    void require_source(const EventVrsInputView& source) const override;
    // SWEGCA: src/swegca_vrs2/store.py@7536139:282-291
    [[nodiscard]] std::optional<std::uint32_t> component_for(
        std::uint32_t node) const override;
    // SWEGCA: src/swegca_vrs2/store.py@7536139:299-302
    [[nodiscard]] std::shared_ptr<const RegionTopologyView> topology_for(
        std::uint32_t component) const override;
    // SWEGCA: src/swegca_vrs2/store.py@7536139:299-302
    [[nodiscard]] std::uint32_t local_address(
        std::uint32_t component, std::uint32_t node) const override;
    // SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_connectivity_regions.py@7536139:235-248
    void require_memory_source(const PublishedHotIndex& memory) const override;

    [[nodiscard]] const NativeRegionManifestCursor& publication() const {
        return publication_;
    }

private:
    NativeGraphRegionDirectory(
        NativeRegionManifestCursor publication,
        std::shared_ptr<const NativeRegionBindingPageFile> binding_file,
        std::shared_ptr<NativeRegionBindingPageCache> binding_cache,
        std::shared_ptr<const NativeRegionTopologyCatalog> catalog,
        std::shared_ptr<NativeRegionTopologyCache> topology_cache);
    void validate_complete_directory() const;

    NativeRegionManifestCursor publication_;
    std::shared_ptr<const NativeRegionBindingView> bindings_;
    std::shared_ptr<const NativeRegionTopologyCatalog> catalog_;
    std::shared_ptr<NativeRegionTopologyCache> topology_cache_;
};

}  // namespace swegca::vrs
