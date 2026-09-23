#pragma once

#include "swegca_vrs/journal_file_io.hpp"
#include "swegca_vrs/journal_extent_index.hpp"
#include "swegca_vrs/journal_format.hpp"
#include "swegca_vrs/journal_position.hpp"
#include "swegca_vrs/authority_roles.hpp"
#include "swegca_vrs/allocation.hpp"
#include "swegca_vrs/identity_types.hpp"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <span>
#include <stdexcept>
#include <string_view>
#include <type_traits>

// Main-owned native journal directory (format v8): exclusive owner lock, detached
// staging, one serialized publisher, snapshot readers (each read holds one
// immutable published snapshot for its whole duration and takes no journal
// lock; the atomic snapshot pointer itself is not promised lock-free),
// fail-closed recovery from the published HEAD only, and the derived
// exact-address and index views published by the same HEAD, compacted and
// rebuildable from the records.
// Rule: SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md §3B, §9; one
// current generation (I01), provenance-bound and recoverable persistent
// mutation (I07), ARCHITECTURE_SPEC.md@5901a5a:205-207,214-216.
// re-created (user@2026-09-23): replaces SQLite, which the user removed.
// Resources (codex J8, J9, J13; user 2026-09-23 16:10): the host counts and
// judges. Every byte kept on disk is allowed by the host's StorageBudget
// before it is written, and every buffer and container the journal
// allocates goes through the host's AllocationContext, which sees its exact
// requested size. Not reached: path strings, exception objects, OS
// handles and the caller's own inputs (drafts,
// views); Main integration closes those.
namespace swegca::vrs {
class ExperienceAppend;
}

namespace swegca::vrs::journal {

class JournalStore;

// The unique owner keeps the host allocator alive through store destruction.
// A nullable owner is represented by optional<JournalStoreOwner> at the host
// boundary; constructing one without a host allocator is not valid.
class JournalStoreDeleter final {
public:
    // Lineage: native mechanism — host-owned storage for the journal object.
    // SWEGCA: user@2026-09-22:89-92
    void operator()(JournalStore* store) noexcept;

private:
    // Lineage: native mechanism — bind deallocation to the host allocator used at construction.
    // SWEGCA: user@2026-09-22:89-92
    explicit JournalStoreDeleter(AllocationAdapter<JournalStore> allocation) noexcept
        : allocation_(allocation) {}
    friend class JournalStore;
    AllocationAdapter<JournalStore> allocation_;
};
using JournalStoreOwner = std::unique_ptr<JournalStore, JournalStoreDeleter>;

// One published generation as readers see it. Immutable once published; the
// object and its control block are allocated through the host's allocator.
struct PublishedSnapshot {
    // Lineage: weak analogy — the author's snapshot pairs Main hot memory with a VRS id; here one journal generation.
    // SWEGCA: src/tinylm_slicer/mosaic_memory_activation.py@3bddcb7:462-468
    PublishedSnapshot(const AllocationContext& memory, Manifest manifest)
        : head(std::move(manifest)), extents(memory) {}

    Manifest head;
    ManifestLocation location;
    ExtentIndex extents;
    std::uint64_t storage = 0;  // charged on-disk use of the files this generation reaches
    // Lease on the page logs this generation's view reaches, shared by every
    // generation until the view is rewritten into new logs. Logs a rewrite
    // left behind are removed only once their lease has no holder.
    std::shared_ptr<const void> page_logs;
    // The view is derived from the records. Set when open found its page
    // logs damaged: lookups, stages and compaction fail closed with
    // `journal_view_unavailable` until `rebuild_view` publishes a new view.
    bool view_unavailable = false;
};

// What one published generation holds, from one snapshot: its number and
// manifest digest, and its records' count and bytes (the universe a read
// ran over). Both counts cover the whole journal, every record kind:
// `record_count` is the tail sequence (sequences run 1..record_count) and
// `record_bytes` the bytes of every segment extent, headers included.
struct PublishedUniverse {
    std::uint64_t generation = 0;
    Digest manifest_digest{};
    std::uint64_t record_count = 0;
    std::uint64_t record_bytes = 0;
};

// One published record read back: its exact bytes, allocated through the
// host's allocator, and the view decoded from them. The view, and every text and span
// it hands out, is valid while this object lives. It can only be moved
// (moving carries the byte buffer and the view to the new object and leaves
// the source with an empty view and position, so a moved-from record never
// exposes bytes it no longer owns); it cannot be assigned, copied, or have
// its bytes taken out (codex J14, codex 14:31).
class PublishedRecord final {
public:
    PublishedRecord(PublishedRecord&& other) noexcept;
    PublishedRecord& operator=(PublishedRecord&&) = delete;
    PublishedRecord(const PublishedRecord&) = delete;
    PublishedRecord& operator=(const PublishedRecord&) = delete;
    ~PublishedRecord() = default;

