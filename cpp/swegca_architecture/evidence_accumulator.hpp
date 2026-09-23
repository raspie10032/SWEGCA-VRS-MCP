#pragma once

#include "swegca_architecture/authority_roles.hpp"
#include "swegca_architecture/digest_bytes.hpp"
#include "swegca_architecture/evidence_kernel.hpp"
#include "swegca_architecture/evidence_rules.hpp"
#include "swegca_architecture/allocation.hpp"
#include "swegca_architecture/strong_types.hpp"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#if !defined(__SIZEOF_INT128__)
#error "exact axis sums need a 128-bit unsigned integer"
#endif

// Main-owned evidence accumulation for one claim revision. This is the shell
// around the nano-core: it admits observations, keeps the fixed-size tally
// the kernel judges, records every rejected observation, records Main's
// Re-evidence of admitted originals, and issues decisions that carry what the
// design board requires (claim revision, accumulator revision, admitted and
// rejected address sets, evaluated delta and mask digests, rule configuration
// digest, binding digest and decision digest).
// Every container allocates through Main's allocation context (the one
// Main creates the accumulator with); nothing here reads the journal.
// Rules: ARCHITECTURE_SPEC.md@5901a5a §4.4 (accumulation; decisions are
// process-local), §4.5 (Bind), I03 provenance; design board @7c0b62f:195-198;
// Re-evidence is an explicit input stage (board @cefdc3f:459-462) run by Main
// against the current generation after Replay (order @30b73e7:24-29).
namespace swegca::architecture {

namespace journal {
class JournalStore;
class PublishedRecord;
struct RecordView;
}  // namespace journal

class CognitiveState;
class ExperienceRecord;

struct SourceFamilyTag {
    static constexpr std::string_view name = "source_family";
};
using SourceFamily = TextIdentity<SourceFamilyTag>;

enum class EvidenceOutcome : std::uint8_t { support = 1, refute = 2, insufficient = 3 };

// One observation judged against one claim revision: the producer's borrowed
// input. Its texts must satisfy the identity rule; Main copies what it keeps
// onto its own account. `address` must name a published experience record
// (spec :118, board §4 :213-214); Main replays it, decodes it as experience
// and hands it to `admit`, so no caller-supplied position or digest is ever
// trusted. `judged_against` is
// the Cognitive State generation the producer judged on; it is admitted only
// when it is the generation Main's journal HEAD names (the author's audit row
// requires the world hash to be the current state's), and then it is the
// observation generation, which Re-evidence never rewrites.
// Provenance is the replayed experience's (COMPONENT_LEDGER.md@5901a5a:
// 44-50, codex 15:40): `source_family` must be the family (SourceFamilies)
// of one of the experience's root sources, `context` one of its root
// contexts and `observed_at` its step. `producer` is the judge, which no experience
// names; it cannot raise diversity above what the experiences bear out,
// since source and context diversity are each the smaller of the verified
// count and the producer count. Evidence is grouped by what it shares,
// not by the one family and context it names (see EvidenceAccumulator).
struct EvidenceObservation {
    std::string_view claim;  // claim id of the revision judged
    std::uint64_t claim_revision = 0;
    std::string_view address;
    StateGeneration judged_against;
    std::string_view source_family;
    Digest256 context;
    std::uint32_t axis = 0;  // index into the policy's axes
    EvidenceOutcome outcome = EvidenceOutcome::insufficient;
    std::uint64_t observed_at = 0;
    std::optional<std::uint64_t> expires_at;
    std::string_view producer;
    double producer_confidence = 0;
};

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

enum class AdmissionResult : std::uint8_t {
    applied = 1,
    expired = 2,
    insufficient = 3,
    duplicate = 4,
    stale = 5,  // judged on a generation other than the one HEAD names
};

template <class T>
using EvidenceVector = std::vector<T, AllocationAdapter<T>>;

// What an admitted original still binds after admission: the published
// record's content digest (from Replay), the observation generation, its
// expiry and outcome, and every provenance field of the author's audit row
// (axis, source family, context, producer, observation step, producer
// confidence). It is never rewritten.
struct AdmittedEvidence {
    ExperienceAddress address;
    DigestBytes record_digest{};
    StateGeneration judged_against;
    std::optional<std::uint64_t> expires_at;
    EvidenceOutcome outcome = EvidenceOutcome::insufficient;
    std::uint32_t axis = 0;
    SourceFamily source_family;
    Digest256 context;
    ProducerId producer;
    std::uint64_t observed_at = 0;
    double producer_confidence = 0;
};

// An observation that was not admitted, with the reason. Rejected evidence is
// kept, never dropped: it is part of what a decision was made from.
struct RejectedEvidence {
    ExperienceAddress address;
    AdmissionResult reason = AdmissionResult::expired;

