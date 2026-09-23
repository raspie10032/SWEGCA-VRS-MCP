#pragma once

#include "swegca_architecture/authority.hpp"
#include "swegca_architecture/digest_bytes.hpp"
#include "swegca_architecture/journal_position.hpp"
#include "swegca_architecture/memory_ledger.hpp"
#include "swegca_architecture/resource_limits.hpp"
#include "swegca_architecture/sha256.hpp"
#include "swegca_architecture/strong_types.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

// Byte format of the Main-owned native journal (v7): append-only record
// segments, an append-only manifest log, checkpoint manifests, append-only
// page logs of the derived exact-address and index views, and a fixed-size HEAD
// pointer, all as flat files in one directory. Storage format only; no record grants
// authority.
// Rules: SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md §3B, §9;
// ARCHITECTURE_SPEC.md@5901a5a:205-207,214-216 (I03, I07).
// re-created (user@2026-09-23): replaces SQLite, which the user removed.
// Every size here is bounded by the product ceilings in ResourceLimits: the
// extent table by the storage ceiling (codex J8), and recovery by a fixed
// read budget rather than by a count of generations (codex J10).
// Memory (codex J13): every buffer this format fills is a LedgerBytes or
// LedgerVector, so each allocation is charged to Main's ledger with its
// exact requested size before it is made. Decoded records, manifests and
// pages are views into those exact bytes; decoding makes no string copies.
namespace swegca::architecture::journal {

using Digest = DigestBytes;  // one digest byte type across the rebuild (codex J12)
inline constexpr Digest zero_digest{};

using LedgerBytes = std::vector<std::byte, MemoryLedger::Allocator<std::byte>>;
template <class T>
using LedgerVector = std::vector<T, MemoryLedger::Allocator<T>>;

inline constexpr std::size_t max_payload_bytes = 16u * 1024u * 1024u;
inline constexpr std::size_t max_segment_bytes = 64u * 1024u * 1024u;
inline constexpr std::size_t max_generation_bytes = 64u * 1024u * 1024u;
inline constexpr std::size_t max_manifest_bytes = 16u * 1024u * 1024u;
inline constexpr std::size_t max_manifest_log_bytes = 64u * 1024u * 1024u;
// Enough full segments to hold the whole storage ceiling, and no more.
inline constexpr std::size_t max_extents = static_cast<std::size_t>(
    (ResourceLimits::max_storage_bytes + max_segment_bytes - 1) / max_segment_bytes);
inline constexpr std::size_t max_manifest_views = 4096;
// A checkpoint is written at least every `checkpoint_interval` generations
// and whenever recovery would otherwise read more than `max_recovery_bytes`
// of manifests (the head back to its checkpoint, both included).
inline constexpr std::uint64_t checkpoint_interval = 4096;
inline constexpr std::uint64_t max_recovery_bytes = 64u * 1024u * 1024u;
static_assert(max_recovery_bytes >= max_manifest_bytes,
              "a checkpoint alone must always fit the recovery budget");
inline constexpr std::size_t segment_header_bytes = 4 + 2 + 8 + 8;
inline constexpr std::size_t manifest_log_header_bytes = 4 + 2 + 8;
inline constexpr std::size_t record_prefix_bytes = 4 + 2 + 4;
// magic, version, length, kind, authority, reserved, sequence, 8 text lengths,
// index entry count, payload length, payload/previous/record digests.
inline constexpr std::size_t minimum_record_bytes =
    4 + 2 + 4 + 2 + 1 + 1 + 8 + 8 * 4 + 4 + 4 + 3 * 32;
// Index entries one record carries (J18). An entry is a kind letter (ASCII
// a-z or A-Z) followed by a nonempty value, an identity text without the
// index separator; its index-view key is (entry, separator, address), which
// must itself be an identity text, so the separator splits every key exactly
// and the keys of one kind and value are contiguous. Lowercase kinds belong
// to experience records (the two kinds below, experience.hpp); a record of
// any other kind may carry only uppercase kinds. The journal enforces the
// kind rule; that only the experience module writes kinds 1 and 2 is Main's
// rule (it stages them only through ExperienceJournal), and decoding an
// experience rejects any record it did not write.
inline constexpr std::size_t max_record_index_entries = 16384;
inline constexpr char index_separator = '\x1f';
inline constexpr std::uint16_t original_experience_record_kind = 1;
inline constexpr std::uint16_t derived_experience_record_kind = 2;
// ordinal, first sequence, record count, byte length, last record digest.
inline constexpr std::size_t encoded_extent_bytes = 4 * 8 + 32;

// Derived views (board §3B :122, §9 :569-592): the exact-address tree (one
// entry per record, keyed by address) and the index tree (one entry per
// index entry of every record, keyed by entry, separator, address; L3 hot
// cue and index lookup). Main
// builds both in the same detached generation as the records they index, as
// immutable pages in shared append-only page logs; the manifest names each
// root page by location and digest, so one HEAD publishes records and views
// together and every page is bound to that HEAD through its parent's digest.
// They are rebuildable from the records and never the source of truth; the
// record chain is.
inline constexpr std::size_t address_page_max_bytes = 16u * 1024u;
inline constexpr std::size_t address_page_header_bytes = 4 + 2 + 1 + 1 + 4;
inline constexpr std::size_t max_page_log_bytes = 64u * 1024u * 1024u;
inline constexpr std::size_t page_log_header_bytes = 4 + 2 + 8;
inline constexpr std::uint32_t max_address_height = 32;
inline constexpr std::size_t encoded_page_ref_bytes = 8 + 8 + 4 + 32;
inline constexpr std::size_t encoded_view_tree_bytes = 8 + 4 + encoded_page_ref_bytes + 8;
inline constexpr std::size_t encoded_view_pages_bytes = 2 * encoded_view_tree_bytes + 4 * 8;

struct JournalIdentityTag { static constexpr std::string_view name = "journal_identity"; };
struct SourceRevisionTag { static constexpr std::string_view name = "source_revision"; };
struct OperationIdTag { static constexpr std::string_view name = "operation_id"; };
struct OutcomeTextTag { static constexpr std::string_view name = "outcome"; };
using JournalIdentity = TextIdentity<JournalIdentityTag>;

// A record before Main assigns its sequence and chain digests. On disk an
// absent optional is an empty text and an absent authority is byte 0; an
// appended record documents authority, it never carries it. The draft is the
// caller's borrowed input: every text must satisfy the identity rule (a
// present optional too) and is checked before anything is encoded; the
// journal copies texts and payload into charged bytes.
struct RecordDraft {
    std::uint16_t kind = 0;  // owner-defined, nonzero
    std::optional<AuthorityDomain> authority;
    std::string_view address;          // experience address
    std::string_view source;           // producer id
    std::string_view source_revision;
    std::optional<std::string_view> previous_revision_address;
    std::optional<std::string_view> claim;
    std::optional<std::string_view> outcome;
    std::string_view operation_id;
    std::optional<std::string_view> transaction_id;
    // Strictly increasing index entries (see `is_index_entry`), each allowed
    // for `kind` (`index_entry_allowed`); each with the address forms one
    // index-view key.
    std::span<const std::string_view> index;
    std::span<const std::byte> payload;  // canonical owner bytes
};

// A decoded record: every text and the payload are views into the bytes it
// was decoded from, valid only while those bytes are. An absent optional
// text is empty. Copying it copies views, never data.
struct RecordView {
    std::uint64_t sequence = 0;
    std::uint16_t kind = 0;
    std::optional<AuthorityDomain> authority;
    std::string_view address;
    std::string_view source;
    std::string_view source_revision;
    std::string_view previous_revision_address;
    std::string_view claim;
    std::string_view outcome;
    std::string_view operation_id;
    std::string_view transaction_id;
    std::uint32_t index_count = 0;
    std::span<const std::byte> index;  // `index_count` length-prefixed entries, increasing
    std::span<const std::byte> payload;
    Digest payload_digest{};
    Digest previous_record_digest{};
    Digest record_digest{};
};

// Little-endian encoder appending to a ledger buffer that should already
// have capacity for what is written (so encoding never reallocates).
class ByteWriter final {
public:
    // SWEGCA: user@2026-09-22:60-61
    explicit ByteWriter(LedgerBytes& target) noexcept : out_(&target) {}
    ByteWriter(const ByteWriter&) = delete;
    ByteWriter& operator=(const ByteWriter&) = delete;

