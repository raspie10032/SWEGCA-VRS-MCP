#pragma once

#include "graph_batch_append.hpp"
#include "hot_index_pending.hpp"
#include "journal_frame.hpp"
#include "main_operations.hpp"

#include <cstdint>
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

// Detached candidate only. Main must commit journal rows, derived directories,
// and the matching pair before making this generation visible to readers.
// SWEGCA: src/swegca_vrs2/store.py@c06092a:1389-1460
struct MainObservationBatchPlan {
    std::string parent_pair_id;
    std::string memory_snapshot_id;
    std::string graph_snapshot_id;
    std::string pair_snapshot_id;
    std::vector<HotIndexAppendPlan> memory_additions;
    std::optional<GraphBatchAppendPlan> graph;
    std::vector<PendingJournalRow> journal_rows;
    std::vector<MainBatchRowResult> results;
};

// Normalize the complete batch, reject conflicting request-ID reuse, append
// each fresh observation to one unpublished memory view, fold only new
// original fingerprints into Graph.append_many, then certify one pair for
// every fresh journal row. No persistent state is changed here.
// SWEGCA: src/swegca_vrs2/store.py@c06092a:1389-1460
[[nodiscard]] MainObservationBatchPlan plan_main_observation_batch(
    std::span<const Json> batch,
    const HotIndexRead& published_memory,
    const MainOperationRead& published_operations,
    const GraphNodeDirectory& nodes,
    const ValidatedEventVrsInputs& current_graph,
    std::string_view expected_parent_pair,
    std::string_view stable_version_id,
    std::uint64_t stable_edge_count,
    bool graph_substring_cues = false);

}  // namespace swegca::vrs
