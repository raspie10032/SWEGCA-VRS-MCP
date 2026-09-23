#pragma once

#include "swegca_architecture/digest_bytes.hpp"
#include "swegca_architecture/journal_format.hpp"
#include "swegca_architecture/journal_store.hpp"
#include "swegca_architecture/memory_ledger.hpp"
#include "swegca_architecture/strong_types.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

// Main-owned original experience (board §3B, §4, §5): complete observations
// appended unfiltered to Main's journal under stable digest-bound addresses,
// decoded back exactly, and selected for cognition through the cue view with
// a receipt whose authority is statically none. Nothing here grants
// authority: a record documents, a receipt audits, and neither converts to
// a capability. Main publishes what is staged here; only Main does.
// Rules: board @cefdc3f §3B :116-126, §4 :200-212, §5 :282-286, §9 :592-595;
// L3 mosaic_unrestricted_experience.py@5901a5a (artifacts 23-155, selection
// receipts 158-290, hot index 293-339, discovery/index/selection 342-537).
namespace swegca::architecture {

class MainOwner;

// Journal record kinds this module owns: an original experience, and one
// derived from earlier experience (it names what it was derived from).
inline constexpr std::uint16_t original_experience_kind = 1;
inline constexpr std::uint16_t derived_experience_kind = 2;
inline constexpr std::string_view experience_address_prefix = "experience:";

// Where in its source the raw bytes came from.
struct SourceSpan {
    std::uint64_t offset = 0;
    std::uint64_t length = 0;
};

// One complete observation: the producer's borrowed input. Nothing is
// filtered (no success, verification or file-type test) and every field is
// kept. Before Main appends it, it has no address; appending gives it one
// and still grants nothing (board §4 :200-202).
struct Observation {
    std::string_view source;  // producer id
    std::string_view source_revision;
    std::uint64_t observed_at = 0;  // Main step it was observed at
    std::optional<std::string_view> previous_revision_address;  // revision lineage
    std::span<const std::string_view> derived_from;  // published addresses; empty for an original
    std::optional<std::string_view> outcome;
    double uncertainty = 0;    // [0, 1]
    double contradiction = 0;  // [0, 1]
    std::optional<SourceSpan> source_span;
    std::span<const std::byte> raw;         // the exact bytes observed
    std::span<const std::byte> structured;  // canonical structured form; empty when none
    // Rozephine-authored cues (author: semantic keys by address), each a cue
    // text (journal_format.hpp); the source and its revision, and their
    // tokens under the cue rule, are added automatically.
    std::span<const std::string_view> semantic_cues;
};

// The cue rule (author regex `n\d+|r\d+|[a-z]+|\d+|[^\W\d_]+` over lowered
// text, re-created natively rather than emulated): ASCII letters are
// lowered; a token is `n` or `r` followed by digits, a run of ASCII letters,
// a run of ASCII digits, or a run of letters starting with a non-ASCII
// letter (which continues through ASCII letters). A non-ASCII letter is any
// code point from U+00C0 outside the listed punctuation and symbol blocks;
// everything else separates tokens. Tokens view the lowered copy it keeps.
class CueTokens final {
public:
    // `source` must be strict UTF-8 (every identity text is).
    CueTokens(const MemoryLedger::Account& memory, std::string_view source);
    CueTokens(CueTokens&&) noexcept = default;
    CueTokens& operator=(CueTokens&&) = delete;
    CueTokens(const CueTokens&) = delete;
    CueTokens& operator=(const CueTokens&) = delete;
    ~CueTokens() = default;

    // In text order; a token may repeat.
    // SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:487-490
    [[nodiscard]] std::span<const std::string_view> tokens() const noexcept { return tokens_; }

private:
    journal::LedgerBytes text_;  // the lowered text; tokens view it
    journal::LedgerVector<std::string_view> tokens_;
};

// One experience record read back: the published record Main replayed and
// the experience fields decoded from its payload. Every text and span it
// hands out views the record's bytes and is valid while this object lives.
class ExperienceRecord final {
public:
    // Requires an experience kind without authority or claim, a well-formed
    // payload, and an address that is the digest of the record's identity.
    // Its lineage list is kept on `memory`.
    [[nodiscard]] static ExperienceRecord decode(journal::PublishedRecord record,
                                                 const MemoryLedger::Account& memory);