    void u8(std::uint8_t value);
    void u16(std::uint16_t value);
    void u32(std::uint32_t value);
    void u64(std::uint64_t value);
    void text(std::string_view value, std::size_t limit);
    void bytes(std::span<const std::byte> value, std::size_t limit);
    void digest(const Digest& value);
    void raw(std::span<const std::byte> value);
    // SWEGCA: user@2026-09-22:60-61
    [[nodiscard]] std::size_t size() const noexcept { return out_->size(); }

private:
    LedgerBytes* out_;
};

// Bounds-checked decoder over bytes it does not own. Every overrun throws
// `journal_truncated`; texts and byte strings are returned as views.
class ByteReader final {
public:
    // SWEGCA: user@2026-09-22:60-61
    explicit ByteReader(std::span<const std::byte> data) noexcept : data_(data) {}

    [[nodiscard]] std::uint8_t u8();
    [[nodiscard]] std::uint16_t u16();
    [[nodiscard]] std::uint32_t u32();
    [[nodiscard]] std::uint64_t u64();
    // A length-prefixed strict UTF-8 text.
    [[nodiscard]] std::string_view text_view(std::size_t limit);
    // A length-prefixed byte string.
    [[nodiscard]] std::span<const std::byte> bytes_view(std::size_t limit);
    [[nodiscard]] Digest digest();
    [[nodiscard]] std::span<const std::byte> raw(std::size_t count);