    // Lineage: native mechanism — a lifetime-bound accessor: the view is valid only while its record owns the bytes.
    // SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@cefdc3fce8b5c605166d668924baa5d4a6c49dc0:50
    [[nodiscard]] const RecordView& view() const& noexcept { return view_; }
    const RecordView& view() const&& = delete;
    // Lineage: native mechanism — exposes the exact position (segment, offset, sequence, digest) naming the record.
    // SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@cefdc3fce8b5c605166d668924baa5d4a6c49dc0:569-570
    [[nodiscard]] const RecordPosition& position() const noexcept { return position_; }

private:
    friend class JournalStore;
    // Decodes `bytes` and requires the record `position` names.
    PublishedRecord(LedgerBytes bytes, const RecordPosition& position);

    LedgerBytes bytes_;
    RecordView view_;
    RecordPosition position_;
};

// Borrowed exact-address reader for owner validation during a view rebuild.
// It reads from the unpublished rebuilt address tree and the published record
// extents. The callable and this adapter live only through rebuild_view.
// Weak source analogy: the author's cold block loader exposes a result only
// after the complete layout validates. This address reader is additional C++
// journal-view infrastructure, not that loader or a prior Python type.
// Contract: docs/SWEGCA_CPP_MAIN_STATE_STORAGE_REVIEW.md.
class RebuildReader final {
public:
    RebuildReader(const RebuildReader&) = delete;
    RebuildReader& operator=(const RebuildReader&) = delete;
    RebuildReader(RebuildReader&&) = delete;
    RebuildReader& operator=(RebuildReader&&) = delete;
    template <class R, class Q>
        requires(std::is_lvalue_reference_v<R&&> && std::is_lvalue_reference_v<Q&&> &&
                 std::is_object_v<std::remove_reference_t<R>> &&
                 std::is_object_v<std::remove_reference_t<Q>> &&
                 std::is_invocable_r_v<PublishedRecord, R&, std::string_view> &&
                 std::is_invocable_r_v<std::optional<RecordPosition>, Q&,
                                       std::string_view>)
    // Lineage: native mechanism — type-erased borrowed reader so rebuild validation reads the unpublished view.
    // SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@cefdc3fce8b5c605166d668924baa5d4a6c49dc0:589-590
    RebuildReader(R&& replay, Q&& resolve) noexcept
        : target_(static_cast<const void*>(std::addressof(replay))),
          call_(&invoke<R>),
          resolve_target_(static_cast<const void*>(std::addressof(resolve))),
          resolve_call_(&invoke_resolve<Q>) {}

    // Lineage: native mechanism — reads an original through the unpublished rebuilt address view.
    // SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@cefdc3fce8b5c605166d668924baa5d4a6c49dc0:589-590
    [[nodiscard]] PublishedRecord replay(std::string_view address) const {
        return call_(target_, address);
    }

    // Resolves against the unpublished rebuilt address tree, not caller
    // supplied positions. The returned position must be compared exactly.
    // Lineage: native mechanism — resolves against the unpublished rebuilt address view for exact comparison.
    // SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@cefdc3fce8b5c605166d668924baa5d4a6c49dc0:589-590
    [[nodiscard]] std::optional<RecordPosition> resolve(std::string_view address) const {
        return resolve_call_(resolve_target_, address);
    }

private:
    using Call = PublishedRecord (*)(const void*, std::string_view);
    using ResolveCall = std::optional<RecordPosition> (*)(const void*, std::string_view);
    // Lineage: native mechanism — call trampoline of the type-erased borrowed reader; allocates nothing.
    // SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@cefdc3fce8b5c605166d668924baa5d4a6c49dc0:50
    template <class F>
    static PublishedRecord invoke(const void* target, std::string_view address) {
        auto& replay = *static_cast<std::remove_reference_t<F>*>(const_cast<void*>(target));
        return replay(address);
    }

    // Lineage: native mechanism — call trampoline of the type-erased borrowed resolver; allocates nothing.
    // SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@cefdc3fce8b5c605166d668924baa5d4a6c49dc0:50
    template <class Q>
    static std::optional<RecordPosition> invoke_resolve(const void* target,
                                                         std::string_view address) {
        auto& resolve = *static_cast<std::remove_reference_t<Q>*>(const_cast<void*>(target));
        return resolve(address);
    }