    auto operator<=>(const RejectedEvidence&) const = default;
};

class ReEvidence;

// Main's Re-evidence of one admitted original: the record Main replayed (its
// address and content digest), the claim revision, the generation it was
// re-judged against (the re-evidence generation), who re-judged it and the
// outcome. Only Main's Re-evidence component constructs one, after Replay.
// It adds nothing to the tally (the original is counted once); it decides
// whether the original is current at that generation, and a result whose
// outcome differs from the original's is an unresolved conflict, kept. A
// re-evidencer that changes its outcome on the same generation adds a second
// result; only an exact repeat is refused, and the refusal is kept.
class ReEvidenceResult final {
public:
    // SWEGCA: docs/SWEGCA_VRS_MCP_ORDER_FOR_REVIEW.md@30b73e7:24-29
    [[nodiscard]] const ClaimRevision& claim() const noexcept { return claim_; }
    // SWEGCA: docs/SWEGCA_VRS_MCP_ORDER_FOR_REVIEW.md@30b73e7:24-29
    [[nodiscard]] const ExperienceAddress& address() const noexcept { return address_; }
    // SWEGCA: docs/SWEGCA_VRS_MCP_ORDER_FOR_REVIEW.md@30b73e7:24-29
    [[nodiscard]] const DigestBytes& record_digest() const noexcept { return record_digest_; }
    // SWEGCA: docs/SWEGCA_VRS_MCP_ORDER_FOR_REVIEW.md@30b73e7:24-29
    [[nodiscard]] const StateGeneration& generation() const noexcept { return generation_; }
    // SWEGCA: docs/SWEGCA_VRS_MCP_ORDER_FOR_REVIEW.md@30b73e7:24-29
    [[nodiscard]] const ProducerId& re_evidenced_by() const noexcept { return by_; }
    // SWEGCA: docs/SWEGCA_VRS_MCP_ORDER_FOR_REVIEW.md@30b73e7:24-29
    [[nodiscard]] EvidenceOutcome outcome() const noexcept { return outcome_; }

private:
    friend class ReEvidence;
    ReEvidenceResult(ClaimRevision claim, ExperienceAddress address, DigestBytes record_digest,
                     StateGeneration generation, ProducerId by, EvidenceOutcome outcome);