    // SWEGCA: user@2026-09-22:60-61
    [[nodiscard]] std::size_t offset() const noexcept { return offset_; }
    // SWEGCA: user@2026-09-22:60-61
    [[nodiscard]] std::size_t remaining() const noexcept { return data_.size() - offset_; }
    [[nodiscard]] std::span<const std::byte> consumed_since(std::size_t start) const;

private:
    std::span<const std::byte> data_;
    std::size_t offset_ = 0;
};

// Visits a decoded record's index entries in their increasing order; each
// text views the record's bytes.
// SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:398-437
template <class Visit>
void for_each_index_entry(const RecordView& record, Visit&& visit) {
    ByteReader reader(record.index);
    for (std::uint32_t at = 0; at < record.index_count; ++at)
        visit(reader.text_view(detail::identity_text_max_bytes));
}

// A kind letter and a nonempty value: an identity text without the index
// separator.
[[nodiscard]] bool is_index_entry(std::string_view index_entry) noexcept;

// Whether a record of `record_kind` may carry `index_entry`: lowercase kinds
// only on experience records.
[[nodiscard]] bool index_entry_allowed(std::uint16_t record_kind, std::string_view index_entry) noexcept;

// Size of the index-view key (entry, separator, address).
// SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:398-437
[[nodiscard]] constexpr std::size_t index_key_size(std::string_view index_entry,
                                                 std::string_view address) noexcept {
    return index_entry.size() + 1 + address.size();
}

// Exact encoded size of `draft` as a record. Validates every limit, so a
// draft that sizes can be encoded. Allocates nothing.
[[nodiscard]] std::size_t encoded_record_size(const RecordDraft& draft);

// Appends one record at `sequence`, chained to `previous_record_digest`, to
// `out`, which should already have capacity for it.
void append_record(LedgerBytes& out, const RecordDraft& draft, std::uint64_t sequence,
                   const Digest& previous_record_digest, Digest& record_digest);

// Decodes and verifies one record: magic, version, declared length, identity
// texts, payload digest and record digest. The result views the reader's bytes.
[[nodiscard]] RecordView decode_record(ByteReader& reader);

// The published part of one append-only segment file. `byte_length` is the
// published length; bytes past it are unpublished and never read.
struct SegmentExtent {
    std::uint64_t ordinal = 0;         // contiguous from 1
    std::uint64_t first_sequence = 0;  // contiguous across segments
    std::uint64_t record_count = 0;    // nonzero
    std::uint64_t byte_length = 0;     // header + records
    Digest last_record_digest{};
};

void append_segment_header(LedgerBytes& out, std::uint64_t ordinal, std::uint64_t first_sequence);

// Verifies records [first_sequence, first_sequence + record_count) held in
// `bytes`, which start at file offset `base_offset` (0 means the bytes begin
// with the segment header, which is checked against `extent`). The chain must
// enter at `entering` and end at `expected_last` exactly at the end of
// `bytes`. `visit`, when set, receives each record and its file offset; the
// record views `bytes`.
using RecordVisitor = std::function<void(const RecordView&, std::uint64_t byte_offset)>;
void decode_segment_range(std::span<const std::byte> bytes, std::uint64_t base_offset,
                          const SegmentExtent& extent, std::uint64_t first_sequence,
                          std::uint64_t record_count, const Digest& entering,
                          const Digest& expected_last, const RecordVisitor* visit);

// Where one published page is and the digest of its bytes.
struct PageRef {
    std::uint64_t log_ordinal = 0;
    std::uint64_t offset = 0;
    std::uint32_t length = 0;
    Digest digest{};

