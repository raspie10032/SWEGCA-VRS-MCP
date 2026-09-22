#include "graph_regions.hpp"

#include <algorithm>
#include <set>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace swegca::vrs {
namespace {

// SWEGCA: src/swegca_vrs2/store.py@7536139:41-47
Json promotion_plain(const VRSExperiencePromotionDecision& source) {
    Json::Object value;
    value.emplace("snapshot_id", Json(source.snapshot_id));
    value.emplace("connection_id", Json(source.connection_id));
    value.emplace("previous_strength", Json(source.previous_strength));
    value.emplace("current_strength", Json(source.current_strength));
    value.emplace("action", Json(source.action));
    value.emplace("promoted", Json(source.promoted));
    value.emplace("semantic_evidence_allowed", Json(source.semantic_evidence_allowed));
    value.emplace("underlying_experience_preserved", Json(source.underlying_experience_preserved));
    value.emplace("action_authorized", Json(source.action_authorized));
    value.emplace("persistent_write_authorized", Json(source.persistent_write_authorized));
    return Json(std::move(value));
}

// SWEGCA: src/swegca_vrs2/store.py@7536139:41-47
Json state_update_plain(const VRSConnectionStateUpdate& source) {
    Json::Object value;
    value.emplace("episode_id", Json(source.episode_id));
    value.emplace("connection_id", Json(source.connection_id));
    value.emplace("proposition", Json(source.proposition));
    value.emplace("verdict", Json(source.verdict));
    value.emplace("previous_strength", Json(source.previous_strength));
    value.emplace("current_strength", Json(source.current_strength));
    value.emplace("update_action", Json(source.update_action));
    value.emplace("promotion", promotion_plain(source.promotion));
    value.emplace("underlying_experience_preserved", Json(source.underlying_experience_preserved));
    Json::Array judgments;
    for (const auto& [episode, proposition, verdict] : source.source_judgments) {
        Json::Array row;
        row.emplace_back(episode);
        row.emplace_back(proposition);
        row.emplace_back(verdict);
        judgments.emplace_back(Json(std::move(row)));
    }
    value.emplace("source_judgments", Json(std::move(judgments)));
    return Json(std::move(value));
}

// SWEGCA: src/swegca_vrs2/store.py@7536139:41-47
Json state_receipt_plain(const VRSStateUpdateReceipt& source) {
    Json::Object value;
    value.emplace("snapshot_id", Json(source.snapshot_id));
    Json::Array updates;
    for (const auto& update : source.updates)
        updates.emplace_back(state_update_plain(update));
    value.emplace("updates", Json(std::move(updates)));
    Json::Array stages;
    for (const auto& stage : source.stage_order) stages.emplace_back(stage);
    value.emplace("stage_order", Json(std::move(stages)));
    value.emplace("detached_state_only", Json(source.detached_state_only));
    value.emplace("persistent_state_mutated", Json(source.persistent_state_mutated));
    value.emplace("action_authorized", Json(source.action_authorized));
    value.emplace("persistent_write_authorized", Json(source.persistent_write_authorized));
    value.emplace("semantic_promotion_authorized", Json(source.semantic_promotion_authorized));
    return Json(std::move(value));
}

}  // namespace

// SWEGCA: src/swegca_vrs2/store.py@7536139:282-298
GraphRegionPlan prepare_graph_regions(
    const MemoryEpisode& episode, const GraphAppendPlan& plan,
    const GraphNumericalCandidate& candidate,
    const ValidatedEventVrsInputs& parent,
    const GraphRegionDirectory& previous_regions) {
    const auto& previous = parent.require_validated_immutable();
    if (previous.snapshot_id() != plan.parent_snapshot_id ||
        !candidate.settled || candidate.event_snapshot_id != plan.snapshot_id ||
        plan.new_nodes.empty() || plan.new_nodes.front().first != episode.episode_id)
        throw std::runtime_error("graph region generation changed");
    previous_regions.require_source(previous);
    auto source = std::make_shared<const AffectedGraphComponent>(
        affected_graph_component(plan, candidate));
    auto topology = std::make_shared<const ConnectivityRegions>(
        ConnectivityRegions::build(
            source, candidate.settled->require_validated_immutable().snapshot_id()));
    if (!topology->converged())
        throw std::runtime_error("region_topology_pending_no_publication");
    std::set<std::uint32_t> obsolete;
    for (const auto node : source->nodes) {
        const auto old = previous_regions.component_for(node);
        if (old) obsolete.insert(*old);
    }
    auto receipt = candidate.signal.receipt().object();
    receipt.insert_or_assign("changed_component_nodes",
                             Json(static_cast<std::int64_t>(source->nodes.size())));
    receipt.insert_or_assign("changed_component_edges",
                             Json(static_cast<std::int64_t>(source->edges.size())));
    receipt.insert_or_assign("global_recomputation_reason", Json(std::string(
        "Only affected connected component: modularity equivalence cannot be guaranteed by local moves alone.")));
    receipt.insert_or_assign("source_episode_count_added", Json(std::int64_t{1}));
    receipt.insert_or_assign("historical_outcome", Json(episode.steps.at(0).outcome));
    receipt.insert_or_assign("re_evidence_updates", state_receipt_plain(plan.strength_receipt));
    receipt.insert_or_assign("recorded_agreement_is_not_independent_factual_corroboration", Json(true));
    receipt.insert_or_assign("logical_implication_claimed", Json(false));
    receipt.insert_or_assign("grants_authority", Json(false));
    return GraphRegionPlan{std::move(source), std::move(topology),
                           std::vector<std::uint32_t>(obsolete.begin(), obsolete.end()),
                           Json(std::move(receipt))};
}

// SWEGCA: src/swegca_vrs2/store.py@7536139:299-302
std::vector<std::tuple<std::string, std::uint32_t, double>>
graph_memberships(std::string_view identifier, const EventVrsInputView& inputs,
                  const GraphNodeDirectory& nodes,
                  const GraphRegionDirectory& regions) {
    nodes.require_source(inputs);
    regions.require_source(inputs);
    const auto address = nodes.address(identifier);
    const auto component = regions.component_for(address);
    if (!component) throw std::runtime_error("graph node has no component");
    const auto topology = regions.topology_for(*component);
    if (!topology || topology->vrs_snapshot_id() != inputs.snapshot_id())
        throw std::runtime_error("region topology belongs to a different VRS generation");
    const auto local = regions.local_address(*component, address);
    if (local >= topology->terms().size() || topology->terms()[local] != address)
        throw std::runtime_error("region node address changed");
    std::vector<std::tuple<std::string, std::uint32_t, double>> result;
    for (const auto& [group, weight] : topology->memberships_for_term(local))
        result.emplace_back(topology->topology_id(), group, weight);
    return result;
}

// SWEGCA: src/swegca_vrs2/store.py@7536139:304-306
double graph_strength(std::string_view identifier,
                      const EventVrsInputView& inputs,
                      const GraphNodeDirectory& nodes) {
    nodes.require_source(inputs);
    const auto address = nodes.address(identifier);
    double strength = 0;
    inputs.dependencies().visit_edges(address, EndpointDirection::outgoing,
        [&](std::uint32_t edge) {
            strength = std::max(strength, static_cast<double>(inputs.strength(edge)));
        });
    return strength;
}

}  // namespace swegca::vrs