    const void* target_;
    Call call_;
    const void* resolve_target_;
    ResolveCall resolve_call_;
};

// Borrowed Main-owned decoder for the second rebuild pass. It checks each
// record kind and may replay prior records through the rebuilt address tree.
// It must not reenter JournalStore publication while rebuild_view holds the
// publication lock.
// Weak source analogy: complete-layout validation precedes exposure in the
// author's cold block loader. Validation of journal records before rebuilding
// a derived address view is additional C++ infrastructure with no direct
// Python type counterpart.
class RebuildValidator final {
public:
    RebuildValidator(const RebuildValidator&) = delete;
    RebuildValidator& operator=(const RebuildValidator&) = delete;
    RebuildValidator(RebuildValidator&&) = delete;
    RebuildValidator& operator=(RebuildValidator&&) = delete;
    // Lineage: native mechanism — type-erased borrowed validator for the second pass of a view rebuild.
    // SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@cefdc3fce8b5c605166d668924baa5d4a6c49dc0:589-590
    template <class F>
        requires(!std::is_same_v<std::remove_cvref_t<F>, RebuildValidator> &&
                 std::is_object_v<F> &&
                 std::is_invocable_v<F&, const RecordView&,
                                     const RecordPosition&, const RebuildReader&>)
    explicit RebuildValidator(F& validate) noexcept
        : target_(static_cast<const void*>(std::addressof(validate))),
          call_(&invoke<F>) {}
    template <class F>
    RebuildValidator(const F&&) = delete;

    // Lineage: native mechanism — hands each verified record to Main's validator before the rebuilt view is published.
    // SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@cefdc3fce8b5c605166d668924baa5d4a6c49dc0:589-590
    void operator()(const RecordView& record, const RecordPosition& position,
                    const RebuildReader& reader) const {
        call_(target_, record, position, reader);
    }

private:
    using Call = void (*)(const void*, const RecordView&, const RecordPosition&,
                          const RebuildReader&);
    // Lineage: native mechanism — call trampoline of the type-erased borrowed validator; allocates nothing.
    // SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@cefdc3fce8b5c605166d668924baa5d4a6c49dc0:50
    template <class F>
    static void invoke(const void* target, const RecordView& record,
                       const RecordPosition& position, const RebuildReader& reader) {
        auto& validate = *static_cast<F*>(const_cast<void*>(target));
        validate(record, position, reader);
    }

    const void* target_;
    Call call_;
};

// One exact original replayed within one published snapshot, with the
// state head that snapshot's manifest names: both belong to the
// same generation, whatever is published meanwhile.
struct ReplayAtHead {
    PublishedRecord record;
    StateHeadReference head;
};

// Bytes one generation places in one segment file: appended at the published
// length of the tail segment, or a new file starting with its header.
struct SegmentPiece {
    std::uint64_t ordinal = 0;
    std::uint64_t file_id = 0;
    bool new_file = false;
    std::uint64_t offset = 0;
    LedgerBytes bytes;
};

// Pages one generation places in one page log: appended at its published
// end, or a new log starting with its header. `parts` lists what is written,
// in order, viewing `header` and `pages`.
struct PagePiece {
    std::uint64_t log_ordinal = 0;
    bool new_file = false;
    std::uint64_t offset = 0;
    LedgerBytes header;
    LedgerVector<LedgerBytes> pages;
    LedgerVector<std::span<const std::byte>> parts;
};

// A detached, unpublished successor generation. It is one-use: publishing or
// moving from it leaves the source invalid, and an invalid staged generation
// can never be published. It already holds the complete next snapshot and
// every byte to write, so publishing allocates no data (codex J13).
class StagedGeneration final {
public:
    StagedGeneration(StagedGeneration&& other) noexcept;
    StagedGeneration& operator=(StagedGeneration&&) = delete;
    StagedGeneration(const StagedGeneration&) = delete;
    StagedGeneration& operator=(const StagedGeneration&) = delete;
    ~StagedGeneration() = default;

    // Lineage: native mechanism — one-use flag: a detached generation can be published at most once.
    // SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@cefdc3fce8b5c605166d668924baa5d4a6c49dc0:587-589
    [[nodiscard]] bool valid() const noexcept { return valid_; }
    // The manifest it would publish; an invalid staged generation has none
    // (`journal_staged_generation_invalid`).
    // Lineage: weak analogy — the author's save yields a candidate for Main's CAS; here a one-use staged manifest.
    // SWEGCA: src/tinylm_slicer/mosaic_vrs_block_store.py@3bddcb7:134-135
    [[nodiscard]] const Manifest& manifest() const {
        if (!valid_) throw std::logic_error("journal_staged_generation_invalid");
        return next_->head;
    }
    // Lineage: native mechanism — the exact positions (segment, offset, sequence, digest) the staged records will take.
    // SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@cefdc3fce8b5c605166d668924baa5d4a6c49dc0:569-570
    [[nodiscard]] std::span<const RecordPosition> positions() const noexcept {
        return positions_;
    }

private:
    friend class JournalStore;
    explicit StagedGeneration(const AllocationContext& memory);