    auto operator<=>(const PageRef&) const = default;
};

// One tree of the derived views.
struct ViewTree {
    std::uint64_t entry_count = 0;
    std::uint32_t height = 0;           // 0 when empty
    PageRef root;                       // zero when empty
    std::uint64_t live_page_bytes = 0;  // bytes of the pages the tree reaches

    auto operator<=>(const ViewTree&) const = default;
};

// The derived views of one generation and the page logs they share. The
// address tree holds one entry per record, so its `entry_count` equals the
// manifest's `tail_sequence`; the index tree holds one per index entry.
struct ViewPages {
    ViewTree addresses;
    ViewTree index;
    std::uint64_t first_page_log = 0;    // oldest page log a tree reaches; 0 without logs
    std::uint64_t page_log_ordinal = 0;  // current page log; 0 without logs
    std::uint64_t page_log_end = 0;      // published length of the current page log
    std::uint64_t page_log_bytes = 0;    // bytes of logs first..current, headers included

    // SWEGCA: user@2026-09-22:72-79
    [[nodiscard]] std::uint64_t live_page_bytes() const noexcept {
        return addresses.live_page_bytes + index.live_page_bytes;
    }
    auto operator<=>(const ViewPages&) const = default;
};

// One leaf entry: a view key (an address in the address tree; entry,
// separator, address in the index tree) and the exact position of its record.
struct AddressLeafItem {
    std::string_view address;
    RecordPosition position;
};

// One branch entry: the smallest key under a child page, and the child.
struct AddressChildItem {
    std::string_view first_address;
    PageRef page;
};

// A decoded page. Its texts view the bytes it was decoded from.
struct AddressPageView {
    // SWEGCA: user@2026-09-22:72-79
    explicit AddressPageView(const MemoryLedger::Account& memory)
        : leaves(memory.allocator<AddressLeafItem>()),
          children(memory.allocator<AddressChildItem>()) {}

    bool leaf = true;
    LedgerVector<AddressLeafItem> leaves;
    LedgerVector<AddressChildItem> children;
};

// SWEGCA: user@2026-09-22:72-79
[[nodiscard]] constexpr std::size_t encoded_leaf_item_size(std::string_view address) noexcept {
    return 4 + address.size() + 3 * 8 + 32;
}
// SWEGCA: user@2026-09-22:72-79
[[nodiscard]] constexpr std::size_t encoded_child_item_size(std::string_view first) noexcept {
    return 4 + first.size() + encoded_page_ref_bytes;
}

// Appends one page to `out` (which should already have capacity for it).
// Items must be nonempty and in strictly increasing address order, and the
// page at most `address_page_max_bytes`. The first item's address starts at
// `address_page_header_bytes + 4` in the page.
void append_leaf_page(LedgerBytes& out, std::span<const AddressLeafItem> items);
void append_branch_page(LedgerBytes& out, std::span<const AddressChildItem> items);

// Decodes and verifies magic, version, kind, order, positions and limits.
[[nodiscard]] AddressPageView decode_address_page(std::span<const std::byte> bytes,
                                                  const MemoryLedger::Account& memory);

void append_page_log_header(LedgerBytes& out, std::uint64_t log_ordinal);
void check_page_log_header(std::span<const std::byte> bytes, std::uint64_t log_ordinal);

struct ManifestLocation {
    std::uint64_t log_ordinal = 0;
    std::uint64_t offset = 0;
    std::uint64_t length = 0;
};

// True when `next` is where the manifest after the one at `previous` goes:
// right behind it in the same log, or first in the following log.
[[nodiscard]] bool follows(const ManifestLocation& previous, const ManifestLocation& next) noexcept;

// A published view generation the manifest names (Main's other derived
// views). Its name views the caller's input or the manifest's bytes.
struct ViewGeneration {
    std::string_view name;
    std::uint64_t generation = 0;
    Digest digest{};
};

// The fixed-size part of one published generation. A checkpoint lists every
// extent; any other generation lists only the extents it touched (the
// extended tail and new segments). `state_generation_*` is Main's Cognitive
// State generation. The accounting fields make recovery cost and storage use
// verifiable from the chain itself:
//   manifest_bytes_before  lengths of every earlier manifest, summed;
//   recovery_bytes_before  lengths of the manifests from the latest
//                          checkpoint up to the parent, summed (0 for a
//                          checkpoint), so recovery from this manifest reads
//                          its own length plus this.
struct ManifestFields {
    std::uint64_t generation = 0;
    Digest previous_manifest_digest{};   // zero for generation 0
    ManifestLocation previous_location;  // zero for generation 0
    bool checkpoint = false;             // generation 0 is a checkpoint
    std::uint64_t checkpoint_generation = 0;
    std::uint64_t recovery_bytes_before = 0;
    std::uint64_t manifest_bytes_before = 0;
    std::uint64_t state_generation_ordinal = 0;
    Digest state_generation_digest{};
    std::uint64_t tail_sequence = 0;  // 0 when empty
    Digest tail_record_digest{};      // zero when empty
    std::uint64_t tail_segment_ordinal = 0;
    ViewPages view_pages;
};

// Exact encoded size of a manifest; validates the count limits and that
// view names are nonempty and strictly increasing. Allocates nothing.
[[nodiscard]] std::size_t encoded_manifest_size(std::string_view journal_identity,
                                                std::size_t extent_count,
                                                std::span<const ViewGeneration> views);

// One published manifest, held as its exact encoded bytes; every accessor
// reads from them, so nothing but the bytes and the view offset table is
// allocated, both through the ledger.
class Manifest final {
public:
    // Verifies magic, version, limits, extent contiguity, tail, view pages,
    // checkpoint and recovery rules, and the digest.
    [[nodiscard]] static Manifest decode(LedgerBytes bytes, const MemoryLedger::Account& memory);

