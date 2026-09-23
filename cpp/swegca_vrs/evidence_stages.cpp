#include "swegca_vrs/evidence_stages.hpp"

#include "swegca_vrs/cognitive_state.hpp"
#include "swegca_vrs/experience.hpp"
#include "swegca_vrs/journal_store.hpp"
#include "swegca_vrs/core_sha256.hpp"

#include <algorithm>
#include <span>
#include <stdexcept>
#include <utility>

namespace swegca::vrs {

// Replay first (the journal resolves the address through its published view
// and verifies the record, or throws), then Main judges the replayed record
// against the claim revision and the state it passes, then the result is
// bound to what was replayed and to that state's generation, and recorded.
// SWEGCA: user@2026-09-22:24-29
ReEvidenceRecorded ReEvidence::apply(EvidenceAccumulator& accumulator,
                                     const ExperienceAddress& address,
                                     const CognitiveState& state, std::string_view by,
                                     ReEvidenceJudge judge) const {
    // One snapshot gives both the generation HEAD names and the original.
    auto at_head = journal_.replay_at_head(address);
    if (state.generation() != at_head.state)
        throw std::invalid_argument("re_evidence_state_not_current");
    const auto experience = ExperienceRecord::decode(std::move(at_head.record), memory_, journal_);
    experience.verify_parts();  // codex 16:32: fail closed before it counts
    const auto& view = experience.record();
    if (view.address != address.value())
        throw std::invalid_argument("re_evidence_replay_address_mismatch");
    const auto outcome = judge(experience, accumulator.claim(), state);
    if (!evidence_outcome_valid(outcome)) throw std::invalid_argument("re_evidence_outcome_invalid");
    ReEvidenceResult result(accumulator.claim(), address, view.record_digest, state.generation(),
                            ProducerId(memory_, by), outcome);
    const auto admission = accumulator.record(result);
    return ReEvidenceRecorded{std::move(result), admission};
}

// Replay and the generation HEAD names come from one snapshot of Main's
// journal, never from the caller; admission is judged against that pair.
// The source admits a replayed experience as evidence only through its
// claim-relevant, address-bound policy. This Main wrapper also binds the
// journal's current state generation; it is additional C++ infrastructure.
// SWEGCA: paper/swegca/ARCHITECTURE_SPEC.md@5901a5a:111-117
AdmissionResult EvidenceAdmission::admit(EvidenceAccumulator& accumulator,
                                         const EvidenceObservation& observation,
                                         std::uint64_t current_step) {
    detail::require_identity_text(observation.address, ExperienceAddressTag::name);
    detail::require_identity_text(observation.source_family, SourceFamilyTag::name);
    auto at_head =
        journal_.replay_at_head(ExperienceAddress(accumulator.memory_, observation.address));
    const auto experience =
        ExperienceRecord::decode(std::move(at_head.record), accumulator.memory_, journal_);
    experience.verify_parts();  // codex 16:32: fail closed before it counts
    // Provenance is the experience's (COMPONENT_LEDGER.md@5901a5a:44-50), and
    // the evidence is linked to all of it: every root context (already
    // increasing, for_each_root_context checks) and every root family.
    const auto memory = accumulator.memory_;
    SourceFamilies::Digests contexts(memory.allocator<DigestBytes>());
    experience.for_each_root_context([&](const DigestBytes& context) {
        contexts.push_back(context);
        return true;
    });
    if (contexts.empty()) throw std::invalid_argument("evidence_context_unbound");
    if (!std::binary_search(contexts.begin(), contexts.end(), observation.context.bytes()))
        throw std::invalid_argument("evidence_provenance_mismatch:context");
    if (experience.observed_at() != observation.observed_at)
        throw std::invalid_argument("evidence_provenance_mismatch:observed_at");
    SourceFamilies::Digests families(memory.allocator<DigestBytes>());
    SourceFamilies::Fixes fixes(memory.allocator<SourceFamilies::Map::node_type>());
    if (!families_.root_families(experience, observation.source_family, families, fixes))
        throw std::invalid_argument("evidence_provenance_mismatch:source_family");
    const ReplayedOriginal replayed{experience.record().address, experience.record().record_digest,
                                    families, contexts};
    const auto result = accumulator.admit(observation, replayed, at_head.state, current_step);
    if (result == AdmissionResult::applied) families_.commit(fixes);
    return result;
}

// SWEGCA: paper/swegca/journal_submission_2026-08-25/COMPONENT_LEDGER.md@5901a5a:44-50
void SourceFamilies::assign(std::string_view source, std::string_view family) {
    detail::require_identity_text(source, ProducerIdTag::name);
    detail::require_identity_text(family, SourceFamilyTag::name);
    const auto root = Sha256::of(std::as_bytes(std::span(source.data(), source.size())));
    const auto grouped = Sha256::of(std::as_bytes(std::span(family.data(), family.size())));
    const auto found = families_.find(root);
    if (found != families_.end()) {
        if (found->second != grouped) throw std::invalid_argument("source_family_reassigned");
        return;
    }
    families_.emplace(root, grouped);
}

// A grouped root's family is Main's; an ungrouped root's is its own name
// (digest: the root), and the entry fixing it is prepared before the
// accumulator's step. One pass over the roots.
// SWEGCA: paper/swegca/journal_submission_2026-08-25/COMPONENT_LEDGER.md@5901a5a:44-50
bool SourceFamilies::root_families(const ExperienceRecord& experience, std::string_view family,
                                   Digests& families, Fixes& fixes) const {
    experience.for_each_root_source([&](const DigestBytes& root) {
        const auto grouped = families_.find(root);
        if (grouped != families_.end()) {
            families.push_back(grouped->second);
        } else {
            families.push_back(root);
            Map staging(families_.get_allocator());
            staging.emplace(root, root);
            fixes.push_back(staging.extract(staging.begin()));
        }
        return true;
    });
    std::sort(families.begin(), families.end());
    families.erase(std::unique(families.begin(), families.end()), families.end());
    const auto named = Sha256::of(std::as_bytes(std::span(family.data(), family.size())));
    return std::binary_search(families.begin(), families.end(), named);
}

// Inserting prepared nodes allocates nothing and the key comparison
// cannot throw.
// SWEGCA: paper/swegca/journal_submission_2026-08-25/COMPONENT_LEDGER.md@5901a5a:44-50
void SourceFamilies::commit(Fixes& fixes) noexcept {
    for (auto& fix : fixes) (void)families_.insert(std::move(fix));
}

}  // namespace swegca::vrs
