#include "swegca_architecture/evidence_gate.hpp"

#include "swegca_architecture/sha256.hpp"

#include <algorithm>
#include <array>
#include <string_view>
#include <utility>
#include <vector>

namespace swegca::architecture {
namespace {

// SWEGCA: paper/swegca/ARCHITECTURE_SPEC.md@5901a5a:154-162
void hash_u64(Sha256& hash, std::uint64_t value) {
    std::array<std::byte, 8> bytes{};
    for (std::size_t at = 0; at < bytes.size(); ++at)
        bytes[at] = static_cast<std::byte>((value >> (8 * at)) & 0xff);
    hash.update(bytes);
}

// Only the journal's canonical original-experience address form is accepted.
// SWEGCA: paper/swegca/ARCHITECTURE_SPEC.md@5901a5a:118-123
bool original_address_form(std::string_view address) noexcept {
    if (!address.starts_with(experience_address_prefix) ||
        address.size() != experience_address_prefix.size() + 64) return false;
    for (const char c : address.substr(experience_address_prefix.size()))
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}

}  // namespace

// The policy identity is held by Main; neither a producer nor a decision from
// a different accumulator can substitute it.
// SWEGCA: paper/swegca/ARCHITECTURE_SPEC.md@5901a5a:135-162
EvidenceGate::EvidenceGate(const ExperienceJournal& journal,
                           const MemoryLedger::Account& memory,
                           const EvidencePolicy& evidence_policy)
    : journal_(journal), memory_(memory),
      evidence_policy_digest_(evidence_policy_digest(evidence_policy)) {
    // SWEGCA: paper/swegca/ARCHITECTURE_SPEC.md@5901a5a:135-162
}

// Re-created (user@2026-09-23): Bind precedes arbitration and carries no
// write capability. The receipt commits to every proposal field that can
// affect arbitration, the authoritative decision, the current state and the
// exact failure verdict. Replay validates the cited original kind.
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
    if (proposal.targets().selected_count() == 0) failures |= bind_empty_targets;
    if (decision.judgment().status != kernel::EvidenceStatus::accept)
        failures |= bind_decision_not_accepted;
    if (!authentic || decision.accumulator_revision() != accumulator.revision() ||
        !accumulator.evidence_current(state.generation(), current_step))
        failures |= bind_stale_evidence;

    using Views = std::vector<std::string_view, MemoryLedger::Allocator<std::string_view>>;
    Views cited(memory_.allocator<std::string_view>());
    cited.reserve(proposal.evidence_addresses().size());
    for (const auto& address : proposal.evidence_addresses()) cited.push_back(address);
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

    const auto delta = proposal_delta_digest(proposal);
    const auto mask = proposal_mask_digest(proposal);
    const auto binding = evidence_binding_digest(memory_, proposal.claim().claim().value(),
                                                 proposal.claim().revision(), cited, delta, mask);
    if (delta != decision.delta_digest() || mask != decision.mask_digest() ||
        binding != decision.binding()) failures |= bind_delta_or_mask_mismatch;

    // Resolve and Replay only exact citations. A missing or corrupt record
    // throws before a BoundProposal exists; it cannot become authority.
    if (failures == 0) {
        for (const auto address : cited) {
            if (!original_address_form(address)) {
                failures |= bind_nonoriginal_address;
                continue;
            }
            const auto original = journal_.replay(ExperienceAddress(memory_, address));
            if (original.derived() || original.record().kind != original_experience_kind)
                failures |= bind_nonoriginal_address;
        }
    }

    Sha256 hash;
    hash.update("swegca.bind_receipt.v1");
    hash.update(proposal_content_digest(proposal).bytes());
    hash.update(decision.decision_digest().bytes());
    hash.update(decision.evidence_digest().bytes());
    hash.update(decision.rules_digest().bytes());
    hash.update(binding.bytes());
    hash_u64(hash, decision.accumulator_revision());
    hash_u64(hash, state.generation().ordinal());
    hash.update(state.generation().digest().bytes());
    hash_u64(hash, failures);
    BindOutcome outcome;
    outcome.failures = failures;
    outcome.receipt = Digest256(hash.finish());
    if (failures == 0) {
        BoundProposal bound(std::move(proposal), decision.decision_digest(), binding,
                            outcome.receipt);
        outcome.bound.emplace(std::move(bound));
    }
    return outcome;
}

}  // namespace swegca::architecture
