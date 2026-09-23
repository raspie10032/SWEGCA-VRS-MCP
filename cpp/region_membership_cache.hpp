#pragma once

#include "event_vrs_inputs.hpp"
#include "graph_regions.hpp"
#include "hot_index.hpp"
#include "memory_vrs_pair.hpp"

#include <cstddef>
#include <cstdint>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace swegca::vrs {

struct CachedGraphMemberships {
    std::string topology_id;
    std::vector<std::pair<std::uint32_t, double>> memberships;
};

struct GraphMembershipCacheStatus {
    std::uint64_t entries;
    std::uint64_t estimated_bytes;
    std::uint64_t maximum_entries;
    std::uint64_t maximum_estimated_bytes;
    std::uint64_t hits;
    std::uint64_t misses;
    std::uint64_t evictions;
    std::uint64_t oversized;
    std::string pair_snapshot_id;
    std::string graph_snapshot_id;
    bool grants_authority = false;
};

// Main-owned memo of pure, derived episode membership. It is bound to one
// exact pair, numerical source, node directory and region directory. Eviction
// removes only derived tuples and never an original or evidence relation.
// SWEGCA: src/tinylm_slicer/mosaic_vrs_membership_cache.py@3bddcb7:14-74
class GraphMembershipCache {
public:
    GraphMembershipCache(
        std::shared_ptr<const FullCurrentMemoryVrsSnapshot> pair,
        std::shared_ptr<const ValidatedEventVrsInputs> inputs,
        std::shared_ptr<const GraphNodeDirectory> nodes,
        std::shared_ptr<const GraphRegionDirectory> regions,
        std::uint64_t maximum_entries,
        std::uint64_t maximum_estimated_bytes);

    // SWEGCA: src/tinylm_slicer/mosaic_vrs_membership_cache.py@3bddcb7:27-30
    void require(const FullCurrentMemoryVrsSnapshot& pair,
                 const EventVrsInputView& inputs,
                 const GraphNodeDirectory& nodes,
                 const GraphRegionDirectory& regions) const;

    // SWEGCA: src/tinylm_slicer/mosaic_vrs_membership_cache.py@3bddcb7:31-58
    [[nodiscard]] CachedGraphMemberships memberships(
        const HotIndexEpisodeHeader& episode,
        const FullCurrentMemoryVrsSnapshot& pair);

    // SWEGCA: src/tinylm_slicer/mosaic_vrs_membership_cache.py@3bddcb7:60-66
    [[nodiscard]] std::optional<SharedExperienceBridge> bridge(
        const HotIndexEpisodeHeader& episode,
        const FullCurrentMemoryVrsSnapshot& pair);

    // SWEGCA: src/tinylm_slicer/mosaic_vrs_membership_cache.py@3bddcb7:68-74
    [[nodiscard]] std::uint64_t maximum_entries() const {
        return maximum_entries_;
    }
    // SWEGCA: src/tinylm_slicer/mosaic_vrs_membership_cache.py@3bddcb7:68-74
    [[nodiscard]] std::uint64_t maximum_estimated_bytes() const {
        return maximum_estimated_bytes_;
    }
    // SWEGCA: src/tinylm_slicer/mosaic_vrs_membership_cache.py@3bddcb7:68-74
    [[nodiscard]] std::uint64_t estimated_bytes() const;
    // SWEGCA: src/tinylm_slicer/mosaic_vrs_membership_cache.py@3bddcb7:68-74
    [[nodiscard]] std::uint64_t entry_count() const;
    // The product graph is split into immutable components, so the source's
    // one topology_id becomes the exact graph generation binding here.
    // SWEGCA: src/tinylm_slicer/mosaic_vrs_membership_cache.py@3bddcb7:68-74
    [[nodiscard]] GraphMembershipCacheStatus status() const;

private:
    struct Key {
        std::string episode_id;
        std::string revision;
        std::vector<std::string> cues;
        [[nodiscard]] bool operator<(const Key& other) const;
    };
    struct Row {
        Key key;
        CachedGraphMemberships value;
        std::uint64_t charge = 0;
    };

    [[nodiscard]] CachedGraphMemberships derive(
        const HotIndexEpisodeHeader& episode,
        const FullCurrentMemoryVrsSnapshot& pair) const;
    [[nodiscard]] static std::uint64_t estimated_charge(const Row& row);

    std::shared_ptr<const FullCurrentMemoryVrsSnapshot> pair_;
    std::string graph_snapshot_id_;
    std::shared_ptr<const ValidatedEventVrsInputs> inputs_;
    std::shared_ptr<const GraphNodeDirectory> nodes_;
    std::shared_ptr<const GraphRegionDirectory> regions_;
    std::uint64_t maximum_entries_;
    std::uint64_t maximum_estimated_bytes_;
    mutable std::mutex mutex_;
    std::list<Row> rows_;
    std::map<Key, std::list<Row>::iterator> index_;
    std::uint64_t estimated_bytes_ = 0;
    std::uint64_t hits_ = 0;
    std::uint64_t misses_ = 0;
    std::uint64_t evictions_ = 0;
    std::uint64_t oversized_ = 0;
};

}  // namespace swegca::vrs
