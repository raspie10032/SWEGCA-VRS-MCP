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

// The caller supplies evidence for the experience being checked in the
// shuffled traversal. This function makes no claim about how that evidence
// was accumulated; the SWEGCA core alone produces the three-state judgment.
[[nodiscard]] inline VerificationResult verify_experience(
    const architecture::kernel::EvidenceRules& rules,
    const architecture::kernel::EvidenceTally& evidence) noexcept {
    const auto judgment = architecture::kernel::judge_evidence(rules, evidence);
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
