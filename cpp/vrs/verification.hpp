#pragma once

#include "swegca_architecture/evidence_kernel.hpp"

#include <cstdint>

namespace swegca::vrs {

enum class ConnectionChange : std::uint8_t {
    preserve = 0,
    strengthen = 1,
    weaken = 2,
};

struct VerificationResult final {
    // Keep the core's reason, including invalid_input on an abstention.
    architecture::kernel::EvidenceJudgment judgment;
    ConnectionChange connection_change = ConnectionChange::preserve;
};

// The caller supplies evidence formed from values of the shuffled experiences.
// This function does not construct that evidence or read a preexisting tally.
// The SWEGCA core alone produces the three-state judgment.
[[nodiscard]] inline VerificationResult verify_experience(
    const architecture::kernel::EvidenceRules& rules,
    const architecture::kernel::EvidenceTally& shuffled_evidence) noexcept {
    const auto judgment = architecture::kernel::judge_evidence(rules, shuffled_evidence);
    ConnectionChange change = ConnectionChange::preserve;
    switch (judgment.status) {
    case architecture::kernel::EvidenceStatus::accept:
        change = ConnectionChange::strengthen;
        break;
    case architecture::kernel::EvidenceStatus::reject:
        change = ConnectionChange::weaken;
        break;
    case architecture::kernel::EvidenceStatus::abstain:
        break;
    }
    return {judgment, change};
}

}  // namespace swegca::vrs
