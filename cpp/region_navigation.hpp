#pragma once

#include "graph_regions.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace swegca::vrs {

// SWEGCA: src/tinylm_slicer/mosaic_vrs_local_navigation.py@3bddcb7:13-21
struct RegionNavigationCursor {
    SharedExperienceBridge bridge;
    std::uint32_t origin_region;
    std::uint32_t destination_region;
    std::vector<std::uint32_t> seed_nodes;
    std::uint64_t seed_offset = 0;
    std::uint64_t region_offset = 0;
    std::uint64_t emitted_terms = 0;
};

// Term IDs are component-local node addresses. Only literal cue nodes enter
// navigation_cues; whole-original graph nodes remain addressable elsewhere.
// SWEGCA: src/tinylm_slicer/mosaic_vrs_local_navigation.py@3bddcb7:24-34
struct RegionNavigationPage {
    RegionNavigationCursor cursor;
    std::optional<RegionNavigationCursor> next_cursor;
    std::vector<std::uint32_t> term_ids;
    std::vector<std::string> cues;
    std::uint64_t examined_terms;
    std::uint64_t remaining_terms;
    bool destination_complete;
    bool complete_memory_search = false;
    bool grants_authority = false;
};

// SWEGCA: src/tinylm_slicer/mosaic_vrs_local_navigation.py@3bddcb7:37-59
[[nodiscard]] RegionNavigationCursor start_graph_region_navigation(
    const FullCurrentMemoryVrsSnapshot& pair, const EventVrsInputView& inputs,
    const GraphNodeDirectory& nodes, const GraphRegionDirectory& regions,
    const SharedExperienceBridge& bridge, std::uint32_t origin_region,
    std::uint32_t destination_region);

// SWEGCA: src/tinylm_slicer/mosaic_vrs_local_navigation.py@3bddcb7:62-107
[[nodiscard]] RegionNavigationPage next_graph_region_cues(
    const FullCurrentMemoryVrsSnapshot& pair, const EventVrsInputView& inputs,
    const GraphNodeDirectory& nodes, const GraphRegionDirectory& regions,
    const RegionNavigationCursor& cursor, std::uint64_t work_budget);

}  // namespace swegca::vrs