    // The source keeps nothing that views the bytes it gave away (like
    // PublishedRecord).
    ExperienceRecord(ExperienceRecord&& other) noexcept;
    ExperienceRecord& operator=(ExperienceRecord&&) = delete;
    ExperienceRecord(const ExperienceRecord&) = delete;
    ExperienceRecord& operator=(const ExperienceRecord&) = delete;
    ~ExperienceRecord() = default;

    // SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:23-60
    [[nodiscard]] const journal::RecordView& record() const noexcept { return record_.view(); }
    // SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:23-60
    [[nodiscard]] const journal::RecordPosition& position() const noexcept { return record_.position(); }
    // SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:23-60
    [[nodiscard]] bool derived() const noexcept {
        return record_.view().kind == derived_experience_kind;
    }
    // SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:23-60
    [[nodiscard]] std::uint64_t observed_at() const noexcept { return observed_at_; }
    // SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:179-205
    [[nodiscard]] double uncertainty() const noexcept { return uncertainty_; }
    // SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:179-205
    [[nodiscard]] double contradiction() const noexcept { return contradiction_; }
    // SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:23-60
    [[nodiscard]] const std::optional<SourceSpan>& source_span() const noexcept { return span_; }
    // SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:23-60
    [[nodiscard]] std::span<const std::string_view> derived_from() const noexcept {
        return derived_from_;
    }
    // SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:23-60
    [[nodiscard]] std::span<const std::byte> raw() const noexcept { return raw_; }
    // SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:23-60
    [[nodiscard]] std::span<const std::byte> structured() const noexcept { return structured_; }

private:
    ExperienceRecord(journal::PublishedRecord record, journal::LedgerVector<std::string_view> derived);

    journal::PublishedRecord record_;
    std::uint64_t observed_at_ = 0;
    double uncertainty_ = 0;
    double contradiction_ = 0;
    std::optional<SourceSpan> span_;
    journal::LedgerVector<std::string_view> derived_from_;  // views the record's bytes
    std::span<const std::byte> raw_;
    std::span<const std::byte> structured_;
};

// What appending staged: the generation Main publishes (absent when every
// observation was already in the journal, which is then not appended again)
// and one address per observation, in input order.
struct ExperienceAppend {
    std::optional<journal::StagedGeneration> staged;
    journal::LedgerVector<ExperienceAddress> addresses;
};

// Main's original-experience journal (board §5 :282-283): stages complete
// observations as records under digest-bound addresses and replays them.
// The address is the digest of the record's identity (kind, source,
// revision, lineage, outcome and payload digest), so the same observation
// appended twice has one address and one record.
class ExperienceJournal final {
public:
    ExperienceJournal(const ExperienceJournal&) = delete;
    ExperienceJournal& operator=(const ExperienceJournal&) = delete;
    ExperienceJournal(ExperienceJournal&&) = delete;
    ExperienceJournal& operator=(ExperienceJournal&&) = delete;
    ~ExperienceJournal() = default;

    // Validates every observation (texts by the identity rule, uncertainty
    // and contradiction finite in [0, 1], every lineage address published),
    // encodes it, and stages the new ones as one generation on `state` with
    // `views`, under Main's `operation_id`. Stages nothing when all exist.
    [[nodiscard]] ExperienceAppend stage(std::span<const Observation> observations,
                                         std::string_view operation_id, const StateGeneration& state,
                                         std::span<const journal::ViewGeneration> views) const;

    // Replays one exact experience (`journal_address_unknown` when absent).
    [[nodiscard]] ExperienceRecord replay(const ExperienceAddress& address) const;

    // The state generation named by the current published journal HEAD.
    [[nodiscard]] StateGeneration state_generation() const;

private:
    friend class MainOwner;
    friend class ExperienceSelector;
    // SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@cefdc3f:116-126
    ExperienceJournal(const journal::JournalStore& journal, const MemoryLedger::Account& memory) noexcept
        : journal_(journal), memory_(memory) {}

