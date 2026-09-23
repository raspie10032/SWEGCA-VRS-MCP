#pragma once

#include "swegca_architecture/journal_file_io.hpp"
#include "swegca_architecture/journal_format.hpp"
#include "swegca_architecture/journal_position.hpp"
#include "swegca_architecture/memory_ledger.hpp"
#include "swegca_architecture/strong_types.hpp"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>

// Main-owned native journal directory (v5): exclusive owner lock, detached
// staging, one serialized publisher, snapshot readers (each read holds one
// immutable published snapshot for its whole duration and takes no journal
// lock; the atomic snapshot pointer itself is not promised lock-free),
// fail-closed recovery from the published HEAD only, and the derived
// exact-address view published by the same HEAD, compacted and rebuildable
// from the records.
// Rule: SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md §3B, §9; one
// current generation (I01), provenance-bound and recoverable persistent
// mutation (I07), ARCHITECTURE_SPEC.md@5901a5a:205-207,214-216.
// re-created (user@2026-09-23): replaces SQLite, which the user removed.
// Resources (codex J8, J9, J13): every byte kept on disk is charged to the
// journal's storage reservation before it is written. Every buffer and
// container the journal allocates uses Main's ledger allocator, so it is
// charged with its exact requested size before it is allocated. Not reached
// by the ledger: path strings, exception objects, OS handles, the store
// object itself, and the caller's own inputs (drafts, views); Main
// integration closes those.
namespace swegca::architecture::journal {

// Published extents by ordinal; every node goes through the ledger allocator.
using ExtentTable =
    std::map<std::uint64_t, SegmentExtent, std::less<>,
             MemoryLedger::Allocator<std::pair<const std::uint64_t, SegmentExtent>>>;

// One published generation as readers see it. Immutable once published; the
// object and its control block are allocated through the ledger allocator.
struct PublishedSnapshot {
    // SWEGCA: user@2026-09-22:72-79
    PublishedSnapshot(const MemoryLedger::Account& memory, Manifest manifest)
        : head(std::move(manifest)),
          extents(memory.allocator<std::pair<const std::uint64_t, SegmentExtent>>()) {}

    Manifest head;
    ManifestLocation location;
    ExtentTable extents;
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

// One published record read back: its exact bytes, allocated through the
// ledger, and the view decoded from them. The view, and every text and span
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

    // SWEGCA: user@2026-09-22:72-79
    [[nodiscard]] const RecordView& view() const& noexcept { return view_; }
    const RecordView& view() const&& = delete;
    // SWEGCA: user@2026-09-22:72-79
    [[nodiscard]] const RecordPosition& position() const noexcept { return position_; }

private:
    friend class JournalStore;
    // Decodes `bytes` and requires the record `position` names.
    PublishedRecord(LedgerBytes bytes, const RecordPosition& position);

    LedgerBytes bytes_;
    RecordView view_;
    RecordPosition position_;
};

// Bytes one generation places in one segment file: appended at the published
// length of the tail segment, or a new file starting with its header.
struct SegmentPiece {
    std::uint64_t ordinal = 0;
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

    // SWEGCA: user@2026-09-22:72-79
    [[nodiscard]] bool valid() const noexcept { return valid_; }
    // The manifest it would publish; an invalid staged generation has none
    // (`journal_staged_generation_invalid`).
    // SWEGCA: user@2026-09-22:72-79
    [[nodiscard]] const Manifest& manifest() const {
        if (!valid_) throw std::logic_error("journal_staged_generation_invalid");
        return next_->head;
    }
    // SWEGCA: user@2026-09-22:72-79
    [[nodiscard]] std::span<const RecordPosition> positions() const noexcept {
        return positions_;
    }

private:
    friend class JournalStore;
    explicit StagedGeneration(const MemoryLedger::Account& memory);

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

class JournalStore final {
public:
    // Opens `directory`, creating it atomically when it does not exist
    // (generation 0 is built in a sibling directory and renamed into place
    // without replacement). An existing directory must be exactly a journal;
    // these fail closed: unknown entries (including a `.part` file that is
    // not an interrupted publication of a journal name), a missing or
    // corrupt HEAD or manifest on the chain back to the checkpoint, a segment
    // no extent names, a missing or corrupt extent the head generation
    // wrote, a recovery chain over its read budget, and published use over
    // `storage_bytes`. The view is derived: a missing page log in its range,
    // or a last log that cannot be cut back to its published end, opens the
    // journal with the view unavailable (lookups, stages and compaction fail
    // with `journal_view_unavailable`) so that Main can `rebuild_view` from
    // the originals. Unpublished
    // leftovers of an interrupted publication and page logs the view no
    // longer reaches are removed. Opening reads HEAD and the manifest chain
    // back to its checkpoint (at most `max_recovery_bytes`) and verifies only
    // the extents the head generation wrote (at most two segments). Older
    // segments (their presence and bytes) and view pages are verified when
    // read: `replay` checks every page digest on the way and the record
    // digest the view names; `for_each_record` and `rebuild_view` check the
    // whole record chain.
    // `identity` must satisfy the identity rule; the store keeps its own copy
    // on `memory`.
    [[nodiscard]] static std::unique_ptr<JournalStore> open(
        const std::filesystem::path& directory, std::string_view identity,
        std::uint64_t storage_bytes, const MemoryLedger::Account& memory);

    JournalStore(JournalStore&&) = delete;
    JournalStore(const JournalStore&) = delete;
    JournalStore& operator=(const JournalStore&) = delete;
    JournalStore& operator=(JournalStore&&) = delete;
    ~JournalStore() = default;