    bool valid_ = false;
    std::uint64_t parent_generation_ = 0;
    Digest parent_manifest_digest_{};
    LedgerBytes head_bytes_;
    LedgerBytes log_header_;  // only when a new manifest log starts
    LedgerVector<SegmentPiece> pieces_;
    LedgerVector<PagePiece> page_pieces_;
    LedgerVector<RecordPosition> positions_;
    std::shared_ptr<PublishedSnapshot> next_;
};

// The host's storage budget (user 2026-09-23 16:10 via codex): the host
// counts and judges storage, the journal only asks. `allows(used)` answers
// whether the journal may use `used` bytes in all (published generation,
// logs a rewrite left behind, and what a write in progress adds). It must
// not throw, and must be safe to call from any thread the journal writes on.
// Lineage: native mechanism — a host port that makes the hard disk budget executable: the journal asks, the host judges.
// SWEGCA: docs/SWEGCA_CPP_VRS_LAYER_PLAN.md@472d23225c973fa0a33581afd6bd9026df6fc98a:170-176
class StorageBudget {
public:
    virtual ~StorageBudget() = default;
    [[nodiscard]] virtual bool allows(std::uint64_t used) const noexcept = 0;
};

// Pages of the derived views kept for the read path (journal_store.cpp).
class PageCache;

// The visitor of one index lookup, borrowed like a function reference: pass
// a callable object directly (a lambda or functor, not a plain function) and
// never keep one; nothing is allocated for it. It returns false to stop.
class IndexVisitor final {
public:
    // Lineage: native mechanism — type-erased borrowed index visitor; nothing is allocated for it.
    // SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@cefdc3fce8b5c605166d668924baa5d4a6c49dc0:50
    template <class F>
        requires(!std::is_same_v<std::remove_cvref_t<F>, IndexVisitor> &&
                 std::is_object_v<F> &&
                 std::is_invocable_r_v<bool, F&, std::string_view,
                                       const RecordPosition&>)
    IndexVisitor(F& visit) noexcept  // NOLINT(google-explicit-constructor)
        : target_(static_cast<const void*>(std::addressof(visit))),
          call_(&invoke<F>) {}
    template <class F>
    IndexVisitor(const F&&) = delete;

    // Lineage: native mechanism — calls the borrowed visitor for one index match.
    // SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@cefdc3fce8b5c605166d668924baa5d4a6c49dc0:50
    bool operator()(std::string_view address, const RecordPosition& position) const {
        return call_(target_, address, position);
    }

private:
    using Call = bool (*)(const void*, std::string_view, const RecordPosition&);
    // Lineage: native mechanism — call trampoline of the type-erased borrowed visitor.
    // SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@cefdc3fce8b5c605166d668924baa5d4a6c49dc0:50
    template <class T>
    static bool invoke(const void* target, std::string_view address, const RecordPosition& position) {
        auto& visit = *static_cast<T*>(const_cast<void*>(target));
        return static_cast<bool>(visit(address, position));
    }