    ClaimRevision claim_;
    ExperienceAddress address_;
    DigestBytes record_digest_;
    StateGeneration generation_;
    ProducerId by_;
    EvidenceOutcome outcome_;
};

// Digest that binds a decision to (claim id and revision, exact admitted
// address set, delta, mask). The set is sorted and deduplicated before
// hashing, so the same set gives the same digest regardless of citation
// order; the sort scratch is charged to `memory`.
[[nodiscard]] Digest256 evidence_binding_digest(const AllocationContext& memory,
                                                std::string_view claim,
                                                std::uint64_t claim_revision,
                                                std::span<const std::string_view> addresses,
                                                const Digest256& delta_digest,
                                                const Digest256& mask_digest);

class EvidenceAccumulator;

// An evidence decision exists only as an accumulator output; visible fields
// cannot construct one. It is process-local and never persisted as authority.
class EvidenceDecision final {
public:
    // SWEGCA: src/swegca/mosaic_evidence_accumulator.py@5901a5a:238-250
    [[nodiscard]] const ClaimRevision& claim() const noexcept { return claim_; }
    // SWEGCA: src/swegca/mosaic_evidence_accumulator.py@5901a5a:238-250
    [[nodiscard]] std::uint64_t accumulator_revision() const noexcept {
        return judgment_.revision;
    }
    // SWEGCA: src/swegca/mosaic_evidence_accumulator.py@5901a5a:238-250
    [[nodiscard]] const kernel::EvidenceJudgment& judgment() const noexcept { return judgment_; }
    // SWEGCA: src/swegca/mosaic_evidence_accumulator.py@5901a5a:238-250
    [[nodiscard]] std::span<const ExperienceAddress> admitted() const noexcept {
        return admitted_;
    }
    // SWEGCA: src/swegca/mosaic_evidence_accumulator.py@5901a5a:238-250
    [[nodiscard]] std::span<const RejectedEvidence> rejected() const noexcept {
        return rejected_;
    }
    // SWEGCA: src/swegca/mosaic_evidence_accumulator.py@5901a5a:238-250
    [[nodiscard]] const Digest256& delta_digest() const noexcept { return delta_digest_; }
    // SWEGCA: src/swegca/mosaic_evidence_accumulator.py@5901a5a:238-250
    [[nodiscard]] const Digest256& mask_digest() const noexcept { return mask_digest_; }
    // SWEGCA: src/swegca/mosaic_evidence_accumulator.py@5901a5a:238-250
    [[nodiscard]] const Digest256& rules_digest() const noexcept { return rules_digest_; }
    // SWEGCA: src/swegca/mosaic_evidence_accumulator.py@5901a5a:238-250
    [[nodiscard]] const Digest256& binding() const noexcept { return binding_; }
    // SWEGCA: src/swegca/mosaic_evidence_accumulator.py@5901a5a:238-250
    [[nodiscard]] const Digest256& decision_digest() const noexcept { return decision_digest_; }
    // Digest of the admitted originals and every Re-evidence result the
    // decision was made from.
    // SWEGCA: src/swegca/mosaic_evidence_revision.py@5901a5a:81-180
    [[nodiscard]] const Digest256& evidence_digest() const noexcept { return evidence_digest_; }
    // True only for the very accumulator object that issued this decision
    // (spec :135); another accumulator with equal visible fields is not it.
    [[nodiscard]] bool issued_by(const EvidenceAccumulator& accumulator) const noexcept;

private:
    friend class EvidenceAccumulator;
    EvidenceDecision(const AllocationContext& memory, ClaimRevision claim,
                     kernel::EvidenceJudgment judgment, EvidenceVector<ExperienceAddress> admitted,
                     EvidenceVector<RejectedEvidence> rejected, Digest256 delta_digest,
                     Digest256 mask_digest, Digest256 rules_digest, Digest256 evidence_digest,
                     std::weak_ptr<const void> origin);
    [[nodiscard]] Digest256 compute_decision_digest() const;

