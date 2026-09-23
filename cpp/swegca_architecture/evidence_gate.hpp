#pragma once

#include "swegca_architecture/evidence_accumulator.hpp"
#include "swegca_architecture/memory_ledger.hpp"
#include "swegca_architecture/proposal.hpp"

#include <cstdint>
#include <optional>

// Main binds a transient producer proposal to its own current evidence
// decision before arbitration. This gate cannot issue a state-write token:
// arbitration, guarded authorization, and publication are later Main stages.
// Rule: ARCHITECTURE_SPEC.md@5901a5a:135,154-174; board §3D, §5, §10.5.
namespace swegca::architecture {

class ExperienceJournal;

enum BindFailure : std::uint32_t {
    bind_foreign_decision = 1u << 0,
    bind_foreign_policy = 1u << 1,
    bind_wrong_claim = 1u << 2,
    bind_stale_evidence = 1u << 3,
    bind_decision_not_accepted = 1u << 4,
    bind_stale_state = 1u << 5,
    bind_empty_targets = 1u << 6,
    bind_empty_evidence = 1u << 7,
    bind_evidence_set_mismatch = 1u << 8,
    bind_delta_or_mask_mismatch = 1u << 9,
    bind_noncanonical_address = 1u << 10,
    bind_record_changed = 1u << 11,
};

struct BindOutcome final {
    std::uint32_t failures = 0;
    Digest256 receipt{Digest256::Bytes{}};
    std::optional<BoundProposal> bound;
};

class EvidenceGate final {
public:
    EvidenceGate(const EvidenceGate&) = delete;
    EvidenceGate& operator=(const EvidenceGate&) = delete;
    EvidenceGate(EvidenceGate&&) = delete;
    EvidenceGate& operator=(EvidenceGate&&) = delete;
    ~EvidenceGate() = default;

    // Main supplies its exact journal, decision, accumulator and current
    // state. Every cited address must be an admitted published experience.
    // An invalid proposal returns a receipt with failure bits and no bound
    // value. Journal corruption or capacity failure propagates fail closed.
    // SWEGCA: paper/swegca/ARCHITECTURE_SPEC.md@5901a5a:154-174
    [[nodiscard]] BindOutcome bind(const EvidenceDecision& decision,
                                   const EvidenceAccumulator& accumulator,
                                   SynapseProposal proposal,
                                   const CognitiveState& state,
                                   std::uint64_t current_step) const;

private:
    friend class MainOwner;
    EvidenceGate(const ExperienceJournal& journal, const MemoryLedger::Account& memory,
                 const EvidencePolicy& evidence_policy);

    const ExperienceJournal& journal_;
    MemoryLedger::Account memory_;
    Digest256 evidence_policy_digest_;
};

}  // namespace swegca::architecture
