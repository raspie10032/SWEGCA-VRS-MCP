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

// The caller reads the existing cumulative evidence for the original selected
// by the shuffled traversal. Shuffling does not re-accumulate that evidence;
// for fixed rules and a fixed tally, order cannot change this item's verdict.
// The SWEGCA core alone produces the three-state judgment.
[[nodiscard]] inline VerificationResult verify_experience(
    const architecture::kernel::EvidenceRules& rules,
    const architecture::kernel::EvidenceTally& cumulative_evidence) noexcept {
    const auto judgment = architecture::kernel::judge_evidence(rules, cumulative_evidence);
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