    const void* target_;
    Call call_;
};

// Only ExperienceAppend can form this key. It permits staging verified
// experience kinds without granting the publication methods to that class.
class ExperienceStageKey final {
public:
    ExperienceStageKey(const ExperienceStageKey&) = delete;
    ExperienceStageKey& operator=(const ExperienceStageKey&) = delete;
    ~ExperienceStageKey() = default;

private:
    ExperienceStageKey() = default;
    friend class swegca::vrs::ExperienceAppend;
};

// Only Main can form this key. It admits reserved state record kinds to
// detached staging; publication still requires Main's owner authority.
class StateStageKey final {
public:
    StateStageKey(const StateStageKey&) = delete;
    StateStageKey& operator=(const StateStageKey&) = delete;
    ~StateStageKey() = default;

private:
    StateStageKey() = default;
    friend class swegca::vrs::MainOwner;
};

// Main's publication root names one journal generation by where its
// manifest was published and that manifest's digest. The journal treats the
// pair as a storage identity chosen by Main, never as something it
// discovers (tinylm mosaic_vrs_event_durable.py@3bddcb7:3-5, :152).
struct JournalRoot {
    ManifestLocation location;
    Digest manifest_digest{};
};

class JournalStore final {
public:
    // This lower-journal open still adopts its own HEAD and removes or cuts
    // unpublished tails. It is not the Main-owned durable publication-root
    // recovery API; `open_at_root` below opens the generation Main's root
    // names and keeps and charges the bytes outside it. Writing after such a
    // rollback (reconciling the kept bytes, sealed tail and logs) remains
    // required before product use.
    // Opens `directory`, creating it atomically when it does not exist
    // (generation 0 is built in a sibling directory and renamed into place
    // without replacement). An existing directory must be exactly a journal;
    // these fail closed: unknown entries (including a `.part` file that is
    // not an interrupted publication of a journal name), a missing or
    // corrupt HEAD or manifest on the chain back to the checkpoint, a segment
    // no extent names, a missing or corrupt extent the head generation
    // wrote, a recovery chain over its read budget, and published use over
    // what the host's `storage` allows. The view is derived: a missing page log in its range,
    // or a last log that cannot be cut back to its published end, opens the
    // journal with the view unavailable (lookups, stages and compaction fail
    // with `journal_view_unavailable`) so that Main can `rebuild_view` from
    // the originals. Unpublished
    // leftovers of an interrupted publication and page logs the view no
    // longer reaches are removed. Opening reads HEAD and the manifest chain
    // back to its checkpoint (at most `max_recovery_bytes`) and verifies the
    // extents the head generation wrote (at most two segments) plus the
    // selected tail when those extents omit it. Older segments (their
    // presence and bytes) and view pages are verified when
    // read: `replay` checks every page digest on the way and the record
    // digest the view names; `for_each_record` and `rebuild_view` check the
    // whole record chain.
    // `identity` must satisfy the identity rule; the store keeps its own copy
    // on `memory`. `page_cache`, when given, is the context the verified
    // view pages lookups keep are allocated through (see PageCache); the
    // host's refusal there evicts. The cache is split into
    // `page_cache_shards` independently locked shards (the host sets it to
    // its worker count; nonzero when a cache is kept). Every budget is the
    // host's: the journal counts no resource and fixes no limit. The store
    // shares ownership of `storage` (non-null, `journal_budget_missing`), so
    // the budget lives as long as the store whatever order the host tears
    // down in (codex 17:04). The cache is bounded only by `page_cache`: the
    // host must refuse (AllocationRefused) past the budget it gives it.
    [[nodiscard]] static JournalStoreOwner open(
        const std::filesystem::path& directory, std::string_view identity,
        std::shared_ptr<const StorageBudget> storage, const AllocationContext& memory,
        const std::optional<AllocationContext>& page_cache, std::size_t page_cache_shards);

    // Opens the generation `root` names (Main's choice; the journal's own
    // HEAD file is neither read nor changed). Nothing is created, removed or
    // cut. Before it is used the chosen generation is checked like `open`
    // checks its HEAD: the manifest at `root.location` must have
    // `root.manifest_digest` and this identity, the manifest chain back to
    // its checkpoint must hold, every segment it names must exist with at
    // least its published length, and the extents it wrote plus its tail
    // (for a checkpoint generation, its last two extents, since a generation
    // writes at most two) are verified record by record. Every byte outside
    // the chosen generation is kept and charged to `storage` with the
    // published files: segments it does not name above its tail, the tail's
    // bytes past its published end, manifest logs after its log and bytes
    // past its manifest, page logs outside its view's range and bytes past
    // the last one's end, bytes added to earlier logs, HEAD and the lock
    // beyond their sizes, and `.part` files. These fail closed, removing
    // nothing: a missing directory or owner.lock, an entry that is not a
    // regular file when the directory is listed (a link entry is refused,
    // not followed; links in `directory` and above are), a digest or chain
    // mismatch, a segment below the tail that no extent names, a segment
    // other than the tail longer than published, and a total over the
    // budget. A page log missing in the view's range, or a last one shorter
    // than its published end, leaves the view unavailable.
    // The store is read-only: stage, publish, compact_view and rebuild_view
    // fail with `journal_root_read_only`. How writing resumes past kept
    // bytes (reconciling them, sealed logs) is not decided yet (codex 22:53);
    // until then this refuses writes the way the user's commit path refuses
    // them while an unreconciled marker is retained.
    [[nodiscard]] static JournalStoreOwner open_at_root(
        const std::filesystem::path& directory, std::string_view identity,
        std::shared_ptr<const StorageBudget> storage, const AllocationContext& memory,
        const std::optional<AllocationContext>& page_cache, std::size_t page_cache_shards,
        const JournalRoot& root);

