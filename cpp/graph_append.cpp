#include "graph_append.hpp"

#include "digest.hpp"

#include <algorithm>
#include <limits>
#include <map>
#include <optional>
#include <stdexcept>
#include <utility>

namespace swegca::vrs {
namespace {

// SWEGCA: src/swegca_vrs2/store.py@7536139:214-220
std::optional<std::string> optional_observation_text(const Json& observation,
                                                      std::string_view key) {
    const auto& value = observation.at(key);
    if (std::holds_alternative<std::nullptr_t>(value.data)) return std::nullopt;
    return value.string();
}

// SWEGCA: src/swegca_vrs2/store.py@7536139:185-190
Json::Array empty_graph_identity(std::string_view identity) {
    Json::Array values;
    values.emplace_back(std::string("vrs2"));
    values.emplace_back(std::string(identity));
    return values;
}

}  // namespace

// SWEGCA: src/swegca_vrs2/store.py@7536139:185-191
std::string empty_graph_snapshot_id(std::string_view identity) {
    return sha256_hex(Json(empty_graph_identity(identity)).canonical());
}

// SWEGCA: src/swegca_vrs2/store.py@7536139:208-210
std::shared_ptr<const ValidatedEventVrsInputs> prepare_graph_event_delta(
    const GraphAppendPlan& plan,
    std::shared_ptr<const ValidatedEventVrsInputs> parent) {
    if (!parent || parent->require_validated_immutable().snapshot_id() != plan.parent_snapshot_id)
        throw std::runtime_error("graph append parent generation changed");
    EventDeltaChanges changes;
    changes.appended_direct = plan.appended_direct;
    changes.appended_score = plan.appended_score;
    changes.appended_unresolved = plan.appended_unresolved;
    changes.appended_edges = plan.appended_edges;
    changes.appended_strength = plan.appended_strength;
    return prepare_event_delta(std::move(parent), plan.snapshot_id, changes);
}

// SWEGCA: src/swegca_vrs2/store.py@7536139:250-262
GraphNumericalCandidate settle_graph_event(
    const GraphAppendPlan& plan,
    std::shared_ptr<const ValidatedEventVrsInputs> parent) {
    auto prepared = prepare_graph_event_delta(plan, std::move(parent));
    std::vector<std::int64_t> changed;
    changed.reserve(plan.changed_nodes.size());
    for (const auto node : plan.changed_nodes) changed.push_back(node);
    auto receipt = std::make_shared<const VRSStateUpdateReceipt>(plan.strength_receipt);
    auto signal = settle_event_signal(prepared, changed, std::move(receipt),
                                      "vrs-edge:", nullptr, 512);
    if (!signal.pending_nodes().empty())
        throw std::runtime_error("vrs_signal_pending_no_publication");
    EventDeltaChanges edits;
    Json::Array score_rows;
    for (const auto& [address, score] : signal.scores()) {
        edits.score_edits.emplace_back(address, score);
        Json::Array row;
        row.emplace_back(static_cast<std::int64_t>(address));
        row.emplace_back(static_cast<double>(score));
        score_rows.emplace_back(Json(std::move(row)));
    }
    for (const auto& [address, strength] : signal.strengths())
        edits.strength_edits.emplace_back(address, strength);
    Json::Array identity;
    identity.emplace_back(plan.snapshot_id);
    identity.emplace_back(std::string("settled"));
    identity.emplace_back(Json(std::move(score_rows)));
    auto settled = prepare_event_delta(
        std::move(prepared), sha256_hex(Json(std::move(identity)).canonical()), edits);
    return GraphNumericalCandidate{std::move(settled), std::move(signal)};
}

// SWEGCA: src/swegca_vrs2/store.py@7536139:193-251
GraphAppendPlan plan_graph_append(
    const MemoryEpisode& episode, std::string snapshot_id,
    const HotIndexRead& memory, const GraphNodeDirectory& nodes,
    const ValidatedEventVrsInputs& current_inputs) {
    const auto& graph = current_inputs.require_validated_immutable();
    nodes.require_source(graph);
    if (nodes.node_count() >= std::uint64_t{0x100000000ULL} ||
        nodes.node_count() != graph.node_count() || nodes.contains(episode.episode_id) ||
        !memory.contains_episode(episode.episode_id))
        throw std::runtime_error("graph append generation or episode changed");

    // Add the full original first, then only cue nodes absent from this graph.
    // Existing cue nodes remain shared literal associations.
    // SWEGCA: src/swegca_vrs2/store.py@7536139:196-203
    std::vector<std::pair<std::string, std::uint32_t>> new_nodes;
    new_nodes.emplace_back(episode.episode_id,
                           static_cast<std::uint32_t>(nodes.node_count()));
    std::map<std::string, std::uint32_t> new_addresses;
    new_addresses.emplace(episode.episode_id, new_nodes.front().second);
    for (const auto& cue : episode.cues) {
        const auto name = "cue:" + cue;
        if (!nodes.contains(name) && !new_addresses.contains(name)) {
            const auto address = nodes.node_count() + new_nodes.size();
            if (address > std::numeric_limits<std::uint32_t>::max())
                throw std::runtime_error("event delta node directory exceeds u32");
            new_nodes.emplace_back(name, static_cast<std::uint32_t>(address));
            new_addresses.emplace(name, static_cast<std::uint32_t>(address));
        }
    }
    if (nodes.node_count() + new_nodes.size() > std::uint64_t{0x100000000ULL})
        throw std::runtime_error("event delta node directory exceeds u32");
    const auto center = new_nodes.front().second;
    std::vector<std::uint32_t> endpoints;
    endpoints.reserve(episode.cues.size());
    for (const auto& cue : episode.cues) {
        const auto name = "cue:" + cue;
        const auto found = new_addresses.find(name);
        endpoints.push_back(found == new_addresses.end() ? nodes.address(name) : found->second);
    }
    if (episode.cues.size() >
        (std::numeric_limits<std::uint32_t>::max() - graph.edge_count()) / 2)
        throw std::runtime_error("event delta edge directory exceeds u32");
    std::vector<EventEdge> appended_edges;
    appended_edges.reserve(endpoints.size() * 2);
    for (const auto endpoint : endpoints) {
        appended_edges.push_back(EventEdge{center, endpoint, 1, 0.5f});
        appended_edges.push_back(EventEdge{endpoint, center, 1, 0.5f});
    }
    std::vector<float> direct(new_nodes.size(), 0.0f);
    direct.front() = 0.1f;
    std::vector<float> scores(new_nodes.size(), 0.0f);
    std::vector<std::uint8_t> unresolved(new_nodes.size(), 1);
    std::vector<float> strengths(appended_edges.size(), 0.5f);

    const auto& observation = episode.steps.at(0).observation;
    const auto proposition = optional_observation_text(observation, "proposition_id");
    const auto polarity = optional_observation_text(observation, "evidence_polarity");
    const auto replaced_id = optional_observation_text(observation, "supersedes");
    std::vector<HotIndexEpisodeHeader> prior;
    if (proposition) {
        for (const auto& identifier : memory.proposition_ids(*proposition)) {
            if (identifier != episode.episode_id && !memory.successor_of(identifier))
                prior.push_back(memory.episode_header(identifier));
        }
        if (replaced_id) {
            const auto replaced = memory.episode_header(*replaced_id);
            if (replaced.proposition_id == proposition) prior.push_back(replaced);
        }
    }
    std::sort(prior.begin(), prior.end(), [](const auto& left, const auto& right) {
        return left.episode_id < right.episode_id;
    });

    // Proposition and source/revision identity determine the fixed strength
    // proposal. Neither shared lexical cues nor an outcome label is evidence.
    // SWEGCA: src/swegca_vrs2/store.py@7536139:223-241
    std::vector<VRSConnectionStateUpdate> updates;
    for (const auto& old : prior) {
        const bool opposing = old.evidence_polarity != polarity;
        const bool retracted = opposing && old.episode_id == replaced_id;
        const bool conflict = opposing && !retracted;
        const bool duplicate_source = old.source_addresses == episode.source_addresses &&
                                      old.revision == episode.revision;
        const auto action = conflict ? "abstain_conflict" :
            duplicate_source ? "preserve_unresolved" :
            retracted ? "weaken" : "reinforce";
        const auto verdict = conflict ? "conflict" :
            duplicate_source ? "available" :
            retracted ? "refute" : "support";
        const auto old_node = nodes.address(old.episode_id);
        graph.dependencies().visit_edges(old_node, EndpointDirection::outgoing,
            [&](std::uint32_t edge) {
                const double previous = static_cast<double>(graph.strength(edge));
                const double current = conflict || duplicate_source ? previous :
                    previous * (retracted ? 0.995 : 1.01);
                const auto connection = "vrs-edge:" + std::to_string(edge);
                updates.emplace_back(
                    old.episode_id, connection, *proposition, verdict,
                    previous, current, action,
                    assess_vrs_experience_promotion(snapshot_id, connection,
                                                    previous, current),
                    true,
                    {{episode.episode_id, *proposition, verdict}});
            });
    }

    // Any unresolved opposing original prevents reinforcement of matching
    // originals in the same claim event. Preserve each source judgment.
    // SWEGCA: src/swegca_vrs2/store.py@7536139:242-249
    const bool any_conflict = std::any_of(updates.begin(), updates.end(), [](const auto& row) {
        return row.verdict == "conflict";
    });
    if (any_conflict) {
        std::vector<VRSConnectionStateUpdate> abstained;
        abstained.reserve(updates.size());
        for (const auto& row : updates) {
            abstained.emplace_back(
                row.episode_id, row.connection_id, row.proposition, "conflict",
                row.previous_strength, row.previous_strength, "abstain_conflict",
                assess_vrs_experience_promotion(snapshot_id, row.connection_id,
                                                row.previous_strength, row.previous_strength),
                row.underlying_experience_preserved, row.source_judgments);
        }
        updates.swap(abstained);
    }
    std::vector<std::uint32_t> changed_nodes;
    changed_nodes.reserve(endpoints.size() + 1);
    changed_nodes.push_back(center);
    changed_nodes.insert(changed_nodes.end(), endpoints.begin(), endpoints.end());
    return GraphAppendPlan{
        graph.snapshot_id(), snapshot_id, std::move(new_nodes), std::move(direct), std::move(scores),
        std::move(unresolved), std::move(appended_edges), std::move(strengths),
        std::move(changed_nodes), VRSStateUpdateReceipt(snapshot_id, std::move(updates))};
}

}  // namespace swegca::vrs
