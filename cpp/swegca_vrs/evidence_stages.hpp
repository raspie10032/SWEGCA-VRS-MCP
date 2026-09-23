#pragma once

#include "swegca_vrs/allocation.hpp"
#include "swegca_vrs/core_digest.hpp"

#include "swegca_vrs/evidence_accumulator.hpp"
#include "swegca_vrs/allocation.hpp"
#include "swegca_architecture/digest_bytes.hpp"
#include "swegca_vrs/identity_types.hpp"

#include <map>
#include <memory>
#include <string_view>
#include <type_traits>
#include <vector>

// VRS stages that feed the core verifier (layer plan §1): Main's grouping
// of sources into families, admission (Replay, provenance, then the
// accumulator's admission step) and Re-evidence (Replay, Main's judge, then
// the accumulator's recording step). They read the journal and the
// experience records; the core does not.
// Rules: ARCHITECTURE_SPEC.md@5901a5a §4.4, I03 provenance; order
// @30b73e7:24-29; COMPONENT_LEDGER.md@5901a5a:44-50.
namespace swegca::vrs {

namespace journal {
class JournalStore;
}  // namespace journal

class CognitiveState;
class ExperienceRecord;

// Main's grouping of sources into families (COMPONENT_LEDGER.md@5901a5a:
// 44-50: results sharing a source family must be grouped). A source Main
// has not grouped is its own family. A source's family is fixed once Main
// sets it or once evidence is applied under it, so no source is ever
// counted under two families; an observation that is rejected or fails
// fixes nothing. Evidence is applied under every root source of its
// experience (they are linked, see EvidenceAccumulator), so applying it
// fixes every ungrouped root as its own family. Sources and families are
// kept by the SHA-256 of their text, as experiences record their root
// sources; an ungrouped source's family digest is its own.
class SourceFamilies final {
public:
    SourceFamilies(const SourceFamilies&) = delete;
    SourceFamilies& operator=(const SourceFamilies&) = delete;
    SourceFamilies(SourceFamilies&&) = delete;
    SourceFamilies& operator=(SourceFamilies&&) = delete;
    ~SourceFamilies() = default;

    // Groups `source` under `family` (both identity texts). A source whose
    // family is already another fails with `source_family_reassigned`.
    void assign(std::string_view source, std::string_view family);

private:
    friend class MainOwner;
    friend class EvidenceAdmission;
    // SWEGCA: paper/swegca/journal_submission_2026-08-25/COMPONENT_LEDGER.md@5901a5a:44-50
    explicit SourceFamilies(const AllocationContext& memory)
        : memory_(memory), families_(memory.allocator<std::pair<const DigestBytes, DigestBytes>>()) {}

    // source digest -> family digest
    using Map = std::map<DigestBytes, DigestBytes, std::less<>,
                         AllocationAdapter<std::pair<const DigestBytes, DigestBytes>>>;
    using Digests = std::vector<DigestBytes, AllocationAdapter<DigestBytes>>;
    using Fixes = std::vector<Map::node_type, AllocationAdapter<Map::node_type>>;

    // The family digest of every root source of the experience, sorted and
    // unique, into `families`, and whether `family` is one of them, changing
    // nothing. The entries fixing its ungrouped roots are prepared in
    // `fixes`, for `commit` once applied.
    [[nodiscard]] bool root_families(const ExperienceRecord& experience, std::string_view family,
                                     Digests& families, Fixes& fixes) const;
    // Records prepared entries; allocates nothing.
    void commit(Fixes& fixes) noexcept;

    AllocationContext memory_;
    Map families_;
};

// What Re-evidence recorded: the result, and whether it was applied or was
// an exact repeat of a result already kept (original, generation,
// re-evidencer, outcome).
struct ReEvidenceRecorded {
    ReEvidenceResult result;
    AdmissionResult admission = AdmissionResult::applied;
};

// Main's evidence admission (spec :118, order @30b73e7:24-29): replays the
// observation's address from Main's journal and admits it against the state
// content the journal's HEAD names, both from one snapshot, so neither the
// record nor the current content is the caller's to choose. A later HEAD
// with different content requires Re-evidence before that original is current;
// a bit-exact rollback to the same content can reuse its judgment. This is
// the only caller of the accumulator's admission step.
class EvidenceAdmission final {
public:
    EvidenceAdmission(const EvidenceAdmission&) = delete;
    EvidenceAdmission& operator=(const EvidenceAdmission&) = delete;
    EvidenceAdmission(EvidenceAdmission&&) = delete;
    EvidenceAdmission& operator=(EvidenceAdmission&&) = delete;
    ~EvidenceAdmission() = default;

