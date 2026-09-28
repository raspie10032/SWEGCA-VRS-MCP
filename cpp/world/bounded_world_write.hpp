#pragma once

#include "world/cognitive_state.hpp"
#include "world/evidence_accumulator.hpp"
#include "world/evidence_revision.hpp"
#include "world/synapse_arbiter.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace swegca::world {

class BoundedWorldWriteConfig final {
public:
    BoundedWorldWriteConfig(double minimum_causal_lower_bound = 0.55,
                            std::size_t minimum_source_diversity = 2,
                            std::size_t minimum_context_diversity = 4,
                            double maximum_slot_delta = 0.02,
                            double minimum_proposal_weight = 0.5);

    const double minimum_causal_lower_bound;
    const std::size_t minimum_source_diversity;
    const std::size_t minimum_context_diversity;
    const double maximum_slot_delta;
    const double minimum_proposal_weight;
};

class WorldWriteGates final {
public:
    // Public construction carries policy data only and cannot mint write authority.
    WorldWriteGates(std::string evidence_status, double causal_lower_bound,
                    std::size_t source_diversity, std::size_t context_diversity,
                    bool definitions_complete, bool counterfactual_support,
                    bool intervention_support, bool regime_change_suspected,
                    bool slot_gate_passed, bool device_gate_passed,
                    bool capacity_strategy_safe, bool evidence_current,
                    bool accumulator_revision_current, bool runtime_context_safe);

    const std::string evidence_status;
    const double causal_lower_bound;
    const std::size_t source_diversity;
    const std::size_t context_diversity;
    const bool definitions_complete;
    const bool counterfactual_support;
    const bool intervention_support;
    const bool regime_change_suspected;
    const bool slot_gate_passed;
    const bool device_gate_passed;
    const bool capacity_strategy_safe;
    const bool evidence_current;
    const bool accumulator_revision_current;
    const bool runtime_context_safe;

private:
    WorldWriteGates(std::string evidence_status, double causal_lower_bound,
                    std::size_t source_diversity, std::size_t context_diversity,
                    bool definitions_complete, bool counterfactual_support,
                    bool intervention_support, bool regime_change_suspected,
                    bool slot_gate_passed, bool device_gate_passed,
                    bool capacity_strategy_safe, bool evidence_current,
                    bool accumulator_revision_current, bool runtime_context_safe,
                    std::shared_ptr<const void> authority,
                    std::string authority_digest,
                    std::string proposal_binding_digest);

    const std::shared_ptr<const void> authority_;
    const std::string authority_digest_;
    const std::string proposal_binding_digest_;

    friend WorldWriteGates world_write_gates_from_decision(
        const AccumulatorDecision&, const SynapseProposal&, bool, bool, bool,
        bool, bool, bool, bool, bool, bool, bool,
        const EvidenceRevisionVerification*);
    friend class WorldWriteAccess;
};

[[nodiscard]] WorldWriteGates world_write_gates_from_decision(
    const AccumulatorDecision& decision, const SynapseProposal& proposal,
    bool definitions_complete, bool counterfactual_support,
    bool intervention_support, bool regime_change_suspected,
    bool slot_gate_passed, bool device_gate_passed,
    bool capacity_strategy_safe, bool evidence_current,
    bool accumulator_revision_current, bool runtime_context_safe,
    const EvidenceRevisionVerification* revision_verification = nullptr);

class BoundedWorldWriteReceipt final {
public:
    BoundedWorldWriteReceipt(
        std::string receipt_id, std::int64_t revision, std::string target_role,
        std::string before_state_hash, std::string after_state_hash,
        Tensor before_slot, std::string before_slot_hash,
        std::string after_slot_hash, std::string applied_delta_hash,
        std::vector<std::string> evidence_refs,
        std::optional<JsonValue::Object> prior_write_metadata,
        std::string hypothesis_id = {},
        std::string proposal_binding_digest = {});

    const std::string receipt_id;
    const std::int64_t revision;
    const std::string target_role;
    const std::string before_state_hash;
    const std::string after_state_hash;
    const Tensor before_slot;
    const std::string before_slot_hash;
    const std::string after_slot_hash;
    const std::string applied_delta_hash;
    const std::vector<std::string> evidence_refs;
    const std::optional<JsonValue::Object> prior_write_metadata;
    const std::string hypothesis_id;
    const std::string proposal_binding_digest;
};

class BoundedWorldWriteResult final {
public:
    BoundedWorldWriteResult(std::shared_ptr<const CognitiveState> state,
                            bool authorized, bool committed, std::string reason,
                            Tensor proposed_delta,
                            std::shared_ptr<const BoundedWorldWriteReceipt> receipt = {});

    const std::shared_ptr<const CognitiveState> state;
    const bool authorized;
    const bool committed;
    const std::string reason;
    const Tensor proposed_delta;
    const std::shared_ptr<const BoundedWorldWriteReceipt> receipt;
};

[[nodiscard]] std::string cognitive_state_hash(const CognitiveState& state);

[[nodiscard]] BoundedWorldWriteResult bounded_verification_write(
    std::shared_ptr<const CognitiveState> state,
    const SynapseProposal& proposal, const WorldWriteGates& gates,
    const BoundedWorldWriteConfig& config, bool commit);

}  // namespace swegca::world
