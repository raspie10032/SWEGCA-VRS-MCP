#pragma once

#include "event_vrs_inputs.hpp"
#include "hot_index.hpp"
#include "vrs_state_update.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace swegca::vrs {

// The old immutable graph generation supplies node addresses. A physical
// implementation must bind this directory to its EventVrsInputView.
class GraphNodeDirectory {
public:
    virtual ~GraphNodeDirectory() = default;
    // SWEGCA: src/swegca_vrs2/store.py@7536139:178-193
    virtual void require_source(const EventVrsInputView& source) const = 0;
    // SWEGCA: src/swegca_vrs2/store.py@7536139:196-202
    [[nodiscard]] virtual std::uint64_t node_count() const = 0;
    // SWEGCA: src/swegca_vrs2/store.py@7536139:196-202
    [[nodiscard]] virtual bool contains(std::string_view name) const = 0;
    // SWEGCA: src/swegca_vrs2/store.py@7536139:201-202
    [[nodiscard]] virtual std::uint32_t address(std::string_view name) const = 0;
};

// Detached inputs for prepare_event_delta and settle_event_signal. Main
// publishes no memory, graph or pair from this plan before journal commit.
// SWEGCA: src/swegca_vrs2/store.py@7536139:193-251
struct GraphAppendPlan {
    std::string parent_snapshot_id;
    std::string snapshot_id;
    std::vector<std::pair<std::string, std::uint32_t>> new_nodes;
    std::vector<float> appended_direct;
    std::vector<float> appended_score;
    std::vector<std::uint8_t> appended_unresolved;
    std::vector<EventEdge> appended_edges;
    std::vector<float> appended_strength;
    std::vector<std::uint32_t> changed_nodes;
    VRSStateUpdateReceipt strength_receipt;
};

// SWEGCA: src/swegca_vrs2/store.py@7536139:185-191
[[nodiscard]] std::string empty_graph_snapshot_id(std::string_view identity);

// The new episode is already in the unpublished HotIndex view. Old inputs and
// node addresses belong to the same pinned graph generation.
// SWEGCA: src/swegca_vrs2/store.py@7536139:193-251
[[nodiscard]] GraphAppendPlan plan_graph_append(
    const MemoryEpisode& episode, std::string snapshot_id,
    const HotIndexRead& memory, const GraphNodeDirectory& nodes,
    const ValidatedEventVrsInputs& current_inputs);

}  // namespace swegca::vrs