    // Replays `observation.address` (throws if the journal cannot confirm
    // it), requires its provenance to be the experience's (an experience
    // without a context fails `evidence_context_unbound`; a family, context
    // or step that is not the experience's fails
    // `evidence_provenance_mismatch:source_family|context|observed_at`) and
    // admits the observation into `accumulator` as its admission step
    // describes, at `current_step`. A family is fixed as its source's own
    // (SourceFamilies) only when the observation is applied.
    AdmissionResult admit(EvidenceAccumulator& accumulator, const EvidenceObservation& observation,
                          std::uint64_t current_step);

private:
    friend class MainOwner;
    // SWEGCA: user@2026-09-22:24-29
    EvidenceAdmission(const journal::JournalStore& journal, SourceFamilies& families) noexcept
        : journal_(journal), families_(families) {}

    const journal::JournalStore& journal_;  // Main's journal; originals are replayed here
    SourceFamilies& families_;              // Main's grouping of sources
};

// The judgment Main runs on a replayed original: it receives the replayed
// experience, the claim revision and the Cognitive State it re-judges against,
// and returns the outcome (the author's runtime judge sees the artifact it
// judges, never a caller's verdict). It must not touch the accumulator.
// Borrowed for one call, like a function reference: it owns and allocates
// nothing, so the callable must outlive the call it is passed to (pass it
// directly; never keep one).
class ReEvidenceJudge final {
public:
    // SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:475-512
    template <class F>
        requires(!std::is_same_v<std::remove_cvref_t<F>, ReEvidenceJudge> &&
                 std::is_invocable_r_v<EvidenceOutcome, std::remove_reference_t<F>&,
                                       const ExperienceRecord&, const ClaimRevision&,
                                       const CognitiveState&>)
    ReEvidenceJudge(F&& judge) noexcept  // NOLINT(google-explicit-constructor)
        : target_(static_cast<const void*>(std::addressof(judge))),
          call_(&invoke<std::remove_reference_t<F>>) {}

    // SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:508-512
    EvidenceOutcome operator()(const ExperienceRecord& record, const ClaimRevision& claim,
                               const CognitiveState& state) const {
        return call_(target_, record, claim, state);
    }

private:
    using Call = EvidenceOutcome (*)(const void*, const ExperienceRecord&,
                                     const ClaimRevision&, const CognitiveState&);
    // SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:508-512
    template <class T>
    static EvidenceOutcome invoke(const void* target, const ExperienceRecord& record,
                                  const ClaimRevision& claim, const CognitiveState& state) {
        auto& judge = *static_cast<T*>(const_cast<void*>(target));
        return static_cast<EvidenceOutcome>(judge(record, claim, state));
    }

    const void* target_;
    Call call_;
};

// Main's Re-evidence component (order @30b73e7:24-29): replays an admitted
// original from Main's journal and records its re-judgment against the
// current Cognitive State generation. It is the only constructor of
// ReEvidenceResult and the only caller of the accumulator's recording step.
class ReEvidence final {
public:
    ReEvidence(const ReEvidence&) = delete;
    ReEvidence& operator=(const ReEvidence&) = delete;
    ReEvidence(ReEvidence&&) = delete;
    ReEvidence& operator=(ReEvidence&&) = delete;
    ~ReEvidence() = default;

    // Requires `state` to be the generation Main's journal HEAD names
    // (`re_evidence_state_not_current` otherwise), replays `address` (throws
    // if the journal cannot confirm it), asks `judge` for the outcome of the
    // replayed record against the accumulator's claim revision and `state`,
    // requires the record to be an admitted original of `accumulator` with
    // the same content digest, and records that outcome by `by` (a producer
    // id, borrowed) against that generation. Any throw leaves the
    // accumulator exactly as it was.
    [[nodiscard]] ReEvidenceRecorded apply(EvidenceAccumulator& accumulator,
                                 const ExperienceAddress& address, const CognitiveState& state,
                                 std::string_view by, ReEvidenceJudge judge) const;

private:
    friend class MainOwner;
    // SWEGCA: user@2026-09-22:24-29
    ReEvidence(const journal::JournalStore& journal, const AllocationContext& memory) noexcept
        : journal_(journal), memory_(memory) {}

    const journal::JournalStore& journal_;  // Main's journal; originals are replayed here
    AllocationContext memory_;          // Main's allocation context; results are kept in it
};

}  // namespace swegca::vrs