    const journal::JournalStore& journal_;
    MemoryLedger::Account memory_;
};

// q of Select(q, U): the query text, tokenized by the cue rule, and the
// digest of the context the query was asked in.
struct SelectionQuery {
    std::string_view text;
    Digest256 context;
};

// What the judge sees of one candidate: its address, exact position and how
// many distinct query cues retrieved it. The address lives for the call only.
struct SelectionCandidate {
    std::string_view address;
    journal::RecordPosition position;
    std::uint32_t matched_cues = 0;
};

// One judgment of a retrieved candidate (author ExperienceCandidateJudgment
// :179-205), as the judge returns it: borrowed, and copied at once.
struct CandidateVerdict {
    std::string_view address;
    bool selected = false;
    double relevance = 0;      // [0, 1]
    double contradiction = 0;  // [0, 1]
    std::string_view verification_state;
    std::string_view revision;
    std::string_view rationale;
    std::span<const std::string_view> rejection_evidence;  // each nonempty
};

// The runtime cognition judgment over one candidate, borrowed for one
// selection like a function reference (pass it directly; never keep one).
class SelectionJudge final {
public:
    // SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:475-512
    template <class F>
        requires(!std::is_same_v<std::remove_cvref_t<F>, SelectionJudge> &&
                 std::is_invocable_r_v<CandidateVerdict, std::remove_reference_t<F>&,
                                       const SelectionCandidate&, const SelectionQuery&>)
    SelectionJudge(F&& judge) noexcept  // NOLINT(google-explicit-constructor)
        : target_(static_cast<const void*>(std::addressof(judge))),
          call_(&invoke<std::remove_reference_t<F>>) {}

    // SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:508-512
    CandidateVerdict operator()(const SelectionCandidate& candidate,
                                const SelectionQuery& query) const {
        return call_(target_, candidate, query);
    }

private:
    using Call = CandidateVerdict (*)(const void*, const SelectionCandidate&, const SelectionQuery&);
    // SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:508-512
    template <class T>
    static CandidateVerdict invoke(const void* target, const SelectionCandidate& candidate,
                                   const SelectionQuery& query) {
        auto& judge = *static_cast<T*>(const_cast<void*>(target));
        return static_cast<CandidateVerdict>(judge(candidate, query));
    }

    const void* target_;
    Call call_;
};

struct VerificationStateTag { static constexpr std::string_view name = "verification_state"; };
struct RevisionTextTag { static constexpr std::string_view name = "revision"; };
struct RationaleTag { static constexpr std::string_view name = "rationale"; };
struct EvidenceTextTag { static constexpr std::string_view name = "rejection_evidence"; };
struct QueryTextTag { static constexpr std::string_view name = "query"; };
using VerificationState = TextIdentity<VerificationStateTag>;
using RevisionText = TextIdentity<RevisionTextTag>;
using Rationale = TextIdentity<RationaleTag>;
using EvidenceText = TextIdentity<EvidenceTextTag>;
using QueryText = TextIdentity<QueryTextTag>;

// J: one kept judgment, on Main's ledger.
struct CandidateJudgment {
    ExperienceAddress address;
    journal::RecordPosition position;
    std::uint32_t matched_cues = 0;
    bool selected = false;
    double relevance = 0;
    double contradiction = 0;
    VerificationState verification_state;
    RevisionText revision;
    Rationale rationale;
    journal::LedgerVector<EvidenceText> rejection_evidence;
};

// C: one selected experience with its replay handle (exact position) and
// what Replay verified of it (author selected artifact :520-529).
struct SelectedExperience {
    ExperienceAddress address;
    journal::RecordPosition position;
    std::uint64_t byte_count = 0;  // raw bytes of the experience
    DigestBytes record_digest{};
    VerificationState verification_state;
    RevisionText revision;
};

// U: the published journal generation a selection ran over.
using SelectionUniverse = journal::PublishedUniverse;

// The static authority of a selection receipt: none. It is the only
// authority a receipt is defined for, and nothing converts it to another.
struct NoAuthority final {
    static constexpr bool external_action_authorized = false;
    static constexpr bool memory_write_authorized = false;
    static constexpr bool world_write_authorized = false;
    static constexpr bool training_write_authorized = false;
    static constexpr bool p3_promotion_authorized = false;
};

template <class Authority>
class SelectionReceipt;  // defined for NoAuthority only (board §4 :207-210)

class ExperienceSelector;

// rho: the replayable receipt of Rozephine judging every retrieved candidate
// (author RuntimeExperienceSelectionReceipt :208-290). It audits; it
// authorizes nothing, and its digest covers every field.
template <>
class SelectionReceipt<NoAuthority> final {
public:
    using Authority = NoAuthority;

