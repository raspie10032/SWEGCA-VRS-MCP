#pragma once

#include "deja_vu.hpp"
#include "hot_index.hpp"

#include <string>
#include <utility>
#include <vector>

namespace swegca::vrs {

struct RecallCandidate {
    std::string episode_id;
    std::vector<std::string> matched_cues;
    double cue_overlap;
    std::string revision;
    std::string verification_state;
    std::vector<std::string> historical_outcomes;
};

class RecallResult {
public:
    const std::string query;
    const std::vector<RecallCandidate> candidates;
    const std::string snapshot_id;
    const bool codex_per_item_allowlist_used;
    const bool action_authorized;
    const bool persistent_write_authorized;
    const std::vector<std::pair<std::string, std::string>> source_dependencies;

    // SWEGCA: src/swegca_vrs2/engine/mosaic_memory_activation.py@7536139:280-298
    RecallResult(std::string query, std::vector<RecallCandidate> candidates,
                 std::string snapshot_id,
                 std::vector<std::pair<std::string, std::string>> source_dependencies = {},
                 bool codex_per_item_allowlist_used = false,
                 bool action_authorized = false,
                 bool persistent_write_authorized = false);
};

// Logical author Recall over a pinned published generation. The complete
// candidate set and source-family closure are retained; no top-K is applied.
// This source-level implementation is not yet the bounded physical read path.
// SWEGCA: src/swegca_vrs2/engine/mosaic_memory_activation.py@7536139:301-348
[[nodiscard]] RecallResult recall_memory(const PublishedHotIndex& index,
                                         const DejaVuSignal& signal,
                                         const std::vector<std::string>& navigation_cues = {});

// Keep the full candidate set in the caller's memory_selection. The receipt
// gets only the first active original, or the first address if all are
// superseded, as in the author product path.
// SWEGCA: src/swegca_vrs2/store.py@7536139:421-427
// SWEGCA: user@2026-09-22:24-25
[[nodiscard]] RecallResult select_replay_original(const PublishedHotIndex& index,
                                                  const RecallResult& complete);

}  // namespace swegca::vrs