    ClaimRevision claim_;
    kernel::EvidenceJudgment judgment_;
    EvidenceVector<ExperienceAddress> admitted_;  // sorted, unique
    EvidenceVector<RejectedEvidence> rejected_;   // sorted, unique
    Digest256 delta_digest_;
    Digest256 mask_digest_;
    Digest256 rules_digest_;
    Digest256 evidence_digest_;
    std::weak_ptr<const void> origin_;  // the issuing accumulator, never owned
    Digest256 binding_;
    Digest256 decision_digest_;
};

// Registered accumulator state is process-local authority (spec :135): only
// Main creates accumulators, so a decision cannot come from an accumulator
// Main did not register. It can be neither copied nor moved, so the object
// Main registered is the only one that holds its identity.
//
// Groups (COMPONENT_LEDGER.md@5901a5a:44-50: results sharing a source
// family, context or derived evidence must be grouped). The author's
// accumulator groups by (source family, context hash) only and has no
// derived evidence; linking what derived evidence shares, transitively,
// is this rebuild's conservative reading of that rule, not the author's
// code (codex 17:04): it can only lower diversity. Each admitted
// experience links the families of all its root sources, and all its root
// contexts; families linked directly or through other admitted evidence
// are one source group, contexts likewise one context group, and a group
// of evidence is a (source group, context group) pair. An original has one
// root source and one context, so with originals alone the groups are the
// author's (source family, context hash). Groups only merge (nothing
// admitted is removed), so they are the same in any admission order, and
// diversity counts groups: evidence that shares a root with other evidence
// never counts as an independent sample of it.
//
// Axis sums: each (source, context) group contributes the author's ratios
// supports / total and refutes / total as IEEE doubles, unchanged. A nonzero
// ratio is at least 1 / total > 2^-32 (total < 2^32), so it is an exact
// integer multiple of 2^-84; the axis keeps the exact 128-bit sum of those
// multiples and rounds to double once. That is the exactly rounded value of
// the author's sum of the same ratios, independent of admission order, and
// one admission updates it in O(log n), or, when it joins groups, in one
// pass over the groups (as the author's admission always takes).
class EvidenceAccumulator final {
public:
    EvidenceAccumulator(const EvidenceAccumulator&) = delete;
    EvidenceAccumulator& operator=(const EvidenceAccumulator&) = delete;
    EvidenceAccumulator(EvidenceAccumulator&&) = delete;
    EvidenceAccumulator& operator=(EvidenceAccumulator&&) = delete;
    ~EvidenceAccumulator() = default;

    // SWEGCA: src/swegca/mosaic_evidence_accumulator.py@5901a5a:222-236
    [[nodiscard]] const ClaimRevision& claim() const noexcept { return claim_; }

    // SWEGCA: src/swegca/mosaic_evidence_accumulator.py@5901a5a:238-250
    [[nodiscard]] const kernel::EvidenceTally& tally() const noexcept { return tally_; }
    // Advances on every admitted original, every newly rejected observation,
    // every recorded Re-evidence result and every newly refused repeat, so a
    // decision made before any of them is stale.
    // SWEGCA: src/swegca/mosaic_evidence_accumulator.py@5901a5a:238-250
    [[nodiscard]] std::uint64_t revision() const noexcept { return tally_.revision; }
    // In admission order.
    // SWEGCA: src/swegca/mosaic_evidence_revision.py@5901a5a:81-180
    [[nodiscard]] std::span<const AdmittedEvidence> admitted_evidence() const noexcept {
        return admitted_evidence_;
    }
    // In recording order.
    // SWEGCA: docs/SWEGCA_VRS_MCP_ORDER_FOR_REVIEW.md@30b73e7:24-29
    [[nodiscard]] std::span<const ReEvidenceResult> re_evidence() const noexcept {
        return re_evidence_;
    }

    // Spec :142 from metadata only: every admitted original is unexpired at
    // `current_step` and current at `generation` — observed on it, or
    // re-evidenced on it with the original's outcome — and no Re-evidence
    // result at `generation` conflicts with its original. O(log n).
    [[nodiscard]] bool evidence_current(const StateGeneration& generation,
                                        std::uint64_t current_step) const;

    // Judges the current tally and binds the decision to the admitted set and
    // the Main-evaluated delta and mask digests.
    [[nodiscard]] EvidenceDecision decide(const Digest256& delta_digest,
                                          const Digest256& mask_digest) const;

private:
    friend class MainOwner;
    friend class EvidenceDecision;
    friend class ReEvidence;
    friend class EvidenceAdmission;
    EvidenceAccumulator(const AllocationContext& memory, ClaimRevision claim,
                        const EvidencePolicy& policy);

