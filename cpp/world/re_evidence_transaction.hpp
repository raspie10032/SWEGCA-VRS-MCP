#pragma once

#include "world/cognitive_event.hpp"
#include "world/evidence_accumulator.hpp"
#include "world/re_evidence_receipt.hpp"
#include "world/synapse_arbiter.hpp"

#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace swegca::world {

class ExperienceVectorBinding final {
public:
    ExperienceVectorBinding(std::string source_address,
                            std::string source_revision,
                            std::vector<std::string> evidence_addresses,
                            std::vector<double> values);

    const std::string source_address;
    const std::string source_revision;
    const std::vector<std::string> evidence_addresses;
    const std::vector<double> values;
};

[[nodiscard]] SynapseProposal
experience_delta_proposal_from_accepted_observations(
    std::shared_ptr<const WorldState> world,
    const AccumulatorDecision& decision,
    std::span<const EvidenceObservation> observations,
    std::span<const ExperienceVectorBinding> vectors,
    std::string source);

[[nodiscard]] SynapseProposal
experience_delta_proposal_from_accepted_observations(
    std::shared_ptr<const CognitiveState> world,
    const AccumulatorDecision& decision,
    std::span<const EvidenceObservation> observations,
    std::span<const ExperienceVectorBinding> vectors,
    std::string source);

class ReEvidenceAccumulatorAdmission final {
public:
    ReEvidenceAccumulatorAdmission(
        std::shared_ptr<const EvidenceAccumulatorState> state,
        std::optional<CognitiveEvent> event,
        std::vector<EvidenceObservation> observations,
        std::vector<AccumulatorUpdate> updates,
        std::shared_ptr<const AccumulatorDecision> decision, bool admitted,
        std::string reason, bool world_write_eligible);

    const std::shared_ptr<const EvidenceAccumulatorState> state;
    const std::optional<CognitiveEvent> event;
    const std::vector<EvidenceObservation> observations;
    const std::vector<AccumulatorUpdate> updates;
    const std::shared_ptr<const AccumulatorDecision> decision;
    const bool admitted;
    const std::string reason;
    const bool world_write_eligible;
};

[[nodiscard]] ReEvidenceAccumulatorAdmission admit_re_evidence_to_accumulator(
    const MainReEvidenceReceipt& receipt,
    std::shared_ptr<const EvidenceAccumulatorState> state,
    const EvidenceAccumulatorConfig& config, std::string hypothesis_id,
    std::string event_id, std::int64_t current_step);

}  // namespace swegca::world
