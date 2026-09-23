#include "swegca_architecture/evidence_gate.hpp"

#include "swegca_architecture/sha256.hpp"

#include <array>
#include <string_view>

namespace swegca::architecture {
namespace {

// The target is fixed by the architecture, not configurable.
constexpr std::string_view verification_role = "verification";

// Length-prefixed text so no two field sequences hash the same bytes.
// SWEGCA: src/swegca/mosaic_bounded_world_write.py@5901a5a:153-172
void hash_text(Sha256& hash, std::string_view text) {
    const auto length = static_cast<std::uint64_t>(text.size());
    std::array<std::byte, 8> prefix{};
    for (std::size_t at = 0; at < prefix.size(); ++at)
        prefix[at] = static_cast<std::byte>((length >> (8 * at)) & 0xff);
    hash.update(prefix);
    hash.update(text);
}

// SWEGCA: src/swegca/mosaic_bounded_world_write.py@5901a5a:153-172
void hash_u64(Sha256& hash, std::uint64_t value) {
    std::array<std::byte, 8> bytes{};
    for (std::size_t at = 0; at < bytes.size(); ++at)
        bytes[at] = static_cast<std::byte>((value >> (8 * at)) & 0xff);
    hash.update(bytes);
}

// SWEGCA: src/swegca/mosaic_bounded_world_write.py@5901a5a:153-172
void hash_generation(Sha256& hash, const StateGeneration& generation) {
    hash_u64(hash, generation.ordinal());
    hash.update(generation.digest().bytes());
}

}  // namespace

// SWEGCA: src/swegca/mosaic_bounded_world_write.py@5901a5a:153-172
Digest256 verification_commit_operation(const Digest256& decision_digest,
                                        const Digest256& binding, std::string_view target,
                                        const Digest256& registry_digest,
                                        const StateGeneration& generation) {
    Sha256 hash;
    hash_text(hash, "swegca.verification_commit.v1");
    hash.update(decision_digest.bytes());
    hash.update(binding.bytes());
    hash_text(hash, target);
    hash.update(registry_digest.bytes());
    hash_generation(hash, generation);
    return Digest256(hash.finish());
}

// The gate thresholds, Main's ledger and the one evidence policy whose
// decisions it accepts are fixed when Main creates it.
// SWEGCA: src/swegca/mosaic_bounded_world_write.py@5901a5a:95-150
EvidenceGate::EvidenceGate(MainAuthorityLedger& ledger, const MemoryLedger::Account& memory,
                           const GatePolicy& gate_policy, const EvidencePolicy& evidence_policy)
    : ledger_(ledger), memory_(memory), rules_(make_gate_rules(gate_policy)),
      evidence_policy_digest_(evidence_policy_digest(evidence_policy)) {}

// Everything the gate can compute, it computes; the kernel predicate then
// judges all conditions at once, so the receipt names every failure.
// SWEGCA: src/swegca/mosaic_bounded_world_write.py@5901a5a:286-375
GateOutcome EvidenceGate::authorize(const EvidenceDecision& decision,
                                    const EvidenceAccumulator& accumulator,
                                    const VerificationProposal& proposal,
                                    const MainGateEvaluation& evaluation,
                                    const CognitiveState& state,
                                    std::uint64_t current_step) const {
    std::uint32_t shell = 0;
    // :135 — the decision must come from this very registered accumulator,
    // for its claim revision, under the policy Main registered.
    const bool same_claim =
        decision.issued_by(accumulator) && decision.claim() == accumulator.claim();
    if (!same_claim) shell |= gate_decision_foreign;
    if (decision.rules_digest() != evidence_policy_digest_) shell |= gate_rules_not_main;
    // :152 — only the verification role, which lives in scratch. Batch one is
    // an invariant of CognitiveState itself.
    const auto* role = state.roles().find(proposal.target);
    if (proposal.target != verification_role || role == nullptr ||
        role->partition != TensorPartition::scratch)
        shell |= gate_target_not_verification;
    // :142, :150 — the proposal was made against the current state.
    if (proposal.based_on != state.generation()) shell |= gate_generation_stale;

    // :155-162 — Bind over the proposal's claim and named evidence and the
    // delta and mask Main evaluated from the actual tensors.
    const auto binding =
        evidence_binding_digest(memory_, proposal.claim, proposal.claim_revision,
                                proposal.addresses, evaluation.delta_digest, evaluation.mask_digest);

    // :142 — current evidence and accumulator revision, both computed. The
    // author derives currentness from pinned artifacts matching and every
    // update having been made against the current state
    // (mosaic_evidence_revision.py@5901a5a:81-180); natively the accumulator
    // holds each original's replayed content digest and observation
    // generation, and Main's Re-evidence results per generation, so the gate
    // compares metadata only. Any admission or Re-evidence after the
    // decision advances the revision, so a stale decision fails here.
    const bool revision_current =
        same_claim && decision.accumulator_revision() == accumulator.revision();
    const bool evidence_current =
        revision_current && accumulator.evidence_current(state.generation(), current_step);

    std::uint16_t conditions = 0;
    const auto set = [&conditions](bool holds, std::uint16_t bit) {
        if (holds) conditions |= bit;
    };
    set(evidence_current, kernel::condition_evidence_current);
    set(revision_current, kernel::condition_accumulator_revision_current);
    set(evaluation.runtime_context_safe, kernel::condition_runtime_context_safe);
    set(evaluation.definitions_complete, kernel::condition_definitions_complete);
    set(evaluation.counterfactual_support, kernel::condition_counterfactual_support);
    set(evaluation.intervention_support, kernel::condition_intervention_support);
    set(evaluation.regime_change_suspected, kernel::condition_regime_change_suspected);
    set(evaluation.slot_gate, kernel::condition_slot_gate);
    set(evaluation.device_gate, kernel::condition_device_gate);
    set(evaluation.capacity_safe, kernel::condition_capacity_safe);

    const auto& judgment = decision.judgment();
    kernel::GateInput input;
    input.status = judgment.status;
    input.causal_lower_bound = judgment.causal_lower_bound;
    input.source_diversity = judgment.source_diversity;
    input.context_diversity = judgment.context_diversity;
    input.conditions = conditions;
    input.address_count = proposal.addresses.size();
    input.decision_binding = decision.binding().bytes();
    input.proposal_binding = binding.bytes();
    const std::uint32_t failures = kernel::authorize_target(rules_, input) | shell;

    GateOutcome outcome;
    outcome.failures = failures;
    Sha256 receipt;
    hash_text(receipt, "swegca.evidence_gate_receipt.v2");
    receipt.update(decision.decision_digest().bytes());
    receipt.update(decision.evidence_digest().bytes());
    receipt.update(binding.bytes());
    hash_text(receipt, proposal.target);
    hash_generation(receipt, proposal.based_on);
    hash_generation(receipt, state.generation());
    hash_u64(receipt, failures);
    outcome.receipt = Digest256(receipt.finish());
    if (failures != 0) return outcome;

    // :150 — authentic capability bound to exactly what was authorized.
    outcome.authority = ledger_.issue<AuthorityDomain::cognitive_state_commit>(
        IssueKey<AuthorityDomain::cognitive_state_commit>{}, state.owner(), state.generation(),
        verification_commit_operation(decision.decision_digest(), binding, role->id.value(),
                                      state.roles().digest(), state.generation()));
    return outcome;
}

}  // namespace swegca::architecture