    JournalStore(JournalStore&&) = delete;
    JournalStore(const JournalStore&) = delete;
    JournalStore& operator=(const JournalStore&) = delete;
    JournalStore& operator=(JournalStore&&) = delete;
    ~JournalStore();

    // The published head manifest, shared with its snapshot: no copy is made
    // and the snapshot lives as long as the returned pointer.
    [[nodiscard]] std::shared_ptr<const Manifest> head() const;
    [[nodiscard]] StateHeadReference state_head() const;
    // Charged on-disk use of the published journal, in bytes, including page
    // logs a view rewrite left behind that are not yet removed.
    [[nodiscard]] std::uint64_t storage_charged() const;

    // Builds a detached successor of the current head: the records, the
    // exact-address view pages for them (addresses must be new and unique),
    // the complete next snapshot and the HEAD bytes. Disk use is checked
    // against the host's storage budget before any record is encoded and again,
    // with the view pages, before anything is written. Fails with
    // `journal_generation_too_large`, `journal_address_duplicate`,
    // `journal_storage_budget_exhausted`, or the host's allocation refusal.
    // Records of the memory/cue kinds (original, derived, part, cue binding)
    // are staged only through ExperienceJournal, which derives a derived
    // record's root sources and contexts from its published lineage; here
    // they fail `journal_experience_kind_reserved`. State kinds 5–7 require
    // Main's StateStageKey and fail `journal_state_kind_reserved` here.
    // This route inherits the parent's state head; a generic record cannot
    // name a different Cognitive State publication.
    [[nodiscard]] StagedGeneration stage(std::span<const RecordDraft> drafts,
                                         std::span<const ViewGeneration> views) const;

    // The experience appender may stage its reserved record kinds but cannot
    // publish or rewrite HEAD. Only MainOwner can perform those mutations.
    // It also inherits the parent's state head without a caller-supplied one.
    // Lineage: native mechanism — the plan's three experience kinds, plus cue bindings (this code's addition), only through ExperienceAppend.
    // SWEGCA: docs/SWEGCA_CPP_VRS_LAYER_PLAN.md@472d23225c973fa0a33581afd6bd9026df6fc98a:159-165
    [[nodiscard]] StagedGeneration stage_experience_records(
        const ExperienceStageKey&, std::span<const RecordDraft> drafts,
        std::span<const ViewGeneration> views) const {
        for (const auto& draft : drafts)
            if (draft.kind != original_experience_record_kind &&
                draft.kind != derived_experience_record_kind &&
                draft.kind != experience_part_record_kind &&
                draft.kind != cue_binding_record_kind)
                throw std::invalid_argument("journal_memory_kind_required");
        return stage_records(drafts, views, std::nullopt);
    }

    // Main alone may stage the reserved state kinds. This key excludes other
    // callers, but does not narrow MainOwner's existing JournalStore friend
    // rights or grant a state-write capability. State codecs and final
    // publication remain Main storage work. With no override, part and
    // candidate-publication generations keep their parent's state head;
    // only the final Main publication supplies a replacement head.
    // Lineage: native mechanism — reserved state staging follows the sole Main-owned state rule; kind numbers and this key are C++ storage choices.
    // SWEGCA: paper/swegca/ARCHITECTURE_SPEC.md@5901a5a:17
    [[nodiscard]] StagedGeneration stage_state_records(
        const StateStageKey&, std::span<const RecordDraft> drafts,
        std::optional<StateHeadReference> state_override,
        std::span<const ViewGeneration> views) const {
        for (const auto& draft : drafts)
            if (draft.kind != state_part_record_kind && draft.kind != state_root_record_kind &&
                draft.kind != state_publication_record_kind)
                throw std::invalid_argument("journal_state_kind_required");
        return stage_records(drafts, views, std::move(state_override));
    }

    // Publishes `staged` only if the head is still its parent. Order: segment
    // bytes and view pages (appends flushed, new files published), manifest
    // log bytes, entries made durable, then HEAD published by an atomic move,
    // entries made durable, and a noexcept snapshot swap. Nothing is
    // allocated here from the data; any I/O failure poisons the store, which
    // must then be reopened, and reopening removes unpublished leftovers.
private:
    [[nodiscard]] static JournalStoreOwner make_owned(
        std::filesystem::path directory, JournalIdentity identity,
        std::shared_ptr<const StorageBudget> storage, const AllocationContext& memory,
        std::uint64_t allocation_unit, std::shared_ptr<io::OwnerLock> lock,
        const std::optional<AllocationContext>& page_cache, std::size_t page_cache_shards);
    void publish(StagedGeneration&& staged);

public:

