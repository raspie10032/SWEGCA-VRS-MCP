#pragma once

#include "event_delta.hpp"
#include "graph_append.hpp"

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace swegca::vrs {

// One source Graph.append_many generation. The existing stable region
// directory remains attached; new nodes stay pending until consolidation.
// No signal settlement, region rebuild, or semantic promotion runs at ingress.
// SWEGCA: src/swegca_vrs2/store.py@c06092a:427-512
struct GraphBatchAppendPlan {
    std::string parent_snapshot_id;
    std::string snapshot_id;
    std::vector<std::pair<std::string, std::uint32_t>> new_nodes;
    EventDeltaChanges changes;
    Json receipt;
};

// The final unpublished memory view already contains all added episodes.
// Each episode sees nodes introduced by earlier episodes in this same batch.
// SWEGCA: src/swegca_vrs2/store.py@c06092a:427-512
[[nodiscard]] GraphBatchAppendPlan plan_graph_batch_append(
    std::span<const MemoryEpisode> episodes, std::string_view batch_snapshot,
    const HotIndexRead& memory, const GraphNodeDirectory& nodes,
    const ValidatedEventVrsInputs& current_inputs,
    std::string_view stable_version_id, std::uint64_t stable_edge_count,
    bool graph_substring_cues = false);

// Publish only after Main commits the original observation batch and matching
// pair. The detached numerical successor retains old scores and strengths.
// SWEGCA: src/swegca_vrs2/store.py@c06092a:479-509
[[nodiscard]] std::shared_ptr<const ValidatedEventVrsInputs>
prepare_graph_batch_event_delta(
    const GraphBatchAppendPlan& plan,
    std::shared_ptr<const ValidatedEventVrsInputs> parent);

}  // namespace swegca::vrs
