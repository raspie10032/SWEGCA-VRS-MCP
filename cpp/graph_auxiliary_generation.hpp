#pragma once

#include "event_delta.hpp"
#include "hot_index.hpp"
#include "json.hpp"
#include "native_journal_entry.hpp"

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace swegca::vrs {

// Author Graph.with_aliases / with_usage keep topology, strengths, regions,
// and stable version, while chaining the Graph snapshot and receipt. This
// detached state must be published only with the unchanged numerical source
// rebound to its new snapshot ID and the matching Main pair certificate.
// SWEGCA: src/swegca_vrs2/store.py@c06092a:396-421
struct GraphAuxiliaryState {
    std::string snapshot_id;
    std::map<std::string, std::string> aliases;
    std::map<std::string, std::array<std::int64_t, 2>> usage;
    Json::Object last_receipt;
};

// SWEGCA: src/swegca_vrs2/store.py@c06092a:383-386
[[nodiscard]] GraphAuxiliaryState empty_graph_auxiliary_state(
    std::string_view identity);

// SWEGCA: src/swegca_vrs2/store.py@c06092a:396-410
[[nodiscard]] GraphAuxiliaryState graph_with_aliases(
    const GraphAuxiliaryState& current, std::string canonical,
    const std::vector<std::string>& aliases);

// SWEGCA: src/swegca_vrs2/store.py@c06092a:412-421
[[nodiscard]] GraphAuxiliaryState graph_with_usage(
    const GraphAuxiliaryState& current,
    const std::map<std::string, std::array<std::int64_t, 2>>& counts);

// This accepts only the two author events whose numeric arrays are unchanged.
// Consolidation follows the separate vrs_refine path and cannot enter here.
// SWEGCA: src/swegca_vrs2/store.py@c06092a:917-933
[[nodiscard]] GraphAuxiliaryState replay_graph_auxiliary_event(
    const GraphAuxiliaryState& current, const NativeJournalEntry& entry);

// SWEGCA: src/swegca_vrs2/store.py@c06092a:396-421
[[nodiscard]] std::shared_ptr<const ValidatedEventVrsInputs>
prepare_graph_auxiliary_inputs(
    std::shared_ptr<const ValidatedEventVrsInputs> parent,
    const GraphAuxiliaryState& current,
    const GraphAuxiliaryState& successor);

struct GraphAliasUpdatePlan {
    Json body;
    std::string fingerprint;
    std::string request_id;
    GraphAuxiliaryState successor;
    std::string pair_snapshot_id;
};

struct GraphUsageUpdatePlan {
    Json body;
    std::string fingerprint;
    std::string request_id;
    GraphAuxiliaryState successor;
    std::string pair_snapshot_id;
};

// Usage is source-bound provenance, never new evidence. Only a changed count
// for a source with a live original enters the journal candidate.
// SWEGCA: src/swegca_vrs2/store.py@c06092a:1291-1317
[[nodiscard]] std::optional<GraphUsageUpdatePlan> plan_graph_usage_update(
    const PublishedHotIndex& memory, const GraphAuxiliaryState& current,
    const std::map<std::string, std::array<std::int64_t, 2>>& counts);

// A declaration binds already known explicit proposition IDs. It is not
// new evidence or an action authority. A no-op returns no journal candidate.
// SWEGCA: src/swegca_vrs2/store.py@c06092a:1318-1347
[[nodiscard]] std::optional<GraphAliasUpdatePlan> plan_graph_alias_update(
    const HotIndexRead& memory, const GraphAuxiliaryState& current,
    std::string canonical, const std::vector<std::string>& aliases);

}  // namespace swegca::vrs