    SelectionReceipt(SelectionReceipt&&) noexcept = default;
    SelectionReceipt& operator=(SelectionReceipt&&) = delete;
    SelectionReceipt(const SelectionReceipt&) = delete;
    SelectionReceipt& operator=(const SelectionReceipt&) = delete;
    ~SelectionReceipt() = default;

    // SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:208-243
    [[nodiscard]] std::string_view query() const noexcept { return query_.value(); }
    // SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:208-243
    [[nodiscard]] const Digest256& context() const noexcept { return context_; }
    // SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:208-243
    [[nodiscard]] const SelectionUniverse& universe() const noexcept { return universe_; }
    // SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:208-243
    [[nodiscard]] std::span<const CandidateJudgment> judgments() const noexcept { return judgments_; }
    // SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:208-243
    [[nodiscard]] std::span<const SelectedExperience> selected() const noexcept { return selected_; }
    // SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:266-272
    [[nodiscard]] static constexpr std::string_view method() noexcept {
        return "runtime_cognition_relevance_and_contradiction_judgment";
    }
    // SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:530-532
    [[nodiscard]] static constexpr std::string_view rationale() noexcept {
        return "Rozephine selected relevant experience after explicit contradiction review";
    }
    // SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:274-290
    [[nodiscard]] const Digest256& digest() const noexcept { return digest_; }

private:
    friend class ExperienceSelector;
    SelectionReceipt(QueryText query, const Digest256& context, const SelectionUniverse& universe,
                     journal::LedgerVector<CandidateJudgment> judgments,
                     journal::LedgerVector<SelectedExperience> selected);
    [[nodiscard]] Digest256 compute_digest() const;

    QueryText query_;
    Digest256 context_;
    SelectionUniverse universe_;
    journal::LedgerVector<CandidateJudgment> judgments_;  // in address order
    journal::LedgerVector<SelectedExperience> selected_;  // the selected judgments, in order
    Digest256 digest_;
};

// Main's limits on one selection. More retrieved entries than
// `max_retrieved` fail closed (`experience_select_over_policy`) rather than
// being cut: no retrieved candidate is ever dropped unjudged.
struct SelectionPolicy {
    std::uint32_t max_retrieved = 1u << 16;
};

// Select(q, U) -> (C, J, rho) (board §3B :124-126). Retrieval derives
// candidates from the query's cues through the cue view of the published
// journal U (never by scanning every experience, board §9 :592-595); the
// judge, Main's runtime cognition, judges every candidate; each selected one
// is replayed exactly and verified. There is no fallback to the whole
// universe: no candidate fails with `experience_select_no_candidates`, and
// no selection with `experience_select_nothing_selected`, as the author's.
class ExperienceSelector final {
public:
    ExperienceSelector(const ExperienceSelector&) = delete;
    ExperienceSelector& operator=(const ExperienceSelector&) = delete;
    ExperienceSelector(ExperienceSelector&&) = delete;
    ExperienceSelector& operator=(ExperienceSelector&&) = delete;
    ~ExperienceSelector() = default;

    [[nodiscard]] SelectionReceipt<NoAuthority> select(const SelectionQuery& query,
                                                       SelectionJudge judge) const;

private:
    friend class MainOwner;
    // SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@cefdc3f:116-126
    ExperienceSelector(const ExperienceJournal& experience, SelectionPolicy policy) noexcept
        : experience_(experience), policy_(policy) {}

    const ExperienceJournal& experience_;
    SelectionPolicy policy_;
};

}  // namespace swegca::architecture
