#pragma once

#include "graph_regions.hpp"
#include "memory_receipt.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace swegca::vrs {

// Each observed original retains its own component topology identity. One
// activation is still one hyperedge even when its originals span components.
// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_coactivation.py@c06092a:80-104
struct CoactivatedExperience {
    std::string episode_id;
    std::string revision;
    std::vector<std::string> source_addresses;
    std::vector<std::string> outcomes;
    std::optional<std::string> topology_id;
    std::vector<std::pair<std::uint32_t, double>> memberships;
    std::string proposition;
    std::string current_verdict;
};

// Source event topology_id is single-valued for a generic topology. The Graph
// product may have multiple disconnected component topologies, so a mixed
// event keeps this optional field empty and binds each original separately.
// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_coactivation.py@c06092a:92-104
struct CoactivationEvent {
    std::string request_id;
    std::int64_t observed_at_ns;
    std::string pair_snapshot_id;
    std::string memory_snapshot_id;
    std::string vrs_snapshot_id;
    std::optional<std::string> topology_id;
    std::string query;
    std::vector<std::string> current_cues;
    std::vector<CoactivatedExperience> experiences;
    bool should_abstain;
    bool grants_authority = false;
};

// Prepared observation only. Main must recheck the pinned generation and
// durably record it before any derived coactivation index can publish it.
// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_coactivation.py@c06092a:107-158
[[nodiscard]] CoactivationEvent prepare_coactivation_event(
    const FullCurrentMemoryVrsSnapshot& pair,
    const MemoryActivationReceipt& activation, std::string request_id,
    std::int64_t observed_at_ns, const EventVrsInputView& inputs,
    const GraphNodeDirectory& nodes, const GraphRegionDirectory& regions);

}  // namespace swegca::vrs
