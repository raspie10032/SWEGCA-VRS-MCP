#include "swegca_architecture/evidence_gate.hpp"

#include "swegca_architecture/sha256.hpp"
#include "swegca_architecture/experience.hpp"

#include <algorithm>
#include <array>
#include <utility>
#include <vector>
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

// Original and derived experiences share the journal's canonical address form.
// SWEGCA: paper/swegca/ARCHITECTURE_SPEC.md@5901a5a:118-123
bool experience_address_form(std::string_view address) noexcept {
    if (!address.starts_with(experience_address_prefix) ||
        address.size() != experience_address_prefix.size() + 64) return false;
    for (const char c : address.substr(experience_address_prefix.size()))
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
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
EvidenceGate::EvidenceGate(const ExperienceJournal& journal, MainAuthorityLedger& ledger,
                           const AllocationContext& memory,
                           const GatePolicy& gate_policy, const EvidencePolicy& evidence_policy)
    : journal_(journal), ledger_(ledger), memory_(memory), rules_(make_gate_rules(gate_policy)),
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

// Re-created (user@2026-09-23): Bind precedes arbitration and carries no
// write capability. The receipt commits to every proposal field that can
// affect arbitration, the authoritative decision, the current state and the
// exact failure verdict. Replay validates each cited admitted experience.
// SWEGCA: paper/swegca/ARCHITECTURE_SPEC.md@5901a5a:154-174
BindOutcome EvidenceGate::bind(const EvidenceDecision& decision,
                               const EvidenceAccumulator& accumulator,
                               SynapseProposal proposal,
                               const CognitiveState& state,
                               std::uint64_t current_step) const {
    std::uint32_t failures = 0;
    const bool authentic = decision.issued_by(accumulator) &&
                           decision.claim() == accumulator.claim();
    if (!authentic) failures |= bind_foreign_decision;
    if (decision.rules_digest() != evidence_policy_digest_) failures |= bind_foreign_policy;
    if (decision.claim() != proposal.claim()) failures |= bind_wrong_claim;
    if (proposal.based_on() != state.generation() ||
        !proposal.targets().matches(state.roles())) failures |= bind_stale_state;
    if (journal_.state_generation() != state.generation()) failures |= bind_stale_state;
    if (proposal.targets().selected_count() == 0) failures |= bind_empty_targets;
    if (decision.judgment().status != kernel::EvidenceStatus::accept)
        failures |= bind_decision_not_accepted;
    if (!authentic || decision.accumulator_revision() != accumulator.revision() ||
        !accumulator.evidence_current(state.generation(), current_step))
        failures |= bind_stale_evidence;

    using Views = std::vector<std::string_view, AllocationAdapter<std::string_view>>;
    Views cited(memory_.allocator<std::string_view>());
    cited.reserve(proposal.evidence_addresses().size());
    for (const auto& address : proposal.evidence_addresses()) {
        cited.push_back(address);
        if (!experience_address_form(address)) failures |= bind_noncanonical_address;
    }
    if (cited.empty() || decision.admitted().empty()) failures |= bind_empty_evidence;
    std::sort(cited.begin(), cited.end());
    if (std::adjacent_find(cited.begin(), cited.end()) != cited.end() ||
        cited.size() != decision.admitted().size())
        failures |= bind_evidence_set_mismatch;
    else {
        for (std::size_t at = 0; at < cited.size(); ++at)
            if (cited[at] != decision.admitted()[at].value()) {
                failures |= bind_evidence_set_mismatch;
                break;
            }
    }

    using Admitted = std::vector<const AdmittedEvidence*,
                                 AllocationAdapter<const AdmittedEvidence*>>;
    Admitted admitted(memory_.allocator<const AdmittedEvidence*>());
    admitted.reserve(accumulator.admitted_evidence().size());
    for (const auto& item : accumulator.admitted_evidence()) admitted.push_back(&item);
    std::sort(admitted.begin(), admitted.end(), [](const auto* left, const auto* right) {
        return left->address.value() < right->address.value();
    });
    if (admitted.size() != cited.size()) failures |= bind_evidence_set_mismatch;
    else {
        for (std::size_t at = 0; at < cited.size(); ++at)
            if (admitted[at]->address.value() != cited[at]) {
                failures |= bind_evidence_set_mismatch;
                break;
            }
    }

    const auto delta = proposal_delta_digest(proposal);
    const auto mask = proposal_mask_digest(proposal);
    const auto binding = evidence_binding_digest(memory_, proposal.claim().claim().value(),
                                                 proposal.claim().revision(), cited, delta, mask);
    if (delta != decision.delta_digest() || mask != decision.mask_digest() ||
        binding != decision.binding()) failures |= bind_delta_or_mask_mismatch;

    // Resolve and Replay only exact admitted citations. Admission may include
    // a derived experience whose root source and context were checked there.
    // Recheck every parted blob and the published record digest now. Missing
    // or corrupt data throws before a BoundProposal exists.
    if (failures == 0) {
        for (std::size_t at = 0; at < cited.size(); ++at) {
            const auto replayed = journal_.replay(ExperienceAddress(memory_, cited[at]));
            replayed.verify_parts();
            if (replayed.record().record_digest != admitted[at]->record_digest)
                failures |= bind_record_changed;
        }
    }
    // Publication during Replay invalidates the decision generation. The
    // guarded writer must recheck at commit because HEAD may advance later.
    if (journal_.state_generation() != state.generation()) failures |= bind_stale_state;

    Sha256 hash;
    hash.update("swegca.bind_receipt.v1");
    hash.update(proposal_content_digest(proposal).bytes());
    hash.update(decision.decision_digest().bytes());
    hash.update(decision.evidence_digest().bytes());
    hash.update(decision.rules_digest().bytes());
    hash.update(binding.bytes());
    hash_u64(hash, decision.accumulator_revision());
    hash_u64(hash, current_step);
    hash_u64(hash, state.generation().ordinal());
    hash.update(state.generation().digest().bytes());
    hash_u64(hash, failures);
    BindOutcome outcome;
    outcome.failures = failures;
    outcome.receipt = Digest256(hash.finish());
    if (failures == 0) {
        BoundProposal bound(std::move(proposal), decision.decision_digest(), binding,
                            outcome.receipt, current_step);
        outcome.bound.emplace(std::move(bound));
    }
    return outcome;
}

}  // namespace swegca::architecture
