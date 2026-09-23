#pragma once

#include "graph_regions.hpp"
#include "hot_index_pending.hpp"
#include "journal_frame.hpp"
#include "main_operations.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace swegca::vrs {

struct MainBatchRowResult {
    Json observation;
    std::optional<std::string> episode_id;
    std::optional<std::string> historical_pair_id;
    bool idempotent_replay;
    bool distinct_source_episode_added;
};

// One newly added original has already completed the author's whole Graph
// transition while still detached: append plan, event settlement and affected
// region preparation. journal_row_index binds it to the exact original row
// whose pair certificate names the settled graph generation.
// SWEGCA: src/swegca_vrs2/store.py@7536139:384-399
struct MainGraphRowTransition {
    std::size_t journal_row_index;
    std::string memory_snapshot_id;
    std::string pair_snapshot_id;
    GraphAppendPlan append;
    GraphNumericalCandidate numerical;
    GraphRegionPlan regions;
};

// Detached candidate only. Main must commit journal rows, derived directories,
// and the matching pair before making this generation visible to readers.
// SWEGCA: src/swegca_vrs2/store.py@7536139:371-404
struct MainObservationBatchPlan {
    std::string parent_pair_id;
    std::string memory_snapshot_id;
    std::string graph_snapshot_id;
    std::string pair_snapshot_id;
    std::vector<HotIndexAppendPlan> memory_additions;
    std::vector<MainGraphRowTransition> graph_transitions;
    std::vector<PendingJournalRow> journal_rows;
    std::vector<MainBatchRowResult> results;
};

// A physical commit may contain several rows, but each fresh observation
// follows the author's ingest transition in input order. Every added original
// completes HotIndex.append -> Graph.append settlement -> region preparation
// and receives that row's pair before the next row begins. Duplicate original
// content is still journaled with its unchanged current pair. No persistent
// state is changed here.
// SWEGCA: src/swegca_vrs2/store.py@7536139:371-404
[[nodiscard]] MainObservationBatchPlan plan_main_observation_batch(
    std::span<const Json> batch,
    const HotIndexRead& published_memory,
    const MainOperationRead& published_operations,
    const GraphNodeDirectory& nodes,
    const GraphRegionDirectory& regions,
    std::shared_ptr<const ValidatedEventVrsInputs> current_graph,
    std::string_view expected_parent_pair);

}  // namespace swegca::vrs
