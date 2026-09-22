#include "main_observation_batch.hpp"

#include "digest.hpp"
#include "memory_episode.hpp"
#include "memory_vrs_pair.hpp"
#include "observation.hpp"

#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace swegca::vrs {
namespace {

struct FreshObservation {
    std::size_t result_position;
    std::string request_id;
    std::string fingerprint;
    std::string episode_id;
    bool added;
};

// SWEGCA: src/swegca_vrs2/store.py@c06092a:1416-1426
std::string fold_graph_snapshot(std::string_view prior,
                                std::string_view fingerprint) {
    Json::Array parts;
    parts.emplace_back(std::string(prior));
    parts.emplace_back(std::string(fingerprint));
    return sha256_hex(Json(std::move(parts)).canonical());
}

}  // namespace

// SWEGCA: src/swegca_vrs2/store.py@c06092a:1389-1460
MainObservationBatchPlan plan_main_observation_batch(
    std::span<const Json> batch, const HotIndexRead& published_memory,
    const MainOperationRead& published_operations,
    const GraphNodeDirectory& nodes,
    const ValidatedEventVrsInputs& current_graph,
    std::string_view expected_parent_pair,
    std::string_view stable_version_id, std::uint64_t stable_edge_count,
    bool graph_substring_cues) {
    if (batch.empty())
        throw std::runtime_error("append_many_needs_at_least_one_episode");
    const auto parent_graph = current_graph.require_validated_immutable().snapshot_id();
    if (full_current_pair_snapshot_id(published_memory.snapshot_id(),
                                      parent_graph) != expected_parent_pair)
        throw std::runtime_error("main_batch_parent_pair_changed");
    std::vector<Json> rows;
    std::vector<std::string> fingerprints;
    rows.reserve(batch.size());
    fingerprints.reserve(batch.size());
    for (const auto& arguments : batch) {
        auto row = observation(arguments);
        fingerprints.push_back(sha256_hex(row.canonical()));
        rows.push_back(std::move(row));
    }
    HotIndexPending memory(published_memory);
    MainOperationPending operations(published_operations);
    std::map<std::string, std::string> seen;
    std::vector<MemoryEpisode> added_episodes;
    std::vector<FreshObservation> fresh;
    std::string graph_input_snapshot = parent_graph;
    MainObservationBatchPlan plan;
    plan.parent_pair_id = std::string(expected_parent_pair);
    plan.results.reserve(rows.size());
    for (std::size_t at = 0; at < rows.size(); ++at) {
        const auto request_id = rows[at].at("request_id").string();
        const auto& fingerprint = fingerprints[at];
        const auto historical = operations.check(request_id, fingerprint);
        if (historical) {
            std::optional<std::string> historical_id;
            if (!historical->episode_id.empty())
                historical_id = historical->episode_id;
            plan.results.push_back(MainBatchRowResult{
                rows[at], std::move(historical_id),
                std::optional<std::string>{historical->pair_snapshot_id},
                true, false});
            continue;
        }
        if (const auto prior = seen.find(request_id); prior != seen.end()) {
            if (prior->second != fingerprint)
                throw std::runtime_error(
                    "request_id_reused_with_different_content");
            plan.results.push_back(MainBatchRowResult{
                rows[at], std::nullopt, std::nullopt, true, false});
            continue;
        }
        seen.emplace(request_id, fingerprint);
        const auto [identifier, added] = memory.append(rows[at]);
        if (added) {
            added_episodes.push_back(episode_from_observation(rows[at]));
            if (added_episodes.back().episode_id != identifier)
                throw std::runtime_error("main_batch_episode_changed");
            graph_input_snapshot = fold_graph_snapshot(
                graph_input_snapshot, fingerprint);
        }
        const auto result_position = plan.results.size();
        plan.results.push_back(MainBatchRowResult{
            rows[at], std::nullopt, std::nullopt, false, added});
        fresh.push_back(FreshObservation{
            result_position, request_id, fingerprint, identifier, added});
    }
    plan.memory_snapshot_id = memory.snapshot_id();
    plan.graph_snapshot_id = parent_graph;
    plan.memory_additions = memory.plans();
    if (plan.memory_additions.size() != added_episodes.size())
        throw std::runtime_error("main_batch_memory_plan_changed");
    if (!added_episodes.empty()) {
        plan.graph.emplace(plan_graph_batch_append(
            added_episodes, graph_input_snapshot, memory, nodes,
            current_graph, stable_version_id, stable_edge_count,
            graph_substring_cues));
        plan.graph_snapshot_id = plan.graph->snapshot_id;
    }
    plan.pair_snapshot_id = full_current_pair_snapshot_id(
        plan.memory_snapshot_id, plan.graph_snapshot_id);
    plan.journal_rows.reserve(fresh.size());
    for (const auto& item : fresh) {
        operations.stage(item.request_id, MainOperation{
            item.fingerprint, item.episode_id, plan.pair_snapshot_id});
        const auto& row = plan.results[item.result_position].observation;
        plan.journal_rows.push_back(PendingJournalRow{
            item.request_id, row.canonical(), item.fingerprint,
            plan.pair_snapshot_id});
        auto& result = plan.results[item.result_position];
        result.episode_id = item.episode_id;
        result.historical_pair_id = plan.pair_snapshot_id;
    }
    return plan;
}

}  // namespace swegca::vrs
