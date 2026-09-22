#pragma once

#include "connectivity_regions.hpp"
#include "graph_append.hpp"
#include "json.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace swegca::vrs {

// The old Graph.components directory is bound to the old immutable input
// generation. A missing entry means this node has not been in a region yet.
class GraphRegionDirectory {
public:
    virtual ~GraphRegionDirectory() = default;
    // SWEGCA: src/swegca_vrs2/store.py@7536139:282-291
    virtual void require_source(const EventVrsInputView& source) const = 0;
    // SWEGCA: src/swegca_vrs2/store.py@7536139:282-291
    [[nodiscard]] virtual std::optional<std::uint32_t> component_for(
        std::uint32_t node) const = 0;
    // SWEGCA: src/swegca_vrs2/store.py@7536139:299-302
    [[nodiscard]] virtual std::shared_ptr<const ConnectivityRegions> topology_for(
        std::uint32_t component) const = 0;
    // SWEGCA: src/swegca_vrs2/store.py@7536139:299-302
    [[nodiscard]] virtual std::uint32_t local_address(
        std::uint32_t component, std::uint32_t node) const = 0;
};

// A detached replacement of one affected component: remove old component
// identities, assign each source node to source.component_id, and insert its
// topology. Main must commit before making this patch visible to readers.
// SWEGCA: src/swegca_vrs2/store.py@7536139:282-298
struct GraphRegionPlan {
    std::shared_ptr<const AffectedGraphComponent> source;
    std::shared_ptr<const ConnectivityRegions> topology;
    std::vector<std::uint32_t> old_components;
    Json receipt;
};

// SWEGCA: src/swegca_vrs2/store.py@7536139:282-298
[[nodiscard]] GraphRegionPlan prepare_graph_regions(
    const MemoryEpisode& episode, const GraphAppendPlan& plan,
    const GraphNumericalCandidate& candidate,
    const ValidatedEventVrsInputs& parent,
    const GraphRegionDirectory& previous_regions);

// SWEGCA: src/swegca_vrs2/store.py@7536139:299-302
[[nodiscard]] std::vector<std::tuple<std::string, std::uint32_t, double>>
graph_memberships(std::string_view identifier, const EventVrsInputView& inputs,
                  const GraphNodeDirectory& nodes,
                  const GraphRegionDirectory& regions);

// SWEGCA: src/swegca_vrs2/store.py@7536139:304-306
[[nodiscard]] double graph_strength(std::string_view identifier,
                                    const EventVrsInputView& inputs,
                                    const GraphNodeDirectory& nodes);

}  // namespace swegca::vrs
