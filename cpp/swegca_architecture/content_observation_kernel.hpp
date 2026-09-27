#pragma once

#include "swegca_architecture/evidence_observation_kernel.hpp"

namespace swegca::architecture::kernel {

// An observation of the explicitly requested predicate "equal byte content".
// This is not a language classifier or an accept/reject/abstain judgment.
// Only judge_evidence, after normal admission/shuffle, can issue that judgment.
// Incomplete or changing reads cannot supply support OR refutation.
[[nodiscard]] constexpr EvidenceOutcome observe_content_equality(
    bool complete, bool stable, bool equal) noexcept {
    if (!complete || !stable) return EvidenceOutcome::insufficient;
    return equal ? EvidenceOutcome::support : EvidenceOutcome::refute;
}

} // namespace swegca::architecture::kernel
