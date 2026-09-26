#pragma once
#include "swegca_architecture/evidence_observation_kernel.hpp"

namespace swegca::architecture::kernel {

// Comparison of a recorded outcome with the core's current-evidence judgment.
// Agreement is not semantic promotion or action/write authority.
enum class ReplayAgreement { invalid, insufficient, agrees, contradicts };
[[nodiscard]] inline ReplayAgreement compare_replay_evidence(const EvidenceRules& rules,
    const EvidenceObservation& remembered, const Digest& hypothesis,
    const EvidenceJudgment& current, std::uint64_t step) noexcept {
    const auto use = admit_observation(rules, hypothesis, remembered, step, false);
    if (use == ObservationUse::invalid || current.reason() == EvidenceReason::invalid_input)
        return ReplayAgreement::invalid;
    if (use != ObservationUse::applied || current.status() == EvidenceStatus::abstain)
        return ReplayAgreement::insufficient;
    const bool agrees = (remembered.outcome == EvidenceOutcome::support && current.status() == EvidenceStatus::accept) ||
        (remembered.outcome == EvidenceOutcome::refute && current.status() == EvidenceStatus::reject);
    return agrees ? ReplayAgreement::agrees : ReplayAgreement::contradicts;
}

}  // namespace swegca::architecture::kernel
