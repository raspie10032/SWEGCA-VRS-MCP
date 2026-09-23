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
// SWEGCA: src/tinylm_slicer/mosaic_vrs_portal_activation.py@3bddcb7:19-27
struct RegionPreactivation {
    std::string pair_snapshot_id;
    std::string topology_id;
    std::uint32_t component;
    DejaVuSignal signal;
    std::vector<std::pair<std::uint32_t, double>> raw_region_masses;
    double global_mass;
    std::vector<std::pair<std::uint32_t, double>> regions;
    std::uint32_t matched_term_count;
    bool memory_identifiers_exposed = false;
    bool grants_authority = false;
};

// Only exact cue addresses and membership weights are read. This component
// step returns raw masses; the portal prefix combines every component with
// one author fsum before filling normalized regions. Call after the already
// computed Déjà vu signal, before Recall or original Replay.
// SWEGCA: src/tinylm_slicer/mosaic_vrs_portal_activation.py@3bddcb7:29-47
[[nodiscard]] RegionPreactivation preactivate_graph_region_masses(
    const FullCurrentMemoryVrsSnapshot& pair, const DejaVuSignal& signal,
    std::uint32_t component, const EventVrsInputView& inputs,
    const GraphNodeDirectory& nodes, const GraphRegionDirectory& directory);

}  // namespace swegca::vrs