    // Called by EvidenceAdmission only. `replayed` is the experience Main
    // replayed at `observation.address` (its record digest is what the
    // original binds), `root_families` and `root_contexts` its root
    // families and contexts (sorted, unique; they must hold the
    // observation's, `evidence_correlation_invalid`) and `current` the
    // state generation HEAD names. Throws on an observation
    // for another claim revision, an unknown axis, invalid fields or a record
    // for another address. A duplicate (address already admitted), stale
    // (judged on another generation than `current`), expired or insufficient
    // observation leaves the tally unchanged and is recorded as rejected. Any
    // throw leaves the accumulator exactly as it was.
    AdmissionResult admit(const EvidenceObservation& observation, const ExperienceRecord& replayed,
                          std::span<const DigestBytes> root_families,
                          std::span<const DigestBytes> root_contexts,
                          const StateGeneration& current, std::uint64_t current_step);

    using Text = std::basic_string<char, std::char_traits<char>, AllocationAdapter<char>>;
    template <class K, class V>
    using Map = std::map<K, V, std::less<>, AllocationAdapter<std::pair<const K, V>>>;
    template <class K>
    using Set = std::set<K, std::less<>, AllocationAdapter<K>>;

    struct GroupKey {
        std::uint32_t source = 0;   // source group (its root id)
        std::uint32_t context = 0;  // context group (its root id)
        auto operator<=>(const GroupKey&) const = default;
    };
    // Union-find over identities (family or context digests): ids in first
    // admission order, union by size, so a find walks O(log n) parents.
    struct Groups {
        Map<DigestBytes, std::uint32_t> ids;
        EvidenceVector<std::uint32_t> parent;
        EvidenceVector<std::uint32_t> size;

        // SWEGCA: paper/swegca/journal_submission_2026-08-25/COMPONENT_LEDGER.md@5901a5a:44-50
        [[nodiscard]] std::uint32_t find(std::uint32_t id) const noexcept {
            while (parent[id] != id) id = parent[id];
            return id;
        }
    };
    // What linking a set of identities into Groups does, prepared without
    // changing it: the root they all end in, the other existing roots it
    // absorbs (sorted) and the nodes of identities not seen before.
    struct Link {
        std::uint32_t root = 0;
        EvidenceVector<std::uint32_t> merged;
        EvidenceVector<Map<DigestBytes, std::uint32_t>::node_type> fresh;

        // SWEGCA: paper/swegca/journal_submission_2026-08-25/COMPONENT_LEDGER.md@5901a5a:44-50
        [[nodiscard]] std::uint32_t remap(std::uint32_t id) const noexcept {
            return std::binary_search(merged.begin(), merged.end(), id) ? root : id;
        }
    };
    struct Group {
        std::uint64_t supports = 0;
        std::uint64_t refutes = 0;
    };
    using ExactSum = unsigned __int128;  // multiples of 2^-84
    struct Axis {
        Map<GroupKey, Group> groups;
        Set<std::uint32_t> sources;
        Set<std::uint32_t> producers;
        ExactSum support = 0;
        ExactSum refute = 0;
    };
    // Per (original, re-evidence generation): whether a consistent result
    // was counted and whether any result conflicted.
    struct CoverKey {
        std::size_t original = 0;
        StateGeneration generation;
        auto operator<=>(const CoverKey&) const = default;
    };
    struct Cover {
        bool consistent = false;
        bool conflicted = false;
    };
    // Per generation: originals observed on it, originals (observed on
    // another generation) re-evidenced on it consistently, and originals with
    // a conflicting result on it.
    struct Coverage {
        std::uint64_t observed = 0;
        std::uint64_t re_evidenced = 0;
        std::uint64_t conflicted = 0;
    };
    // One result per (original, generation, re-evidencer, outcome): an exact
    // repeat is refused (and kept as refused); a changed outcome is recorded.
    struct ResultKey {
        std::size_t original = 0;
        StateGeneration generation;
        std::uint32_t by = 0;
        EvidenceOutcome outcome = EvidenceOutcome::insufficient;
        auto operator<=>(const ResultKey&) const = default;
    };