    // The published head manifest, shared with its snapshot: no copy is made
    // and the snapshot lives as long as the returned pointer.
    [[nodiscard]] std::shared_ptr<const Manifest> head() const;
    [[nodiscard]] StateGeneration state_generation() const;
    // Charged on-disk use of the published journal, in bytes, including page
    // logs a view rewrite left behind that are not yet removed.
    [[nodiscard]] std::uint64_t storage_charged() const;

    // Builds a detached successor of the current head: the records, the
    // exact-address view pages for them (addresses must be new and unique),
    // the complete next snapshot and the HEAD bytes. Disk use is checked
    // against the storage reservation before any record is encoded and again,
    // with the view pages, before anything is written. Fails with
    // `journal_generation_too_large`, `journal_address_duplicate`,
    // `journal_storage_budget_exhausted` or `memory_budget_exhausted`.
    [[nodiscard]] StagedGeneration stage(std::span<const RecordDraft> drafts,
                                         const StateGeneration& state,
                                         std::span<const ViewGeneration> views) const;

    // Publishes `staged` only if the head is still its parent. Order: segment
    // bytes and view pages (appends flushed, new files published), manifest
    // log bytes, entries made durable, then HEAD published by an atomic move,
    // entries made durable, and a noexcept snapshot swap. Nothing is
    // allocated here from the data; any I/O failure poisons the store, which
    // must then be reopened, and reopening removes unpublished leftovers.
    void publish(StagedGeneration&& staged);

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

    // Visits every published record in sequence order over one snapshot,
    // verifying the whole record chain. No lock is held while `visit` runs.
    void for_each_record(const std::function<void(const RecordView&, const RecordPosition&)>& visit) const;

    // Copy on write leaves replaced pages in the page logs. Compaction is due
    // when their bytes exceed the live pages' bytes plus one log, so after
    // Main compacts whenever it is due, the view's disk use stays within
    // twice its live pages plus one log.
    [[nodiscard]] bool compaction_due() const;

    // Rewrites the live view pages, streamed leaf by leaf in address order,
    // into new page logs (memory: one open page per level), then publishes a
    // generation with no records whose view is the rewritten tree over the
    // same entries (`journal_view_unavailable` when the view is). The old
    // logs stay, charged, until no snapshot holds their
    // lease; the next publication removes them, and a reopen removes any
    // left. A failure before HEAD removes the new logs and publishes nothing.
    void compact_view();

    // Rebuilds the view from the records alone, never reading the old view:
    // every record chain is verified while addresses are collected in batches
    // of bounded memory; each sorted batch is written as a run of leaf pages
    // in new logs, the runs are merged in one k-way pass into the final tree
    // (reading one page per level per run), the runs are removed, and the
    // result, which must hold one entry per record, is published like a
    // compaction. I/O is linear in the view's size.
    //
    // Both rewrites hold the publication lock for their whole duration, so a
    // concurrent `publish` waits for them; readers are never blocked.
    void rebuild_view();

    // Removes retired page logs whose lease no snapshot holds. Publication
    // and both rewrites do this first; Main calls it directly when a stage
    // failed for storage while `storage_charged` still includes them.
    void reclaim_retired();

private:
    JournalStore(std::filesystem::path directory, JournalIdentity identity,
                 std::uint64_t storage_bytes, const MemoryLedger::Account& memory,
                 std::uint64_t allocation_unit, std::unique_ptr<io::OwnerLock> lock);

    // Page logs a view rewrite left behind, and the lease that keeps them.
    struct RetiredLogs {
        std::uint64_t first = 0;
        std::uint64_t last = 0;
        std::uint64_t charge = 0;
        std::weak_ptr<const void> lease;
    };

    void load_published_head();
    void require_usable() const;
    [[nodiscard]] std::shared_ptr<const PublishedSnapshot> snapshot() const;
    [[nodiscard]] std::uint64_t storage_of(const ExtentTable& extents, const ManifestFields& head,
                                           const ManifestLocation& location) const;
    [[nodiscard]] std::uint64_t page_log_charge(const AddressView& view) const;
    [[nodiscard]] std::uint64_t allowance(const PublishedSnapshot& current) const;
    void verify_extent(const PublishedSnapshot& snapshot, const SegmentExtent& extent) const;
    [[nodiscard]] StagedGeneration stage_from(const std::shared_ptr<const PublishedSnapshot>& current,
                                              std::span<const RecordDraft> drafts,
                                              const StateGeneration& state,
                                              std::span<const ViewGeneration> views,
                                              const AddressView* replacement,
                                              std::uint64_t retained) const;
    [[nodiscard]] StagedGeneration stage_view(const std::shared_ptr<const PublishedSnapshot>& current,
                                              const AddressView& view) const;
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

    std::filesystem::path directory_;
    JournalIdentity identity_;
    std::uint64_t storage_bytes_;
    MemoryLedger::Account memory_;
    std::uint64_t allocation_unit_;
    std::unique_ptr<io::OwnerLock> lock_;
    std::mutex publish_mutex_;
    std::atomic<std::shared_ptr<const PublishedSnapshot>> snapshot_;
    std::atomic<bool> poisoned_{false};
    LedgerVector<RetiredLogs> retired_;  // guarded by publish_mutex_
    std::atomic<std::uint64_t> retained_bytes_{0};  // charge of retired_, not yet removed
};

}  // namespace swegca::architecture::journal