    // The published generation readers see now.
    [[nodiscard]] PublishedUniverse universe() const;

    // False when open found the derived view damaged; `rebuild_view` makes
    // it available again.
    [[nodiscard]] bool view_available() const;

    // Exact address lookup (board §9 :592) through the published view: the
    // position comes from pages bound to HEAD by digests, never from the
    // caller. Reads one page per level. Empty when the address is not in the
    // published journal.
    [[nodiscard]] std::optional<RecordPosition> resolve(const ExperienceAddress& address) const;

    // Replay of one exact original: resolves the address and reads the record
    // at the resolved position within one snapshot (one page-log lease), and
    // requires its digest and address (`journal_address_unknown` when it is
    // not published).
    [[nodiscard]] PublishedRecord replay(const ExperienceAddress& address) const;

    // Replay of one exact original together with the state generation HEAD
    // names, both from one snapshot (codex 14:46).
    [[nodiscard]] ReplayAtHead replay_at_head(const ExperienceAddress& address) const;

    // Visits every published record in sequence order over one snapshot,
    // verifying the whole record chain. No lock is held while `visit` runs.
    // Lineage: native mechanism — type-erased entry to for_each_record_impl, which verifies the chain.
    // SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@cefdc3fce8b5c605166d668924baa5d4a6c49dc0:50
    template <class F>
        requires(std::is_object_v<F> &&
                 std::is_invocable_v<F&, const RecordView&, const RecordPosition&>)
    void for_each_record(F& visit) const {
        const auto invoke = [](const void* target, const RecordView& record,
                               const RecordPosition& position) {
            (*static_cast<F*>(const_cast<void*>(target)))(record, position);
        };
        for_each_record_impl(std::addressof(visit), invoke);
    }
    template <class F>
    void for_each_record(const F&&) const = delete;

    // Index navigation (board §3B :122-123, §9 :592; L3 hot cue lookup):
    // visits every published record that carries the index entry (`kind`,
    // `value`), in address order, with its address and exact position, over
    // one snapshot, until `visit` returns false. Reads one page per level
    // down to the first match, then the leaves in order, verifying every page
    // digest; the address views a page that lives only for the call. Fails
    // with `journal_index_invalid` (not an index entry) or
    // `journal_view_unavailable`.
    void for_each_index_match(char kind, std::string_view value, IndexVisitor visit) const;

    // Copy on write leaves replaced pages in the page logs. Compaction is due
    // when their bytes exceed the live pages' bytes (both trees) plus one
    // log, so after Main compacts whenever it is due, the views' disk use
    // stays within twice their live pages plus one log.
    [[nodiscard]] bool compaction_due() const;

    // Rewrites the live view pages of both trees, streamed leaf by leaf in
    // key order, into new page logs (memory: one open page per level), then
    // publishes a generation with no records whose views are the rewritten
    // trees over the same entries (`journal_view_unavailable` when the view is). The old
    // logs stay, charged, until no snapshot holds their
    // lease; the next publication removes them, and a reopen removes any
    // left. A failure before HEAD removes the new logs and publishes nothing.
private:
    void compact_view();

    // Rebuilds both views from the records alone, never reading the old
    // ones: every record chain is verified while addresses and index keys are
    // collected in batches of bounded memory; each sorted batch is written as
    // a run of leaf pages in new logs, each tree's runs are merged in one
    // k-way pass into its final tree (reading one page per level per run),
    // both final trees going to one set of new logs (a tree that fits one
    // batch is built straight into them, without a run), the runs are removed,
    // and the result, whose address tree must hold one entry per record, is
    // published like a compaction. After the tree is built, `validate` sees
    // every verified record with a reader backed by that unpublished tree;
    // failure leaves the previous HEAD authoritative. I/O is linear in the
    // views' size plus this cold validation pass and its exact reads.
    //
    // Both rewrites hold the publication lock for their whole duration, so a
    // concurrent `publish` waits for them; readers are never blocked by them
    // (a reader takes only a page-cache shard lock, per page, briefly).
    void rebuild_view(RebuildValidator validate);

    // Removes retired page logs whose lease no snapshot holds. Publication
    // and both rewrites do this first; Main calls it directly when a stage
    // failed for storage while `storage_charged` still includes them.
    void reclaim_retired();

