#include "world/evidence_accumulator_bridge.hpp"

#include "world/semantic_vrs_ingress.hpp"

#include <fstream>
#include <stdexcept>

namespace swegca::world {
namespace {

template <typename State>
SynapseProposal gate(const State& world, const SynapseProposal& proposal,
                     const AccumulatorDecision& decision) {
    const auto shape = world.semantic_slots().shape();
    if (shape.empty()) throw std::invalid_argument("world semantic slots have no batch dimension");
    return sufficiency_gated_proposal(world, proposal,
        BooleanMask({shape.front()}, std::vector<std::uint8_t>(
            static_cast<std::size_t>(shape.front()),
            static_cast<std::uint8_t>(decision.status == "accept"))));
}

JsonValue::Array strings(const std::vector<std::string>& values) {
    JsonValue::Array result;
    for (const auto& value : values) result.emplace_back(value);
    return result;
}

}  // namespace

SynapseProposal accumulator_gated_proposal(
    const WorldState& world, const SynapseProposal& proposal,
    const AccumulatorDecision& decision) {
    return gate(world, proposal, decision);
}

SynapseProposal accumulator_gated_proposal(
    const CognitiveState& world, const SynapseProposal& proposal,
    const AccumulatorDecision& decision) {
    return gate(world, proposal, decision);
}

void append_accumulator_audit_record(
    const std::filesystem::path& path, const EvidenceObservation& observation,
    const AccumulatorUpdate& update, std::string world_hash_before,
    std::string world_hash_after, std::string accumulator_version,
    std::string arbiter_version) {
    if (!update.state || !update.previous_decision || !update.decision)
        throw std::invalid_argument("accumulator audit update is incomplete");
    if (!path.parent_path().empty())
        std::filesystem::create_directories(path.parent_path());
    double recent_sum = 0.0;
    for (const auto value : update.state->recent_outcomes()) recent_sum += value;
    JsonValue::Object record{
        {"world_hash_before", std::move(world_hash_before)},
        {"hypothesis_id", observation.hypothesis_id}, {"proposal_hash", update.proposal_hash},
        {"evidence_addresses", strings({observation.evidence_address})},
        {"source_family", observation.source_family}, {"source_address", observation.source_address},
        {"source_revision", observation.source_revision}, {"context_hash", observation.context_hash},
        {"producer_id", observation.producer_id}, {"outcome", observation.outcome},
        {"axis", observation.axis}, {"counterfactual_type", observation.axis},
        {"posterior_before", update.previous_decision->posterior_mean},
        {"posterior_after", update.decision->posterior_mean},
        {"short_horizon_state", update.state->recent_outcomes().empty()
            ? JsonValue(nullptr) : JsonValue(recent_sum / update.state->recent_outcomes().size())},
        {"long_horizon_state", update.decision->posterior_mean},
        {"causal_lower_bound", update.decision->causal_lower_bound},
        {"regime_change_score", update.decision->regime_change_score},
        {"decision", update.decision->status}, {"update_applied", update.applied},
        {"update_reason", update.reason}, {"world_hash_after", std::move(world_hash_after)},
        {"accumulator_version", std::move(accumulator_version)},
        {"arbiter_version", std::move(arbiter_version)},
        {"revision", JsonInteger{std::to_string(update.state->revision())}}};
    std::ofstream output(path, std::ios::binary | std::ios::app);
    if (!output) throw std::runtime_error("failed to open accumulator audit ledger");
    output << semantic_canonical_json(record) << '\n';
    output.flush();
    if (!output) throw std::runtime_error("failed to append accumulator audit ledger");
}

}  // namespace swegca::world
