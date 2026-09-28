#pragma once

#include "swegca_architecture/connection_strength_kernel.hpp"

#include <cstdint>
#include "vrs/evidence_executor.hpp"

namespace swegca::vrs {

enum class ConnectionChange : std::uint8_t {
    preserve = 0,
    strengthen = 1,
    weaken = 2,
};

class VerificationResult final {
public:
    constexpr VerificationResult() noexcept = default;

    // Keep the complete core result, including invalid_input on an abstention.
    [[nodiscard]] constexpr const architecture::kernel::EvidenceJudgment& judgment() const noexcept {
        return judgment_;
    }

    // Direction is a projection of the core judgment, never independent state.
    [[nodiscard]] constexpr ConnectionChange connection_change() const noexcept {
        switch (judgment_.status()) {
        case architecture::kernel::EvidenceStatus::accept:
            return ConnectionChange::strengthen;
        case architecture::kernel::EvidenceStatus::reject:
            return ConnectionChange::weaken;
        case architecture::kernel::EvidenceStatus::abstain:
            return ConnectionChange::preserve;
        }
        return ConnectionChange::preserve;
    }

private:
    friend VerificationResult verify_experience(
        const architecture::kernel::EvidenceRules&,
        const architecture::kernel::EvidenceTally&);
    constexpr explicit VerificationResult(architecture::kernel::EvidenceJudgment judgment) noexcept
        : judgment_(judgment) {}
    architecture::kernel::EvidenceJudgment judgment_;
};

// The caller supplies evidence formed from values of the shuffled experiences.
// This function does not construct that evidence or read a preexisting tally.
// The SWEGCA core alone produces the three-state judgment.
[[nodiscard]] inline VerificationResult verify_experience(
    const architecture::kernel::EvidenceRules& rules,
    const architecture::kernel::EvidenceTally& shuffled_evidence) {
    return VerificationResult{current_evidence_executor ? current_evidence_executor->judge(rules, shuffled_evidence) : architecture::kernel::judge_evidence(rules, shuffled_evidence)};
}

class ConnectionVerification final {
public:
    constexpr ConnectionVerification() noexcept = default;
    [[nodiscard]] constexpr const VerificationResult& verification() const noexcept {
        return verification_;
    }
    [[nodiscard]] constexpr const architecture::kernel::ConnectionStrengthResult& strength() const noexcept {
        return strength_;
    }

private:
    friend ConnectionVerification verify_connection(
        const architecture::kernel::EvidenceRules&,
        const architecture::kernel::EvidenceTally&, double);
    VerificationResult verification_;
    architecture::kernel::ConnectionStrengthResult strength_;
};

// Both the verdict and numerical strength projection are core operations.
// This composes a candidate result; it does not issue storage authority.
[[nodiscard]] inline ConnectionVerification verify_connection(
    const architecture::kernel::EvidenceRules& rules,
    const architecture::kernel::EvidenceTally& shuffled_evidence,
    double previous_strength) {
    ConnectionVerification result;
    result.verification_ = verify_experience(rules, shuffled_evidence);
    result.strength_ = architecture::kernel::revise_connection_strength(
        previous_strength, result.verification_.judgment());
    return result;
}

}  // namespace swegca::vrs