    friend class swegca::vrs::MainOwner;
    // `stage` without the kind rule: reachable outside JournalStore only
    // through the experience and Main-only state staging keys.
    [[nodiscard]] StagedGeneration stage_records(std::span<const RecordDraft> drafts,
                                                 std::span<const ViewGeneration> views,
                                                 std::optional<StateHeadReference> state_override) const;
    JournalStore(std::filesystem::path directory, JournalIdentity identity,
                 std::shared_ptr<const StorageBudget> storage, const AllocationContext& memory,
                 std::uint64_t allocation_unit, std::shared_ptr<io::OwnerLock> lock,
                 const std::optional<AllocationContext>& page_cache, std::size_t page_cache_shards);

    // Page logs a view rewrite left behind, and the lease that keeps them.
    struct RetiredLogs {
        std::uint64_t first = 0;
        std::uint64_t last = 0;
        std::uint64_t charge = 0;
        std::weak_ptr<const void> lease;
    };

    void load_published_head();
    [[nodiscard]] std::shared_ptr<PublishedSnapshot> load_generation(const HeadPointer& pointer) const;
    void load_root_generation(const JournalRoot& root);
    void require_writable() const;
    void for_each_record_impl(
        const void* target,
        void (*visit)(const void*, const RecordView&, const RecordPosition&)) const;
    // Main must retain the shared_ptr returned by snapshot() for the entire
    // multi-cue activation. The reference alone does not keep this generation
    // or its retired page logs alive. Resolve and replay must use that same
    // pinned snapshot; a separate snapshot() call may see a newer HEAD.
    void for_each_index_match_in(const PublishedSnapshot& current, char kind,
                                 std::string_view value, IndexVisitor visit) const;
    void require_usable() const;
    [[nodiscard]] std::shared_ptr<const PublishedSnapshot> snapshot() const;
    [[nodiscard]] std::uint64_t storage_of(const ExtentIndex& extents, const ManifestFields& head,
                                           const ManifestLocation& location) const;
    [[nodiscard]] std::uint64_t page_log_charge(const ViewPages& view) const;
    [[nodiscard]] std::uint64_t used_bytes(const PublishedSnapshot& current) const;
    void verify_extent(const PublishedSnapshot& snapshot, const SegmentExtent& extent) const;
    [[nodiscard]] StagedGeneration stage_from(const std::shared_ptr<const PublishedSnapshot>& current,
                                              std::span<const RecordDraft> drafts,
                                              const StateHeadReference& state,
                                              std::span<const ViewGeneration> views,
                                              const ViewPages* replacement,
                                              std::uint64_t retained) const;
    [[nodiscard]] StagedGeneration stage_view(const std::shared_ptr<const PublishedSnapshot>& current,
                                              const ViewPages& view) const;
    void publish_locked(StagedGeneration&& staged);
    void reserve_retired();
    void publish_replacing_view(StagedGeneration&& staged,
                                const std::shared_ptr<const PublishedSnapshot>& old);
    void reclaim_locked() noexcept;
    [[nodiscard]] bool remove_page_logs(const RetiredLogs& logs) const noexcept;
    [[nodiscard]] std::optional<RecordPosition> resolve_in(const PublishedSnapshot& current,
                                                           std::string_view key) const;
    [[nodiscard]] PublishedRecord read_in(const PublishedSnapshot& current,
                                          const RecordPosition& position) const;
    [[nodiscard]] PublishedRecord replay_in(const PublishedSnapshot& current,
                                            const ExperienceAddress& address) const;

    std::filesystem::path directory_;
    JournalIdentity identity_;
    std::shared_ptr<const StorageBudget> storage_;  // the host's, shared
    AllocationContext memory_;
    std::uint64_t allocation_unit_;
    std::shared_ptr<io::OwnerLock> lock_;  // control block charged to Main's allocator
    std::shared_ptr<PageCache> cache_;  // charged to its host context; internally locked
    std::mutex publish_mutex_;
    std::atomic<std::shared_ptr<const PublishedSnapshot>> snapshot_;
    // Cold scan includes unpublished segment names, so a physical file id
    // cannot collide with bytes retained during this open. The current
    // own-HEAD cleanup may delete orphan files; a later restart may then
    // reuse their numbers. Root-selected recovery needs a durable high-water
    // mark or preserved orphan names before claiming global non-reuse.
    std::atomic<std::uint64_t> max_physical_segment_id_{0};
    std::atomic<bool> poisoned_{false};
    bool root_selected_ = false;  // opened by open_at_root: read-only; set before the store is returned
    LedgerVector<RetiredLogs> retired_;  // guarded by publish_mutex_
    std::atomic<std::uint64_t> retained_bytes_{0};  // charge of retired_, not yet removed
};

}  // namespace swegca::vrs::journal

// Complete the stage key's friend in every translation unit that sees it. A
// forward declaration alone permits a caller to define a substitute friend.
#include "swegca_vrs/experience.hpp"
