#pragma once

#include "deja_vu.hpp"
#include "graph_regions.hpp"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace swegca::vrs {

// A component-local, anonymous navigation signal. Multiple disconnected
// components retain separate topology identities and source bindings.
// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_portal_activation.py@c06092a:19-27
struct RegionPreactivation {
    std::string pair_snapshot_id;
    std::string topology_id;
    DejaVuSignal signal;
    std::vector<std::pair<std::uint32_t, double>> regions;
    std::uint32_t matched_term_count;
    bool memory_identifiers_exposed = false;
    bool grants_authority = false;
};

// Only exact cue addresses and membership weights are read. Call after the
// already-computed Déjà vu signal, before Recall or original Replay.
// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_portal_activation.py@c06092a:29-47
[[nodiscard]] RegionPreactivation preactivate_graph_regions(
    const FullCurrentMemoryVrsSnapshot& pair, const DejaVuSignal& signal,
    std::uint32_t component, const EventVrsInputView& inputs,
    const GraphNodeDirectory& nodes, const GraphRegionDirectory& directory);

}  // namespace swegca::vrs