    [[nodiscard]] std::uint32_t identity_id(const Map<Text, std::uint32_t>& table,
                                            std::string_view text) const;
    [[nodiscard]] static Link prepare_link(Groups& groups, std::span<const DigestBytes> identities);
    static void commit_link(Groups& groups, Link& link) noexcept;
    AdmissionResult reject(std::string_view address, AdmissionResult reason);
    // Called by ReEvidence only, with a result it built from Replay.
    AdmissionResult record(ReEvidenceResult result);

    // First member: every container below takes its allocator from it.
    AllocationContext memory_;
    // Identity of this object; decisions hold it weakly (spec :135).
    std::shared_ptr<const int> origin_;
    ClaimRevision claim_;
    kernel::EvidenceRules rules_;
    Digest256 rules_digest_;
    std::uint32_t recent_window_;
    EvidenceVector<Axis> axes_{memory_.allocator<char>()};
    Groups family_groups_{Map<DigestBytes, std::uint32_t>(memory_.allocator<char>()),
                          EvidenceVector<std::uint32_t>(memory_.allocator<char>()),
                          EvidenceVector<std::uint32_t>(memory_.allocator<char>())};
    Groups context_groups_{Map<DigestBytes, std::uint32_t>(memory_.allocator<char>()),
                           EvidenceVector<std::uint32_t>(memory_.allocator<char>()),
                           EvidenceVector<std::uint32_t>(memory_.allocator<char>())};
    Map<Text, std::uint32_t> producer_ids_{memory_.allocator<char>()};
    // address -> index in admitted_evidence_
    Map<Text, std::size_t> originals_{memory_.allocator<char>()};
    EvidenceVector<AdmittedEvidence> admitted_evidence_{memory_.allocator<char>()};
    Set<std::pair<Text, AdmissionResult>> rejected_{memory_.allocator<char>()};
    Set<std::uint32_t> sources_{memory_.allocator<char>()};   // source group roots
    Set<std::uint32_t> contexts_{memory_.allocator<char>()};  // context group roots
    Set<std::uint32_t> producers_{memory_.allocator<char>()};
    // ring of the last `recent_window_` outcomes
    EvidenceVector<std::uint8_t> recent_{memory_.allocator<char>()};
    std::size_t recent_next_ = 0;
    std::optional<std::uint64_t> earliest_expiry_;
    EvidenceVector<ReEvidenceResult> re_evidence_{memory_.allocator<char>()};
    Map<ResultKey, std::size_t> results_{memory_.allocator<char>()};
    // Positions in re_evidence_ of results that were submitted again exactly.
    Set<std::size_t> repeated_results_{memory_.allocator<char>()};
    Map<CoverKey, Cover> covers_{memory_.allocator<char>()};
    Map<StateGeneration, Coverage> coverage_{memory_.allocator<char>()};
    kernel::EvidenceTally tally_;
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
// generation the journal's HEAD names, both from one snapshot, so neither the
// record nor the current generation is the caller's to choose. A HEAD
// published after that snapshot leaves the admission judged against the
// earlier generation: evidence_current then requires Re-evidence at the new
// one, so the pair is never mixed. The only caller of the accumulator's
// admission step.
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
    // SWEGCA: docs/SWEGCA_VRS_MCP_ORDER_FOR_REVIEW.md@30b73e7:24-29
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
    // SWEGCA: docs/SWEGCA_VRS_MCP_ORDER_FOR_REVIEW.md@30b73e7:24-29
    ReEvidence(const journal::JournalStore& journal, const AllocationContext& memory) noexcept
        : journal_(journal), memory_(memory) {}

    const journal::JournalStore& journal_;  // Main's journal; originals are replayed here
    AllocationContext memory_;          // Main's allocation context; results are kept in it
};

}  // namespace swegca::architecture
