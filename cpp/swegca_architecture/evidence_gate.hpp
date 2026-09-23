#pragma once

#include "swegca_architecture/authority.hpp"
#include "swegca_architecture/cognitive_state.hpp"
#include "swegca_architecture/evidence_accumulator.hpp"
#include "swegca_architecture/judgment_kernel.hpp"
#include "swegca_architecture/judgment_rules.hpp"
#include "swegca_architecture/allocation.hpp"
#include "swegca_architecture/strong_types.hpp"
#include "swegca_architecture/proposal.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

// Main-owned evidence gate for the guarded verification-slot path, the shell
// around the nano-core `authorize_target` predicate
//   Authorize_target(p, D) = Authorize_impl(p, D) and Nonempty(addresses)
//                            and Bind(D, addresses, delta, mask)
// and the only role that can issue cognitive-state-commit authority.
// Rules (ARCHITECTURE_SPEC.md@5901a5a, paper/swegca):
//   :141-150 Authorize_impl: accepted status, current evidence and
//            accumulator revision, safe runtime context, causal lower bound,
//            source and context diversity, complete definitions,
//            counterfactual and intervention support, no suspected regime
//            change, slot/device/capacity gates, authentic capability and
//            digest at commit time;
//   :152     batch-one Cognitive State, proposal targeting only verification;
//   :155-162 Nonempty and Bind (E001/E002);
//   :135     decisions and registered accumulator state are process-local
//            authority; a forged object with matching fields is not one.
// Bind replays admitted records from Main journal; authorization consumes
// the bound evidence metadata after Replay and Re-evidence.
namespace swegca::architecture {

// What the producer proposes, as borrowed input: the claim revision it
// relies on, the evidence it names, the role it targets and the state
// generation it was made against. Nothing in it is trusted; it is bound.
struct VerificationProposal {
    std::string_view claim;
    std::uint64_t claim_revision = 0;
    std::span<const std::string_view> addresses;
    std::string_view target;
    StateGeneration based_on;
};

// What only Main decides. Main evaluates the delta and mask from the actual
// tensors it would commit (not from the producer's description) and its own
// evaluators decide the conditions the gate cannot compute itself. The gate
// computes the rest (issuing accumulator and its revision, evidence
// currentness, generation, target, registry, binding) and never takes them
// from the caller.
struct MainGateEvaluation {
    Digest256 delta_digest;  // canonical digest of the actual delta tensor
    Digest256 mask_digest;   // canonical digest of the actual role mask
    bool runtime_context_safe = false;
    bool definitions_complete = false;
    bool counterfactual_support = false;
    bool intervention_support = false;
    bool regime_change_suspected = true;
    bool slot_gate = false;
    bool device_gate = false;
    bool capacity_safe = false;
};

// Failures the shell finds beyond the kernel predicate; they share the
// kernel's failure word (kernel bits end at 1 << 17).
enum GateShellFailure : std::uint32_t {
    gate_decision_foreign = 1u << 20,         // :135, not issued by this registered accumulator
    gate_rules_not_main = 1u << 21,           // :135, decided under an unregistered policy
    gate_target_not_verification = 1u << 22,  // :152, target is not the scratch verification role
    gate_generation_stale = 1u << 23,         // :142,150, proposal not made against the current state
};

struct GateOutcome {
    std::uint32_t failures = 0;  // kernel and shell failure bits; 0 means authorized
    std::optional<CognitiveStateCommitAuthority> authority;
    Digest256 receipt{Digest256::Bytes{}};  // what was judged, and the verdict
};

// The operation a cognitive-state-commit capability authorizes: this
// decision, this binding (claim revision, evidence, actual delta and mask),
// this role in this registry, at this generation. The writer recomputes it
// from what it is about to commit and the ledger compares.
[[nodiscard]] Digest256 verification_commit_operation(const Digest256& decision_digest,
                                                      const Digest256& binding,
                                                      std::string_view target,
                                                      const Digest256& registry_digest,
                                                      const StateGeneration& generation);

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

    // Replays admitted citations and binds a producer proposal to Main evidence.
    [[nodiscard]] BindOutcome bind(const EvidenceDecision& decision,
                                   const EvidenceAccumulator& accumulator,
                                   SynapseProposal proposal,
                                   const CognitiveState& state,
                                   std::uint64_t current_step) const;

    // Judges `proposal` against `decision` for the current `state` and issues
    // commit authority only when every condition holds. A failed condition is
    // a result, not an exception; the failure bits name every one. Evidence
    // is current (spec :142) when the decision is at the accumulator's
    // current revision and every admitted original is unexpired at Main's
    // `current_step` and current at the state's generation (observed on it,
    // or re-evidenced on it by Main with the same outcome, with no
    // conflicting result on it). Metadata only; O(log n).
    [[nodiscard]] GateOutcome authorize(const EvidenceDecision& decision,
                                        const EvidenceAccumulator& accumulator,
                                        const VerificationProposal& proposal,
                                        const MainGateEvaluation& evaluation,
                                        const CognitiveState& state,
                                        std::uint64_t current_step) const;

private:
    friend class MainOwner;

    EvidenceGate(const ExperienceJournal& journal, MainAuthorityLedger& ledger,
                 const AllocationContext& memory,
                 const GatePolicy& gate_policy, const EvidencePolicy& evidence_policy);

    const ExperienceJournal& journal_;
    MainAuthorityLedger& ledger_;
    AllocationContext memory_;  // Main's allocation context for binding scratch
    kernel::GateRules rules_;
    Digest256 evidence_policy_digest_;
};

}  // namespace swegca::architecture