    // Encodes and decodes (so what could not be loaded is never kept).
    [[nodiscard]] static Manifest encode(const ManifestFields& fields,
                                         std::string_view journal_identity,
                                         std::span<const SegmentExtent> extents,
                                         std::span<const ViewGeneration> views,
                                         const MemoryLedger::Account& memory);

    Manifest(Manifest&&) noexcept = default;
    Manifest& operator=(Manifest&&) noexcept = default;
    Manifest(const Manifest&) = delete;
    Manifest& operator=(const Manifest&) = delete;
    ~Manifest() = default;

    // SWEGCA: user@2026-09-22:72-79
    [[nodiscard]] const ManifestFields& fields() const noexcept { return fields_; }
    // SWEGCA: user@2026-09-22:72-79
    [[nodiscard]] const Digest& digest() const noexcept { return digest_; }
    // SWEGCA: user@2026-09-22:72-79
    [[nodiscard]] std::span<const std::byte> bytes() const noexcept { return bytes_; }
    [[nodiscard]] std::string_view journal_identity() const noexcept;
    // SWEGCA: user@2026-09-22:72-79
    [[nodiscard]] std::size_t extent_count() const noexcept { return extent_count_; }
    [[nodiscard]] SegmentExtent extent(std::size_t index) const;
    // SWEGCA: user@2026-09-22:72-79
    [[nodiscard]] std::size_t view_count() const noexcept { return view_offsets_.size(); }
    [[nodiscard]] ViewGeneration view(std::size_t index) const;

private:
    // SWEGCA: user@2026-09-22:72-79
    Manifest(LedgerBytes bytes, LedgerVector<std::uint32_t> view_offsets) noexcept
        : bytes_(std::move(bytes)), view_offsets_(std::move(view_offsets)) {}

    LedgerBytes bytes_;
    LedgerVector<std::uint32_t> view_offsets_;  // offsets into bytes_, never pointers
    ManifestFields fields_;
    std::size_t identity_offset_ = 0;
    std::size_t identity_size_ = 0;
    std::size_t extents_offset_ = 0;
    std::size_t extent_count_ = 0;
    Digest digest_{};
};

void append_manifest_log_header(LedgerBytes& out, std::uint64_t log_ordinal);
void check_manifest_log_header(std::span<const std::byte> bytes, std::uint64_t log_ordinal);

// Fixed-size published root: where the head manifest is and its digest.
struct HeadPointer {
    ManifestLocation location;
    Digest manifest_digest{};
};

inline constexpr std::size_t head_bytes = 4 + 2 + 3 * 8 + 32 + 32;
void append_head(LedgerBytes& out, const HeadPointer& head);
[[nodiscard]] HeadPointer decode_head(std::span<const std::byte> bytes);

}  // namespace swegca::architecture::journal
