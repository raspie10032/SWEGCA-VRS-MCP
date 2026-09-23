#include "swegca_architecture/journal_store.hpp"

#include "swegca_architecture/journal_file_io.hpp"
#include "swegca_architecture/resource_limits.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <iterator>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace swegca::architecture::journal {
namespace {

namespace fs = std::filesystem;

constexpr const char* head_name = "HEAD";
constexpr const char* lock_name = "owner.lock";
constexpr char segment_prefix = 'S';
constexpr char manifest_log_prefix = 'M';
constexpr char page_log_prefix = 'P';
constexpr const char* segment_suffix = ".swjs";
constexpr const char* manifest_log_suffix = ".swjm";
constexpr const char* page_log_suffix = ".swjp";
constexpr std::size_t ordinal_digits = 20;

// SWEGCA: user@2026-09-22:72-79
[[noreturn]] void fail(const char* code) { throw std::runtime_error(code); }

// Checked accumulation for byte counts.
// SWEGCA: user@2026-09-22:72-79
std::uint64_t plus(std::uint64_t left, std::uint64_t right, const char* code) {
    if (right > std::numeric_limits<std::uint64_t>::max() - left) fail(code);
    return left + right;
}

// SWEGCA: user@2026-09-22:72-79
std::uint64_t times(std::uint64_t left, std::uint64_t right, const char* code) {
    if (left != 0 && right > std::numeric_limits<std::uint64_t>::max() / left) fail(code);
    return left * right;
}

// On-disk charge of one file: its length rounded up to the allocation unit,
// plus one unit for its metadata. Lengths here are at most a segment or a
// log, so the arithmetic cannot overflow.
// SWEGCA: user@2026-09-22:72-79
std::uint64_t file_charge(std::uint64_t length, std::uint64_t unit) noexcept {
    return (length + unit - 1) / unit * unit + unit;
}

// SWEGCA: user@2026-09-22:72-79
std::string ordinal_name(char prefix, std::uint64_t value, const char* suffix) {
    char buffer[ordinal_digits + 1];
    std::snprintf(buffer, sizeof buffer, "%020llu", static_cast<unsigned long long>(value));
    return std::string(1, prefix) + buffer + suffix;
}

// SWEGCA: user@2026-09-22:72-79
fs::path segment_path(const fs::path& directory, std::uint64_t ordinal) {
    return directory / ordinal_name(segment_prefix, ordinal, segment_suffix);
}

// SWEGCA: user@2026-09-22:72-79
fs::path manifest_log_path(const fs::path& directory, std::uint64_t ordinal) {
    return directory / ordinal_name(manifest_log_prefix, ordinal, manifest_log_suffix);
}

// SWEGCA: user@2026-09-22:72-79
fs::path page_log_path(const fs::path& directory, std::uint64_t ordinal) {
    return directory / ordinal_name(page_log_prefix, ordinal, page_log_suffix);
}

// Parses `<prefix><20 digits><suffix>`; returns 0 when `name` is not one.
// SWEGCA: user@2026-09-22:72-79
std::uint64_t parse_ordinal(std::string_view name, char prefix, std::string_view suffix) {
    if (name.size() != 1 + ordinal_digits + suffix.size() || name.front() != prefix ||
        name.substr(1 + ordinal_digits) != suffix)
        return 0;
    std::uint64_t value = 0;
    for (std::size_t at = 1; at <= ordinal_digits; ++at) {
        const char digit = name[at];
        if (digit < '0' || digit > '9') return 0;
        const auto next = static_cast<std::uint64_t>(digit - '0');
        if (value > (std::numeric_limits<std::uint64_t>::max() - next) / 10) return 0;
        value = value * 10 + next;
    }
    return value;
}

// True for a name the journal publishes through `io::publish_file`; only a
// `.part` file under such a name is an interrupted publication.
// SWEGCA: user@2026-09-22:72-79
bool is_published_name(std::string_view name) {
    return name == head_name || parse_ordinal(name, segment_prefix, segment_suffix) != 0 ||
           parse_ordinal(name, manifest_log_prefix, manifest_log_suffix) != 0 ||
           parse_ordinal(name, page_log_prefix, page_log_suffix) != 0;
}

// Applies one manifest's extents on top of the published extent table: an
// existing ordinal may only grow when it is the tail, a new ordinal must
// follow the tail contiguously, and a checkpoint must name every extent.
// SWEGCA: user@2026-09-22:72-79
void apply_manifest(ExtentTable& extents, const Manifest& manifest) {
    for (std::size_t index = 0; index < manifest.extent_count(); ++index) {
        const auto next = manifest.extent(index);
        const auto tail_ordinal = extents.empty() ? 0 : extents.rbegin()->first;
        const auto found = extents.find(next.ordinal);
        if (found != extents.end()) {
            auto& old = found->second;
            const bool same = next.record_count == old.record_count;
            if (next.first_sequence != old.first_sequence) fail("journal_extent_conflict");
            if (same ? (next.byte_length != old.byte_length ||
                        next.last_record_digest != old.last_record_digest)
                     : (next.record_count < old.record_count ||
                        next.byte_length <= old.byte_length || next.ordinal != tail_ordinal))
                fail("journal_extent_conflict");
            old = next;
            continue;
        }
        if (next.ordinal != tail_ordinal + 1) fail("journal_extent_not_contiguous");
        const auto expected_first =
            extents.empty() ? 1
                            : extents.rbegin()->second.first_sequence +
                                  extents.rbegin()->second.record_count;
        if (next.first_sequence != expected_first) fail("journal_extent_not_contiguous");
        extents.emplace(next.ordinal, next);
    }
    const auto& fields = manifest.fields();
    if (fields.checkpoint && extents.size() != manifest.extent_count())
        fail("journal_checkpoint_incomplete");
    if (extents.size() > max_extents) fail("journal_capacity_exhausted");
    const bool empty = extents.empty();
    const auto& last = empty ? SegmentExtent{} : extents.rbegin()->second;
    const auto tail_sequence = empty ? 0 : last.first_sequence + last.record_count - 1;
    if (fields.tail_segment_ordinal != (empty ? 0 : last.ordinal) ||
        fields.tail_sequence != tail_sequence ||
        fields.tail_record_digest != (empty ? zero_digest : last.last_record_digest))
        fail("journal_manifest_tail_mismatch");
}

// Reads and decodes the manifest stored at `location` into ledger bytes.
// SWEGCA: user@2026-09-22:72-79
Manifest read_manifest(const fs::path& directory, const ManifestLocation& location,
                       const MemoryLedger::Account& memory) {
    if (location.log_ordinal == 0 || location.length == 0 ||
        location.length > max_manifest_bytes || location.offset < manifest_log_header_bytes ||
        location.offset > max_manifest_log_bytes)
        fail("journal_manifest_location_invalid");
    const auto path = manifest_log_path(directory, location.log_ordinal);
    const auto end = location.offset + location.length;
    std::array<std::byte, manifest_log_header_bytes> header{};
    io::read_range(path, end, 0, header, "journal_manifest_log_missing");
    check_manifest_log_header(header, location.log_ordinal);
    LedgerBytes bytes(static_cast<std::size_t>(location.length), memory.allocator<std::byte>());
    io::read_range(path, end, location.offset, bytes, "journal_manifest_log_missing");
    return Manifest::decode(std::move(bytes), memory);
}

// Generation 0 is built in a sibling directory and renamed into place without
// replacement, so a directory that exists either has a published HEAD or is
// not a journal (and then fails closed on open).
// SWEGCA: user@2026-09-22:72-79
void create_initial(const fs::path& directory, std::string_view identity,
                    const MemoryLedger::Account& memory) {
    const auto parent = directory.has_parent_path() ? directory.parent_path() : fs::path(".");
    fs::create_directories(parent);
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto temporary = parent / (directory.filename().string() + ".init-" +
                                     std::to_string(io::process_id()) + "-" + std::to_string(stamp));
    fs::create_directory(temporary);
    try {
        ManifestFields genesis;
        genesis.checkpoint = true;
        const auto manifest = Manifest::encode(genesis, identity, {}, {}, memory);
        LedgerBytes log_header(memory.allocator<std::byte>());
        log_header.reserve(manifest_log_header_bytes);
        append_manifest_log_header(log_header, 1);
        io::publish_file(manifest_log_path(temporary, 1), log_header, manifest.bytes());
        LedgerBytes head(memory.allocator<std::byte>());
        head.reserve(head_bytes);
        append_head(head, HeadPointer{{1, manifest_log_header_bytes, manifest.bytes().size()},
                                      manifest.digest()});
        io::publish_file(temporary / head_name, head);
        io::make_entries_durable(temporary);
        if (!io::rename_directory_no_replace(temporary, directory)) {
            // Another creator won; open what is there.
            std::error_code ignored;
            fs::remove_all(temporary, ignored);
            return;
        }
    } catch (...) {
        std::error_code ignored;
        fs::remove_all(temporary, ignored);
        throw;
    }
    io::make_entries_durable(parent);
}

// A view page read back: its exact bytes and the view decoded from them (the
// view points into `bytes`, whose buffer moves with it).
struct LoadedPage {
    LedgerBytes bytes;
    AddressPageView view;
};

// Reads one published page and requires the digest its parent (or the
// manifest) names.
// SWEGCA: user@2026-09-22:72-79
LoadedPage load_page(const fs::path& directory, const MemoryLedger::Account& memory,
                     const PageRef& ref) {
    LedgerBytes bytes(ref.length, memory.allocator<std::byte>());
    io::read_range(page_log_path(directory, ref.log_ordinal), ref.offset + ref.length, ref.offset,
                   bytes, "journal_page_log_missing");
    if (Sha256::of(bytes) != ref.digest) fail("journal_address_page_digest_mismatch");
    auto view = decode_address_page(bytes, memory);
    return LoadedPage{std::move(bytes), std::move(view)};
}

// Places the pages one generation writes into page logs: after the published
// end of the current log, or in new logs when it is full. Every page buffer
// stays where it was placed until publication, so views into it stay valid.
// SWEGCA: user@2026-09-22:72-79
struct PageWriter {
    const MemoryLedger::Account& memory;
    LedgerVector<PagePiece>& pieces;
    std::uint64_t log_ordinal;  // current page log; 0 before the first
    std::uint64_t log_end;      // its end, placed pages included
    std::uint64_t written = 0;  // bytes added to page logs, headers included
    std::uint64_t page_bytes = 0;

    // SWEGCA: user@2026-09-22:72-79
    std::span<const std::byte> place(LedgerBytes page, PageRef& ref) {
        const auto size = page.size();
        if (log_ordinal == 0 || log_end + size > max_page_log_bytes) {
            if (log_ordinal == std::numeric_limits<std::uint64_t>::max())
                fail("journal_page_log_exhausted");
            ++log_ordinal;
            log_end = page_log_header_bytes;
            pieces.push_back(PagePiece{log_ordinal, true, 0,
                                       LedgerBytes(memory.allocator<std::byte>()),
                                       LedgerVector<LedgerBytes>(memory.allocator<LedgerBytes>()),
                                       LedgerVector<std::span<const std::byte>>(
                                           memory.allocator<std::span<const std::byte>>())});
            pieces.back().header.reserve(page_log_header_bytes);
            append_page_log_header(pieces.back().header, log_ordinal);
            written += page_log_header_bytes;
        } else if (pieces.empty() || pieces.back().log_ordinal != log_ordinal) {
            pieces.push_back(PagePiece{log_ordinal, false, log_end,
                                       LedgerBytes(memory.allocator<std::byte>()),
                                       LedgerVector<LedgerBytes>(memory.allocator<LedgerBytes>()),
                                       LedgerVector<std::span<const std::byte>>(
                                           memory.allocator<std::span<const std::byte>>())});
        }
        ref = PageRef{log_ordinal, log_end, static_cast<std::uint32_t>(size), Sha256::of(page)};
        auto& pages = pieces.back().pages;
        pages.push_back(std::move(page));
        log_end += size;
        written += size;
        page_bytes += size;
        return pages.back();
    }
};

// Splits `items` into pages of at most `address_page_max_bytes`, spread
// evenly, and writes them; returns one child per page, keyed by its first
// address viewed in the written page.
// SWEGCA: user@2026-09-22:72-79
template <class Item, class Key, class Size, class Append>
LedgerVector<AddressChildItem> write_pages(PageWriter& writer, std::span<const Item> items,
                                           Key key, Size size_of, Append append) {
    std::uint64_t total = 0;
    for (const auto& item : items) total += size_of(item);
    constexpr std::uint64_t room = address_page_max_bytes - address_page_header_bytes;
    const auto pages = std::max<std::uint64_t>(1, (total + room - 1) / room);
    const auto target = (total + pages - 1) / pages;
    LedgerVector<AddressChildItem> out(writer.memory.allocator<AddressChildItem>());
    out.reserve(static_cast<std::size_t>(pages) + 1);
    std::size_t start = 0;
    while (start < items.size()) {
        std::uint64_t filled = 0;
        std::size_t end = start;
        while (end < items.size() && filled < target && filled + size_of(items[end]) <= room)
            filled += size_of(items[end++]);
        if (end == start) fail("journal_address_item_too_large");
        LedgerBytes page(writer.memory.allocator<std::byte>());
        page.reserve(static_cast<std::size_t>(address_page_header_bytes + filled));
        append(page, items.subspan(start, end - start));
        PageRef ref;
        const auto stored = writer.place(std::move(page), ref);
        out.push_back(AddressChildItem{
            std::string_view(reinterpret_cast<const char*>(stored.data()) +
                                 address_page_header_bytes + 4,
                             key(items[start]).size()),
            ref});
        start = end;
    }
    return out;
}

// SWEGCA: user@2026-09-22:72-79
LedgerVector<AddressChildItem> write_leaf_pages(PageWriter& writer,
                                                std::span<const AddressLeafItem> items) {
    return write_pages(
        writer, items, [](const AddressLeafItem& item) { return item.address; },
        [](const AddressLeafItem& item) { return encoded_leaf_item_size(item.address); },
        [](LedgerBytes& out, std::span<const AddressLeafItem> page) { append_leaf_page(out, page); });
}

// SWEGCA: user@2026-09-22:72-79
LedgerVector<AddressChildItem> write_branch_pages(PageWriter& writer,
                                                  std::span<const AddressChildItem> items) {
    return write_pages(
        writer, items, [](const AddressChildItem& item) { return item.first_address; },
        [](const AddressChildItem& item) { return encoded_child_item_size(item.first_address); },
        [](LedgerBytes& out, std::span<const AddressChildItem> page) {
            append_branch_page(out, page);
        });
}

// Inserts `added` (sorted, unique, all new) into the subtree at `ref` of
// `height` levels by copy on write: every page on the way is read, verified
// and rewritten, untouched children are kept by reference. Returns the pages
// that replace the subtree root (more than one when it split). The keys of
// kept children view the page read here, which lives until the replacement
// is written.
// SWEGCA: user@2026-09-22:72-79
LedgerVector<AddressChildItem> insert_into(PageWriter& writer, const fs::path& directory,
                                           const PageRef& ref, std::uint32_t height,
                                           std::span<const AddressLeafItem> added,
                                           std::uint64_t& replaced) {
    const auto page = load_page(directory, writer.memory, ref);
    if (page.view.leaf != (height == 1)) fail("journal_address_view_corrupt");
    replaced += ref.length;
    if (page.view.leaf) {
        const auto& old = page.view.leaves;
        LedgerVector<AddressLeafItem> merged(writer.memory.allocator<AddressLeafItem>());
        merged.reserve(old.size() + added.size());
        std::size_t left = 0;
        std::size_t right = 0;
        while (left < old.size() || right < added.size()) {
            if (right == added.size() ||
                (left < old.size() && old[left].address < added[right].address)) {
                merged.push_back(old[left++]);
            } else if (left == old.size() || added[right].address < old[left].address) {
                merged.push_back(added[right++]);
            } else {
                fail("journal_address_duplicate");
            }
        }
        return write_leaf_pages(writer, merged);
    }
    const auto& children = page.view.children;
    LedgerVector<AddressChildItem> next(writer.memory.allocator<AddressChildItem>());
    next.reserve(children.size() + 1);
    std::size_t from = 0;
    for (std::size_t at = 0; at < children.size(); ++at) {
        auto to = added.size();
        if (at + 1 < children.size()) {
            const auto bound = std::lower_bound(
                added.begin() + static_cast<std::ptrdiff_t>(from), added.end(),
                children[at + 1].first_address,
                [](const AddressLeafItem& item, std::string_view key) { return item.address < key; });
            to = static_cast<std::size_t>(bound - added.begin());
        }
        if (to == from) {
            next.push_back(children[at]);
            continue;
        }
        const auto replacement =
            insert_into(writer, directory, children[at].page, height - 1,
                        added.subspan(from, to - from), replaced);
        next.insert(next.end(), replacement.begin(), replacement.end());
        from = to;
    }
    return write_branch_pages(writer, next);
}

// The tree after adding `added` (sorted, unique): new pages for every changed
// path and a taller root when the old root split.
// SWEGCA: user@2026-09-22:72-79
ViewTree update_tree(PageWriter& writer, const fs::path& directory, const ViewTree& tree,
                     std::span<const AddressLeafItem> added) {
    if (added.empty()) return tree;
    const auto written_before = writer.page_bytes;
    std::uint64_t replaced = 0;
    std::uint32_t height = 1;
    auto level = tree.entry_count == 0
                     ? write_leaf_pages(writer, added)
                     : insert_into(writer, directory, tree.root, tree.height, added, replaced);
    if (tree.entry_count != 0) height = tree.height;
    while (level.size() > 1) {
        if (height == max_address_height) fail("journal_address_view_too_tall");
        level = write_branch_pages(writer, level);
        ++height;
    }
    if (replaced > tree.live_page_bytes) fail("journal_address_view_corrupt");
    ViewTree next;
    next.entry_count = plus(tree.entry_count, added.size(), "journal_address_count_overflow");
    next.height = height;
    next.root = level.front().page;
    next.live_page_bytes = tree.live_page_bytes - replaced + (writer.page_bytes - written_before);
    return next;
}

// The view pages after both trees took their additions through `writer`,
// which places every page of this generation in the shared page logs.
// SWEGCA: user@2026-09-22:72-79
ViewPages update_views(PageWriter& writer, const fs::path& directory, const ViewPages& pages,
                       std::span<const AddressLeafItem> addresses,
                       std::span<const AddressLeafItem> cues) {
    ViewPages next = pages;
    next.addresses = update_tree(writer, directory, pages.addresses, addresses);
    next.cues = update_tree(writer, directory, pages.cues, cues);
    if (writer.written == 0) return next;
    next.first_page_log = pages.page_log_ordinal == 0 ? 1 : pages.first_page_log;
    next.page_log_ordinal = writer.log_ordinal;
    next.page_log_end = writer.log_end;
    next.page_log_bytes = plus(pages.page_log_bytes, writer.written, "journal_storage_overflow");
    return next;
}

// Appends `text` to `keys`, which has capacity for it (so nothing moves).
// SWEGCA: user@2026-09-22:72-79
void append_text(LedgerBytes& keys, std::string_view text) {
    if (text.size() > keys.capacity() - keys.size()) fail("journal_key_capacity");
    const auto* bytes = reinterpret_cast<const std::byte*>(text.data());
    keys.insert(keys.end(), bytes, bytes + text.size());
}

// Appends the key of one view entry to `keys`, which has capacity for it,
// and views it there: the address itself when `cue` is empty, otherwise
// the cue-view key (cue, separator, address).
// SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:398-437
std::string_view append_view_key(LedgerBytes& keys, std::string_view cue, std::string_view address) {
    const auto at = keys.size();
    const auto size = cue.empty() ? address.size() : cue_key_size(cue, address);
    if (size > keys.capacity() - at) fail("journal_key_capacity");
    if (!cue.empty()) {
        append_text(keys, cue);
        keys.push_back(static_cast<std::byte>(cue_separator));
    }
    append_text(keys, address);
    return std::string_view(reinterpret_cast<const char*>(keys.data()) + at, size);
}

// The tree a builder finished, with the bytes of its pages.
// SWEGCA: user@2026-09-22:72-79
ViewTree tree_of(std::uint64_t count, std::uint32_t height, const PageRef& root,
                 std::uint64_t page_bytes) {
    if (count == 0) {
        if (page_bytes != 0) fail("journal_address_view_corrupt");
        return ViewTree{};
    }
    return ViewTree{count, height, root, page_bytes};
}

// Pages a view rewrite keeps in memory before appending them to disk.
constexpr std::size_t stream_flush_bytes = 1u << 20;
// Keys and items one view-rebuild pass sorts in memory.
constexpr std::size_t rebuild_batch_key_bytes = 64u << 20;
constexpr std::size_t rebuild_batch_items = 1u << 20;
// Most items one page holds: the smallest item has a one-byte key.
constexpr std::size_t max_page_items =
    (address_page_max_bytes - address_page_header_bytes) / encoded_child_item_size("x");

// Writes view pages straight into new page logs that no HEAD names yet; a
// reopen removes them unless a HEAD publishes them. Each log is charged by
// the storage rule (its bytes plus two allocation units) against
// `allowance` before any of its bytes is written.
// SWEGCA: user@2026-09-22:72-79
class StreamWriter {
public:
    // SWEGCA: user@2026-09-22:72-79
    StreamWriter(const fs::path& directory, const MemoryLedger::Account& memory,
                 std::uint64_t first_log, std::uint64_t allowance, std::uint64_t unit)
        : directory_(&directory), first_log_(first_log), allowance_(allowance), unit_(unit),
          pending_(memory.allocator<std::byte>()) {
        if (first_log_ == 0) fail("journal_page_log_exhausted");
        pending_.reserve(stream_flush_bytes);
    }

    // SWEGCA: user@2026-09-22:72-79
    PageRef write(std::span<const std::byte> page) {
        const auto size = page.size();
        if (log_ordinal_ == 0 || log_end_ + size > max_page_log_bytes) {
            flush();
            const auto next = next_ordinal();
            charge(page_log_header_bytes + 2 * unit_);
            log_ordinal_ = next;
            file_length_ = 0;
            log_end_ = page_log_header_bytes;
            written_ += page_log_header_bytes;
            append_page_log_header(pending_, log_ordinal_);
        }
        charge(size);
        if (pending_.size() + size > pending_.capacity()) flush();
        const PageRef ref{log_ordinal_, log_end_, static_cast<std::uint32_t>(size), Sha256::of(page)};
        pending_.insert(pending_.end(), page.begin(), page.end());
        log_end_ += size;
        written_ += size;
        page_bytes_ += size;
        return ref;
    }

    // Appends what is pending to the current log and makes it durable.
    // SWEGCA: user@2026-09-22:72-79
    void flush() {
        if (pending_.empty()) return;
        const auto path = page_log_path(*directory_, log_ordinal_);
        if (file_length_ == 0)
            io::publish_file(path, pending_);
        else
            io::append_at_published_end(path, file_length_, pending_);
        file_length_ += pending_.size();
        pending_.clear();
    }

    // Removes every log written here; true when none is left. No HEAD may
    // name them (they are removed only before publication or when replaced).
    // SWEGCA: user@2026-09-22:72-79
    [[nodiscard]] bool remove_written() const noexcept {
        if (log_ordinal_ == 0) return true;
        try {
            for (auto ordinal = first_log_;; ++ordinal) {
                auto path = page_log_path(*directory_, ordinal);
                std::error_code error;
                fs::remove(path, error);
                if (error) return false;
                path += io::part_suffix;  // a publication interrupted in flush
                fs::remove(path, error);
                if (error) return false;
                if (ordinal == log_ordinal_) return true;
            }
        } catch (...) {
            return false;
        }
    }

    // Flushes and gives back the write buffer; the logs stay as written.
    // SWEGCA: user@2026-09-22:72-79
    void close() {
        flush();
        pending_ = LedgerBytes(pending_.get_allocator());
    }

    // SWEGCA: user@2026-09-22:72-79
    [[nodiscard]] std::uint64_t next_ordinal() const {
        if (log_ordinal_ == 0) return first_log_;
        if (log_ordinal_ == std::numeric_limits<std::uint64_t>::max())
            fail("journal_page_log_exhausted");
        return log_ordinal_ + 1;
    }

    // The view pages over the two trees written here (each given with the
    // bytes of its pages; every page written here is live) and their logs.
    // SWEGCA: user@2026-09-22:72-79
    [[nodiscard]] ViewPages pages(const ViewTree& addresses, const ViewTree& cues) const {
        if (addresses.live_page_bytes + cues.live_page_bytes != page_bytes_)
            fail("journal_address_view_corrupt");
        ViewPages out;
        out.addresses = addresses;
        out.cues = cues;
        if (log_ordinal_ == 0) return out;
        out.first_page_log = first_log_;
        out.page_log_ordinal = log_ordinal_;
        out.page_log_end = log_end_;
        out.page_log_bytes = written_;
        return out;
    }

    // SWEGCA: user@2026-09-22:72-79
    [[nodiscard]] std::uint64_t page_bytes() const noexcept { return page_bytes_; }

    // SWEGCA: user@2026-09-22:72-79
    [[nodiscard]] std::uint64_t charged() const noexcept { return charged_; }

private:
    // SWEGCA: user@2026-09-22:72-79
    void charge(std::uint64_t bytes) {
        if (bytes > allowance_ - charged_) fail("journal_storage_budget_exhausted");
        charged_ += bytes;
    }

    const fs::path* directory_;
    std::uint64_t first_log_;
    std::uint64_t allowance_;
    std::uint64_t unit_;
    LedgerBytes pending_;
    std::uint64_t log_ordinal_ = 0;  // current log; 0 before the first
    std::uint64_t file_length_ = 0;  // bytes of the current log on disk
    std::uint64_t log_end_ = 0;      // its end, pending bytes included
    std::uint64_t written_ = 0;      // bytes of every log written, headers included
    std::uint64_t page_bytes_ = 0;
    std::uint64_t charged_ = 0;
};

struct BuiltTree {
    std::uint64_t count = 0;
    std::uint32_t height = 0;
    PageRef root;
};

// Walks the leaf items of a tree in address order, holding one verified page
// per level; the item it shows is valid until `next`.
// SWEGCA: user@2026-09-22:72-79
class LeafCursor {
public:
    // SWEGCA: user@2026-09-22:72-79
    LeafCursor(const fs::path& directory, const MemoryLedger::Account& memory, const BuiltTree& tree)
        : directory_(&directory), memory_(memory), frames_(memory.allocator<Frame>()) {
        frames_.reserve(max_address_height);  // frames never move
        if (tree.count != 0) descend(tree.root, tree.height);
    }

    // Positioned at the first item whose key is not below `from`: one page
    // per level on the way down.
    // SWEGCA: user@2026-09-22:72-79
    LeafCursor(const fs::path& directory, const MemoryLedger::Account& memory, const BuiltTree& tree,
               std::string_view from)
        : directory_(&directory), memory_(memory), frames_(memory.allocator<Frame>()) {
        frames_.reserve(max_address_height);  // frames never move
        if (tree.count == 0) return;
        PageRef ref = tree.root;
        for (std::uint32_t height = tree.height;; --height) {
            if (height == 0) fail("journal_address_view_corrupt");
            auto page = load_page(*directory_, memory_, ref);
            if (page.view.leaf != (height == 1)) fail("journal_address_view_corrupt");
            if (page.view.leaf) {
                const auto& leaves = page.view.leaves;
                const auto found = std::lower_bound(
                    leaves.begin(), leaves.end(), from,
                    [](const AddressLeafItem& item, std::string_view key) { return item.address < key; });
                const auto at = static_cast<std::size_t>(found - leaves.begin());
                frames_.push_back(Frame{std::move(page), at, height});
                settle();
                return;
            }
            const auto& children = page.view.children;
            const auto after = std::upper_bound(
                children.begin(), children.end(), from,
                [](std::string_view key, const AddressChildItem& child) { return key < child.first_address; });
            const auto at = after == children.begin()
                                ? std::size_t{0}
                                : static_cast<std::size_t>(after - children.begin()) - 1;
            ref = children[at].page;
            frames_.push_back(Frame{std::move(page), at, height});
        }
    }

    // SWEGCA: user@2026-09-22:72-79
    [[nodiscard]] bool valid() const noexcept { return !frames_.empty(); }

    // SWEGCA: user@2026-09-22:72-79
    [[nodiscard]] const AddressLeafItem& item() const {
        const auto& leaf = frames_.back();
        return leaf.page.view.leaves[leaf.at];
    }

    // SWEGCA: user@2026-09-22:72-79
    void next() {
        ++frames_.back().at;
        settle();
    }

private:
    struct Frame {
        LoadedPage page;
        std::size_t at = 0;
        std::uint32_t height = 0;
    };

    // Moves past exhausted pages to the next item, down to its leaf.
    // SWEGCA: user@2026-09-22:72-79
    void settle() {
        for (;;) {
            const auto& top = frames_.back();
            const auto size =
                top.page.view.leaf ? top.page.view.leaves.size() : top.page.view.children.size();
            if (top.at < size) break;
            frames_.pop_back();
            if (frames_.empty()) return;
            ++frames_.back().at;
        }
        const auto& top = frames_.back();
        if (!top.page.view.leaf) descend(top.page.view.children[top.at].page, top.height - 1);
    }

    // SWEGCA: user@2026-09-22:72-79
    void descend(PageRef ref, std::uint32_t height) {
        for (;;) {
            if (height == 0) fail("journal_address_view_corrupt");
            auto page = load_page(*directory_, memory_, ref);
            if (page.view.leaf != (height == 1)) fail("journal_address_view_corrupt");
            const bool leaf = page.view.leaf;
            if (!leaf) ref = page.view.children.front().page;
            frames_.push_back(Frame{std::move(page), 0, height});
            if (leaf) return;
            --height;
        }
    }

    const fs::path* directory_;
    MemoryLedger::Account memory_;
    LedgerVector<Frame> frames_;
};

// One sorted run a rebuild wrote, and the writer that owns its logs.
struct Run {
    BuiltTree tree;
    StreamWriter writer;
};

// Feeds `builder` the union of `runs` in address order with a binary heap of
// cursors: O(log k) per item, one page per level per run in memory. An
// address in two runs fails as a duplicate.
// SWEGCA: user@2026-09-22:72-79
void merge_runs(TreeBuilder& builder, const fs::path& directory, const MemoryLedger::Account& memory,
                std::span<const Run> runs) {
    LedgerVector<LeafCursor> cursors(memory.allocator<LeafCursor>());
    cursors.reserve(runs.size());
    for (const auto& run : runs) cursors.emplace_back(directory, memory, run.tree);
    LedgerVector<std::size_t> heap(memory.allocator<std::size_t>());
    heap.reserve(cursors.size());
    const auto later = [&cursors](std::size_t left, std::size_t right) {
        return cursors[right].item().address < cursors[left].item().address;
    };
    for (std::size_t at = 0; at < cursors.size(); ++at) {
        if (!cursors[at].valid()) continue;
        heap.push_back(at);
        std::push_heap(heap.begin(), heap.end(), later);
    }
    while (!heap.empty()) {
        std::pop_heap(heap.begin(), heap.end(), later);
        const auto at = heap.back();
        builder.add(cursors[at].item());  // copies the key before the cursor moves
        cursors[at].next();
        if (cursors[at].valid())
            std::push_heap(heap.begin(), heap.end(), later);
        else
            heap.pop_back();
    }
}

// Builds a view tree bottom-up from leaf items in strictly increasing address
// order, writing each page as soon as it is full, so memory holds one open
// page per level whatever the tree size. Keys are copied into each level's
// own buffer, so an item need live only for the call that adds it.
// SWEGCA: user@2026-09-22:72-79
class TreeBuilder {
public:
    // SWEGCA: user@2026-09-22:72-79
    TreeBuilder(StreamWriter& writer, const MemoryLedger::Account& memory)
        : writer_(writer), memory_(memory), levels_(memory.allocator<Level>()),
          last_(memory.allocator<std::byte>()) {
        levels_.reserve(max_address_height);  // levels never move: keys view them
        levels_.emplace_back(memory_, true);
        last_.reserve(detail::identity_text_max_bytes);
    }

    // SWEGCA: user@2026-09-22:72-79
    void add(const AddressLeafItem& item) {
        const std::string_view last(reinterpret_cast<const char*>(last_.data()), last_.size());
        if (count_ != 0 && item.address <= last)
            fail(item.address == last ? "journal_address_duplicate" : "journal_address_order_invalid");
        if (item.address.size() > last_.capacity()) fail("journal_address_page_invalid");
        auto& level = levels_[0];
        const auto size = encoded_leaf_item_size(item.address);
        if (level.bytes + size > address_page_max_bytes) flush(0);
        level.leaves.push_back(AddressLeafItem{keep(level, item.address), item.position});
        level.bytes += size;
        const auto* bytes = reinterpret_cast<const std::byte*>(item.address.data());
        last_.assign(bytes, bytes + item.address.size());
        ++count_;
    }

    // Writes the open pages bottom-up; the single page of the top level is
    // the root. An empty tree writes nothing.
    // SWEGCA: user@2026-09-22:72-79
    BuiltTree finish() {
        if (count_ == 0) return BuiltTree{};
        for (std::size_t at = 0;; ++at) {
            auto& level = levels_[at];
            if (at + 1 == levels_.size() && level.pages == 0)
                return BuiltTree{count_, static_cast<std::uint32_t>(at + 1), write_page(level)};
            flush(at);
        }
    }

private:
    struct Level {
        // SWEGCA: user@2026-09-22:72-79
        Level(const MemoryLedger::Account& memory, bool leaf_level)
            : leaf(leaf_level), keys(memory.allocator<std::byte>()),
              leaves(memory.allocator<AddressLeafItem>()),
              children(memory.allocator<AddressChildItem>()) {
            keys.reserve(address_page_max_bytes);  // a page's keys fit its page
            if (leaf) leaves.reserve(max_page_items);
            else children.reserve(max_page_items);
        }

        bool leaf;
        LedgerBytes keys;
        LedgerVector<AddressLeafItem> leaves;
        LedgerVector<AddressChildItem> children;
        std::size_t bytes = address_page_header_bytes;
        std::uint64_t pages = 0;
    };

    // SWEGCA: user@2026-09-22:72-79
    static std::string_view keep(Level& level, std::string_view key) {
        const auto at = level.keys.size();
        if (key.size() > level.keys.capacity() - at) fail("journal_address_page_invalid");
        const auto* bytes = reinterpret_cast<const std::byte*>(key.data());
        level.keys.insert(level.keys.end(), bytes, bytes + key.size());
        return std::string_view(reinterpret_cast<const char*>(level.keys.data()) + at, key.size());
    }

    // SWEGCA: user@2026-09-22:72-79
    PageRef write_page(const Level& level) {
        LedgerBytes page(memory_.allocator<std::byte>());
        page.reserve(level.bytes);
        if (level.leaf)
            append_leaf_page(page, level.leaves);
        else
            append_branch_page(page, level.children);
        return writer_.write(page);
    }

    // SWEGCA: user@2026-09-22:72-79
    void push_child(std::size_t at, std::string_view first, const PageRef& ref) {
        if (at == levels_.size()) {
            if (at == max_address_height) fail("journal_address_view_too_tall");
            levels_.emplace_back(memory_, false);
        }
        auto& level = levels_[at];
        const auto size = encoded_child_item_size(first);
        if (level.bytes + size > address_page_max_bytes) flush(at);
        level.children.push_back(AddressChildItem{keep(level, first), ref});
        level.bytes += size;
    }

    // Writes the open page of level `at` and hands its first key and
    // reference to the level above, which copies the key before this level's
    // buffer is reused.
    // SWEGCA: user@2026-09-22:72-79
    void flush(std::size_t at) {
        auto& level = levels_[at];
        if (level.bytes == address_page_header_bytes) return;
        const auto ref = write_page(level);
        const auto first = level.leaf ? level.leaves.front().address : level.children.front().first_address;
        push_child(at + 1, first, ref);
        level.keys.clear();
        level.leaves.clear();
        level.children.clear();
        level.bytes = address_page_header_bytes;
        ++level.pages;
    }

    StreamWriter& writer_;
    const MemoryLedger::Account& memory_;
    LedgerVector<Level> levels_;
    LedgerBytes last_;
    std::uint64_t count_ = 0;
};

// Visits the leaf items of the tree at `ref` in address order, holding one
// verified page per level.
// SWEGCA: user@2026-09-22:72-79
template <class Visit>
void visit_leaves(const fs::path& directory, const MemoryLedger::Account& memory,
                  const PageRef& ref, std::uint32_t height, Visit& visit) {
    if (height == 0) fail("journal_address_view_corrupt");
    const auto page = load_page(directory, memory, ref);
    if (page.view.leaf != (height == 1)) fail("journal_address_view_corrupt");
    if (page.view.leaf) {
        for (const auto& item : page.view.leaves) visit(item);
        return;
    }
    for (const auto& child : page.view.children)
        visit_leaves(directory, memory, child.page, height - 1, visit);
}

// Feeds `builder` the union of the tree `previous` (when given) and `batch`
// (sorted), in address order; an address in both fails as a duplicate.
// SWEGCA: user@2026-09-22:72-79
void merge_into(TreeBuilder& builder, const fs::path& directory, const MemoryLedger::Account& memory,
                const BuiltTree* previous, std::span<const AddressLeafItem> batch) {
    std::size_t next = 0;
    auto visit = [&](const AddressLeafItem& item) {
        while (next < batch.size() && batch[next].address < item.address) builder.add(batch[next++]);
        builder.add(item);
    };
    if (previous != nullptr && previous->count != 0)
        visit_leaves(directory, memory, previous->root, previous->height, visit);
    for (; next < batch.size(); ++next) builder.add(batch[next]);
}

// SWEGCA: user@2026-09-22:72-79
std::uint64_t next_page_log(const ViewPages& view) {
    if (view.page_log_ordinal == std::numeric_limits<std::uint64_t>::max())
        fail("journal_page_log_exhausted");
    return view.page_log_ordinal + 1;
}

}  // namespace

// SWEGCA: user@2026-09-22:72-79
PublishedRecord::PublishedRecord(LedgerBytes bytes, const RecordPosition& position)
    : bytes_(std::move(bytes)), position_(position) {
    ByteReader reader(bytes_);
    view_ = decode_record(reader);
    if (reader.remaining() != 0 || view_.sequence != position_.sequence ||
        view_.record_digest != position_.record_digest)
        fail("journal_position_mismatch");
}

// The source keeps no view of the bytes it gave away. A vector move carries
// the buffer itself, so the moved view still points into owned bytes.
// SWEGCA: user@2026-09-22:72-79
PublishedRecord::PublishedRecord(PublishedRecord&& other) noexcept
    : bytes_(std::move(other.bytes_)),
      view_(std::exchange(other.view_, RecordView{})),
      position_(std::exchange(other.position_, RecordPosition{})) {}

// SWEGCA: user@2026-09-22:72-79
StagedGeneration::StagedGeneration(const MemoryLedger::Account& memory)
    : head_bytes_(memory.allocator<std::byte>()), log_header_(memory.allocator<std::byte>()),
      pieces_(memory.allocator<SegmentPiece>()), page_pieces_(memory.allocator<PagePiece>()),
      positions_(memory.allocator<RecordPosition>()) {}

// A moved-from staged generation is invalid and can never be published.
// SWEGCA: user@2026-09-22:72-79
StagedGeneration::StagedGeneration(StagedGeneration&& other) noexcept
    : valid_(std::exchange(other.valid_, false)), parent_generation_(other.parent_generation_),
      parent_manifest_digest_(other.parent_manifest_digest_),
      head_bytes_(std::move(other.head_bytes_)), log_header_(std::move(other.log_header_)),
      pieces_(std::move(other.pieces_)), page_pieces_(std::move(other.page_pieces_)),
      positions_(std::move(other.positions_)), next_(std::move(other.next_)) {}

// SWEGCA: user@2026-09-22:72-79
JournalStore::JournalStore(fs::path directory, JournalIdentity identity,
                           std::uint64_t storage_bytes, const MemoryLedger::Account& memory,
                           std::uint64_t allocation_unit, std::unique_ptr<io::OwnerLock> lock)
    : directory_(std::move(directory)), identity_(std::move(identity)),
      storage_bytes_(storage_bytes), memory_(memory), allocation_unit_(allocation_unit),
      lock_(std::move(lock)), retired_(memory.allocator<RetiredLogs>()) {}

// SWEGCA: user@2026-09-22:72-79
std::unique_ptr<JournalStore> JournalStore::open(const fs::path& directory,
                                                 std::string_view identity,
                                                 std::uint64_t storage_bytes,
                                                 const MemoryLedger::Account& memory) {
    if (storage_bytes == 0 || storage_bytes > ResourceLimits::max_storage_bytes)
        fail("journal_budget_invalid");
    JournalIdentity owned(memory, identity);  // checked before anything is created
    if (!fs::exists(directory)) create_initial(directory, identity, memory);
    auto lock = std::make_unique<io::OwnerLock>(directory);
    const auto unit = io::allocation_unit(directory);
    std::unique_ptr<JournalStore> store(new JournalStore(directory, std::move(owned), storage_bytes,
                                                         memory, unit, std::move(lock)));
    store->load_published_head();
    return store;
}

// On-disk charge of the published journal: every file is charged by
// `file_charge`. Sealed manifest and page logs are charged by their exact
// published bytes plus the rounding bound per log. HEAD is charged twice:
// its replacement is written beside it before the move.
// SWEGCA: user@2026-09-22:72-79
std::uint64_t JournalStore::storage_of(const ExtentTable& extents, const ManifestFields& head,
                                       const ManifestLocation& location) const {
    constexpr const char* code = "journal_storage_overflow";
    const auto unit = allocation_unit_;
    const auto per_log = plus(manifest_log_header_bytes, times(2, unit, code), code);
    std::uint64_t total = file_charge(0, unit);  // owner.lock
    total = plus(total, times(file_charge(head_bytes, unit), 2, code), code);
    for (const auto& [ordinal, extent] : extents)
        total = plus(total, file_charge(extent.byte_length, unit), code);
    total = plus(total, plus(head.manifest_bytes_before, location.length, code), code);
    total = plus(total, times(location.log_ordinal, per_log, code), code);
    return plus(total, page_log_charge(head.view_pages), code);
}

// The views' page logs: their exact bytes plus two allocation units per log.
// SWEGCA: user@2026-09-22:72-79
std::uint64_t JournalStore::page_log_charge(const ViewPages& view) const {
    constexpr const char* code = "journal_storage_overflow";
    if (view.page_log_ordinal == 0) return 0;
    const auto logs = view.page_log_ordinal - view.first_page_log + 1;
    return plus(view.page_log_bytes, times(logs, times(2, allocation_unit_, code), code), code);
}

// What may still be written: the reservation less the published use and
// the logs a rewrite left behind.
// SWEGCA: user@2026-09-22:72-79
std::uint64_t JournalStore::allowance(const PublishedSnapshot& current) const {
    const auto used = plus(current.storage, retained_bytes_.load(), "journal_storage_overflow");
    return used >= storage_bytes_ ? 0 : storage_bytes_ - used;
}

// Recovery reads HEAD, then walks back to the latest checkpoint. Each step
// checks that the manifest sits exactly where its successor says, that the
// byte counters chain, and that the total read stays within
// `max_recovery_bytes`; nothing past HEAD is read. Then unpublished
// leftovers and unreachable page logs are removed so disk use equals what is
// charged.
// SWEGCA: user@2026-09-22:72-79
void JournalStore::load_published_head() {
    // First pass: the directory holds only journal entries. A `.part` file is
    // an interrupted publication only under a name the journal publishes, and
    // then it goes; any other entry fails closed. Nothing is collected.
    bool removed = false;
    for (const auto& entry : fs::directory_iterator(directory_)) {
        const auto name = entry.path().filename().string();
        if (!entry.is_regular_file()) fail("journal_unknown_entry");
        if (name == head_name || name == lock_name) continue;
        const std::string_view part(io::part_suffix);
        if (name.ends_with(part)) {
            if (!is_published_name(std::string_view(name).substr(0, name.size() - part.size())))
                fail("journal_unknown_entry");
            io::remove_file(entry.path());
            removed = true;
            continue;
        }
        if (!is_published_name(name)) fail("journal_unknown_entry");
    }

    std::array<std::byte, head_bytes> head_image{};
    io::read_exact_file(directory_ / head_name, head_image, "journal_head_missing");
    const auto pointer = decode_head(head_image);
    std::uint64_t read_bytes = pointer.location.length;
    if (read_bytes > max_recovery_bytes) fail("journal_recovery_over_budget");

    auto loaded = std::allocate_shared<PublishedSnapshot>(
        memory_.allocator<PublishedSnapshot>(), memory_,
        read_manifest(directory_, pointer.location, memory_));
    const auto& head = loaded->head;
    const auto& fields = head.fields();
    if (head.digest() != pointer.manifest_digest) fail("journal_head_digest_mismatch");
    if (head.journal_identity() != identity_.value()) fail("journal_identity_mismatch");
    if (fields.recovery_bytes_before > max_recovery_bytes - pointer.location.length)
        fail("journal_recovery_over_budget");

    {
        // `older` runs from the head's parent back to the checkpoint. Its
        // capacity is fixed up front, so `walk` stays valid while it grows.
        LedgerVector<Manifest> older(memory_.allocator<Manifest>());
        older.reserve(checkpoint_interval);
        const Manifest* walk = &head;
        auto walk_location = pointer.location;
        while (!walk->fields().checkpoint) {
            const auto& after = walk->fields();
            const auto previous_location = after.previous_location;
            if (!follows(previous_location, walk_location)) fail("journal_manifest_chain_invalid");
            read_bytes = plus(read_bytes, previous_location.length, "journal_recovery_over_budget");
            if (read_bytes > max_recovery_bytes) fail("journal_recovery_over_budget");
            if (older.size() + 1 >= checkpoint_interval) fail("journal_manifest_chain_invalid");
            auto previous = read_manifest(directory_, previous_location, memory_);
            const auto& before = previous.fields();
            if (previous.digest() != after.previous_manifest_digest ||
                before.generation + 1 != after.generation ||
                previous.journal_identity() != identity_.value() ||
                after.manifest_bytes_before !=
                    plus(before.manifest_bytes_before, previous_location.length,
                         "journal_manifest_chain_invalid") ||
                after.recovery_bytes_before !=
                    before.recovery_bytes_before + previous_location.length ||
                after.checkpoint_generation != before.checkpoint_generation)
                fail("journal_manifest_chain_invalid");
            older.push_back(std::move(previous));
            walk = &older.back();
            walk_location = previous_location;
        }
        for (auto manifest = older.rbegin(); manifest != older.rend(); ++manifest)
            apply_manifest(loaded->extents, *manifest);
    }
    apply_manifest(loaded->extents, head);
    loaded->location = pointer.location;
    loaded->page_logs = std::allocate_shared<int>(memory_.allocator<int>(), 0);
    loaded->storage = storage_of(loaded->extents, fields, pointer.location);
    if (loaded->storage > storage_bytes_) fail("journal_storage_budget_exceeded");

    // Second pass, now that the published tails are known: files past them
    // are unpublished leftovers and go, and so do page logs older than the
    // oldest one the view reaches; a segment below the tail that no extent
    // names is not this journal's. A missing page log in the view's range
    // leaves the view unavailable until it is rebuilt from the records.
    // Then bytes past each published end are cut.
    const auto tail_ordinal = fields.tail_segment_ordinal;
    const auto& view = fields.view_pages;
    std::uint64_t page_logs = 0;
    bool last_log_present = false;
    for (const auto& entry : fs::directory_iterator(directory_)) {
        const auto name = entry.path().filename().string();
        if (const auto ordinal = parse_ordinal(name, segment_prefix, segment_suffix)) {
            if (ordinal > tail_ordinal) {
                io::remove_file(entry.path());
                removed = true;
            } else if (!loaded->extents.contains(ordinal)) {
                fail("journal_segment_unaccounted");
            }
        } else if (const auto log = parse_ordinal(name, manifest_log_prefix, manifest_log_suffix)) {
            if (log > loaded->location.log_ordinal) {
                io::remove_file(entry.path());
                removed = true;
            }
        } else if (const auto page_log = parse_ordinal(name, page_log_prefix, page_log_suffix)) {
            if (page_log > view.page_log_ordinal || page_log < view.first_page_log) {
                io::remove_file(entry.path());
                removed = true;
            } else {
                ++page_logs;
                last_log_present = last_log_present || page_log == view.page_log_ordinal;
            }
        }
    }
    const auto expected_page_logs =
        view.page_log_ordinal == 0 ? 0 : view.page_log_ordinal - view.first_page_log + 1;
    if (page_logs != expected_page_logs) loaded->view_unavailable = true;
    if (tail_ordinal != 0)
        io::append_at_published_end(segment_path(directory_, tail_ordinal),
                                    loaded->extents.at(tail_ordinal).byte_length, {});
    io::append_at_published_end(manifest_log_path(directory_, loaded->location.log_ordinal),
                                loaded->location.offset + loaded->location.length, {});
    // The last log is cut whenever it exists, so no unpublished byte stays
    // uncharged; a log that cannot be cut (an I/O failure, a length short of
    // its published end) is a damaged derived view, not a damaged journal.
    if (last_log_present) {
        try {
            io::append_at_published_end(page_log_path(directory_, view.page_log_ordinal),
                                        view.page_log_end, {});
        } catch (const std::runtime_error&) {
            loaded->view_unavailable = true;  // a derived log only; the records decide
        }
    }
    if (removed) io::make_entries_durable(directory_);

    std::shared_ptr<const PublishedSnapshot> published = std::move(loaded);
    snapshot_.store(published);

    // The extents the newest generation wrote are the ones to check; a
    // checkpoint head checks its tail extent.
    const auto& newest = published->head;
    if (newest.fields().checkpoint) {
        if (!published->extents.empty())
            verify_extent(*published, published->extents.rbegin()->second);
    } else {
        for (std::size_t index = 0; index < newest.extent_count(); ++index)
            verify_extent(*published, published->extents.at(newest.extent(index).ordinal));
    }
}

// SWEGCA: user@2026-09-22:72-79
void JournalStore::require_usable() const {
    if (poisoned_.load()) fail("journal_store_poisoned");
}

// SWEGCA: user@2026-09-22:72-79
std::shared_ptr<const PublishedSnapshot> JournalStore::snapshot() const {
    auto current = snapshot_.load();
    if (!current) fail("journal_store_not_loaded");
    return current;
}

// SWEGCA: user@2026-09-22:72-79
std::shared_ptr<const Manifest> JournalStore::head() const {
    require_usable();
    auto current = snapshot();
    return std::shared_ptr<const Manifest>(current, &current->head);
}

// SWEGCA: user@2026-09-22:72-79
StateGeneration JournalStore::state_generation() const {
    require_usable();
    const auto current = snapshot();
    const auto& fields = current->head.fields();
    return StateGeneration(fields.state_generation_ordinal,
                           Digest256(fields.state_generation_digest));
}

// SWEGCA: user@2026-09-22:72-79
std::uint64_t JournalStore::storage_charged() const {
    require_usable();
    const auto current = snapshot();  // before the retained charge (see stage)
    return plus(current->storage, retained_bytes_.load(), "journal_storage_overflow");
}

// Verifies one published extent's whole record chain from its header.
// SWEGCA: user@2026-09-22:72-79
void JournalStore::verify_extent(const PublishedSnapshot& current,
                                 const SegmentExtent& extent) const {
    Digest entering = zero_digest;
    if (extent.ordinal > 1) {
        const auto before = current.extents.find(extent.ordinal - 1);
        if (before == current.extents.end()) fail("journal_extent_chain_missing");
        entering = before->second.last_record_digest;
    }
    LedgerBytes bytes(static_cast<std::size_t>(extent.byte_length), memory_.allocator<std::byte>());
    io::read_range(segment_path(directory_, extent.ordinal), extent.byte_length, 0, bytes,
                   "journal_published_segment_missing");
    decode_segment_range(bytes, 0, extent, extent.first_sequence, extent.record_count, entering,
                         extent.last_record_digest, nullptr);
}

// Sizes the whole generation first, checks the disk use of its records and
// manifest against the storage reservation before encoding, then encodes into
// ledger buffers reserved to their exact size, builds the view pages and the
// complete next snapshot, and checks the full disk use again before
// returning, so publishing only writes and moves.
// SWEGCA: user@2026-09-22:72-79
StagedGeneration JournalStore::stage(std::span<const RecordDraft> drafts,
                                     const StateGeneration& state,
                                     std::span<const ViewGeneration> views) const {
    require_usable();
    // The snapshot is read before the retained charge: a rewrite adds the old
    // logs to the retained charge before it swaps the snapshot, so this order
    // never misses them (it may count them twice, which only overcharges).
    const auto current = snapshot();
    return stage_from(current, drafts, state, views, nullptr, retained_bytes_.load());
}

// `replacement`, when given, is a view already written to new page logs over
// the same entries; the generation then has no records and adds no pages.
// `retained` is the charge of page logs on disk that no view reaches.
// SWEGCA: user@2026-09-22:72-79
StagedGeneration JournalStore::stage_from(const std::shared_ptr<const PublishedSnapshot>& current,
                                          std::span<const RecordDraft> drafts,
                                          const StateGeneration& state,
                                          std::span<const ViewGeneration> views,
                                          const ViewPages* replacement,
                                          std::uint64_t retained) const {
    const auto& parent = current->head.fields();
    if (replacement != nullptr && !drafts.empty()) fail("journal_view_replacement_with_records");
    if (replacement == nullptr && current->view_unavailable) fail("journal_view_unavailable");
    if (parent.generation == std::numeric_limits<std::uint64_t>::max())
        fail("journal_generation_exhausted");

    // One planned piece per segment file this generation writes to. A piece
    // either extends the published tail (offset = its published length) or
    // starts a new file (offset 0, length starting with the header).
    struct PlannedPiece {
        std::uint64_t ordinal = 0;
        bool new_file = false;
        std::uint64_t offset = 0;
        std::uint64_t first_sequence = 0;
        std::uint64_t length = 0;
        std::uint64_t record_count = 0;
    };
    LedgerVector<std::uint64_t> sizes(memory_.allocator<std::uint64_t>());
    sizes.reserve(drafts.size());
    LedgerVector<PlannedPiece> plan(memory_.allocator<PlannedPiece>());
    plan.reserve(drafts.size() + 1);

    const SegmentExtent* tail =
        current->extents.empty() ? nullptr : &current->extents.rbegin()->second;
    std::uint64_t last_ordinal = tail ? tail->ordinal : 0;
    std::uint64_t new_files = 0;
    std::uint64_t record_bytes = 0;
    std::uint64_t sequence = parent.tail_sequence;
    for (const auto& draft : drafts) {
        if (sequence == std::numeric_limits<std::uint64_t>::max())
            fail("journal_sequence_exhausted");
        const std::uint64_t size = encoded_record_size(draft);
        if (size > max_segment_bytes - segment_header_bytes) fail("journal_record_too_large");
        if (size > max_generation_bytes - record_bytes) fail("journal_generation_too_large");
        record_bytes += size;
        if (plan.empty() || plan.back().offset + plan.back().length + size > max_segment_bytes) {
            if (plan.empty() && tail != nullptr && tail->byte_length + size <= max_segment_bytes) {
                plan.push_back({tail->ordinal, false, tail->byte_length, sequence + 1, 0, 0});
            } else {
                if (last_ordinal + 1 > max_extents) fail("journal_capacity_exhausted");
                ++last_ordinal;
                ++new_files;
                plan.push_back({last_ordinal, true, 0, sequence + 1, segment_header_bytes, 0});
            }
        }
        plan.back().length += size;
        plan.back().record_count += 1;
        ++sequence;
        sizes.push_back(size);
    }

    // The manifest this generation publishes, and where it goes.
    const std::string_view identity = identity_.value();
    const std::uint64_t partial_size = encoded_manifest_size(identity, plan.size(), views);
    // Recovery from the parent reads its own length plus what it chained
    // (at most max_recovery_bytes, checked when it was loaded or staged).
    const auto parent_recovery = parent.recovery_bytes_before + current->location.length;
    ManifestFields fields;
    fields.generation = parent.generation + 1;
    fields.checkpoint =
        fields.generation - parent.checkpoint_generation >= checkpoint_interval ||
        parent_recovery > max_recovery_bytes - std::min<std::uint64_t>(partial_size, max_recovery_bytes);
    const std::uint64_t extent_count =
        fields.checkpoint ? current->extents.size() + new_files : plan.size();
    const std::uint64_t manifest_size = encoded_manifest_size(identity, extent_count, views);
    const auto end = current->location.offset + current->location.length;
    const bool new_log = end + manifest_size > max_manifest_log_bytes;
    const ManifestLocation location =
        new_log ? ManifestLocation{current->location.log_ordinal + 1, manifest_log_header_bytes,
                                   manifest_size}
                : ManifestLocation{current->location.log_ordinal, end, manifest_size};

    // Disk use of the records and manifest after publication, checked before
    // anything is encoded; the view pages are added and the whole checked
    // again once they are built.
    std::uint64_t storage = plus(current->storage, retained, "journal_storage_overflow");
    if (replacement != nullptr) {
        // `retained` already holds the old view's logs, which `current->storage`
        // counts too; the generation's own view is the replacement.
        storage = plus(storage - page_log_charge(parent.view_pages),
                       page_log_charge(*replacement), "journal_storage_overflow");
    }
    for (const auto& piece : plan) {
        if (!piece.new_file) storage -= file_charge(tail->byte_length, allocation_unit_);
        storage = plus(storage, file_charge(piece.offset + piece.length, allocation_unit_),
                       "journal_storage_overflow");
    }
    storage = plus(storage, manifest_size, "journal_storage_overflow");
    if (new_log)
        storage = plus(storage, manifest_log_header_bytes + 2 * allocation_unit_,
                       "journal_storage_overflow");
    if (storage > storage_bytes_) fail("journal_storage_budget_exhausted");

    // Encode the records into exactly reserved ledger buffers.
    StagedGeneration staged(memory_);
    staged.parent_generation_ = parent.generation;
    staged.parent_manifest_digest_ = current->head.digest();
    staged.positions_.reserve(drafts.size());
    staged.pieces_.reserve(plan.size());
    LedgerVector<SegmentExtent> touched(memory_.allocator<SegmentExtent>());
    touched.reserve(plan.size());
    sequence = parent.tail_sequence;
    auto chain = parent.tail_record_digest;
    std::size_t next_draft = 0;
    for (const auto& planned : plan) {
        staged.pieces_.push_back(SegmentPiece{planned.ordinal, planned.new_file, planned.offset,
                                              LedgerBytes(memory_.allocator<std::byte>())});
        auto& piece = staged.pieces_.back();
        piece.bytes.reserve(static_cast<std::size_t>(planned.length));
        SegmentExtent extent = planned.new_file
                                   ? SegmentExtent{planned.ordinal, planned.first_sequence, 0,
                                                   segment_header_bytes, zero_digest}
                                   : *tail;
        if (planned.new_file)
            append_segment_header(piece.bytes, planned.ordinal, planned.first_sequence);
        for (std::uint64_t at = 0; at < planned.record_count; ++at, ++next_draft) {
            const auto offset = piece.offset + piece.bytes.size();
            Digest digest{};
            append_record(piece.bytes, drafts[next_draft], sequence + 1, chain, digest);
            ++sequence;
            chain = digest;
            extent.record_count += 1;
            extent.byte_length += sizes[next_draft];
            extent.last_record_digest = digest;
            staged.positions_.push_back(RecordPosition{piece.ordinal, offset, sequence, digest});
        }
        if (piece.bytes.size() != planned.length) fail("journal_piece_size_mismatch");
        touched.push_back(extent);
    }

    // The exact-address view for these records, written as detached pages.
    LedgerVector<AddressLeafItem> added(memory_.allocator<AddressLeafItem>());
    added.reserve(drafts.size());
    for (std::size_t at = 0; at < drafts.size(); ++at)
        added.push_back(AddressLeafItem{drafts[at].address, staged.positions_[at]});
    std::sort(added.begin(), added.end(), [](const AddressLeafItem& left, const AddressLeafItem& right) {
        return left.address < right.address;
    });
    for (std::size_t at = 1; at < added.size(); ++at)
        if (added[at - 1].address == added[at].address) fail("journal_address_duplicate");

    // The cue view: one entry per cue of every record, keyed by (cue,
    // separator, address) in one exactly reserved key buffer. Its size is
    // bounded like the generation's records, since a key repeats the address.
    std::size_t cue_count = 0;
    std::size_t cue_key_bytes = 0;
    for (const auto& draft : drafts) {
        for (const auto cue : draft.cues) {
            const auto size = cue_key_size(cue, draft.address);
            if (size > max_generation_bytes - cue_key_bytes) fail("journal_generation_too_large");
            cue_key_bytes += size;
            ++cue_count;
        }
    }
    LedgerBytes cue_keys(memory_.allocator<std::byte>());
    cue_keys.reserve(cue_key_bytes);
    LedgerVector<AddressLeafItem> cue_added(memory_.allocator<AddressLeafItem>());
    cue_added.reserve(cue_count);
    for (std::size_t at = 0; at < drafts.size(); ++at)
        for (const auto cue : drafts[at].cues)
            cue_added.push_back(AddressLeafItem{append_view_key(cue_keys, cue, drafts[at].address),
                                                staged.positions_[at]});
    std::sort(cue_added.begin(), cue_added.end(),
              [](const AddressLeafItem& left, const AddressLeafItem& right) {
                  return left.address < right.address;
              });
    for (std::size_t at = 1; at < cue_added.size(); ++at)
        if (cue_added[at - 1].address == cue_added[at].address) fail("journal_cue_duplicate");

    if (replacement != nullptr) {
        fields.view_pages = *replacement;
    } else {
        PageWriter writer{memory_, staged.page_pieces_, parent.view_pages.page_log_ordinal,
                          parent.view_pages.page_log_end};
        fields.view_pages = update_views(writer, directory_, parent.view_pages, added, cue_added);
    }
    for (auto& piece : staged.page_pieces_) {
        piece.parts.reserve(piece.pages.size() + 1);
        if (piece.new_file) piece.parts.push_back(piece.header);
        for (const auto& page : piece.pages) piece.parts.push_back(page);
    }

    fields.previous_manifest_digest = current->head.digest();
    fields.previous_location = current->location;
    fields.checkpoint_generation = fields.checkpoint ? fields.generation : parent.checkpoint_generation;
    fields.recovery_bytes_before = fields.checkpoint ? 0 : parent_recovery;
    fields.manifest_bytes_before = plus(parent.manifest_bytes_before, current->location.length,
                                        "journal_manifest_bytes_overflow");
    fields.state_generation_ordinal = state.ordinal();
    fields.state_generation_digest = state.digest().bytes();
    fields.tail_sequence = sequence;
    fields.tail_record_digest = chain;
    fields.tail_segment_ordinal =
        touched.empty() ? parent.tail_segment_ordinal : touched.back().ordinal;

    // A checkpoint lists every extent in ordinal order: the published ones,
    // with the tail replaced when this generation extended it, then the new.
    LedgerVector<SegmentExtent> all(memory_.allocator<SegmentExtent>());
    if (fields.checkpoint) {
        all.reserve(static_cast<std::size_t>(extent_count));
        std::size_t next_touched = 0;
        for (const auto& [ordinal, extent] : current->extents) {
            if (next_touched < touched.size() && touched[next_touched].ordinal == ordinal)
                all.push_back(touched[next_touched++]);
            else
                all.push_back(extent);
        }
        for (; next_touched < touched.size(); ++next_touched) all.push_back(touched[next_touched]);
    }
    auto manifest = Manifest::encode(fields, identity,
                                     fields.checkpoint ? std::span<const SegmentExtent>(all)
                                                       : std::span<const SegmentExtent>(touched),
                                     views, memory_);
    if (manifest.bytes().size() != manifest_size) fail("journal_manifest_size_mismatch");

    // The complete next snapshot, checked the way recovery checks it, and
    // the full disk use including the view pages.
    auto next = std::allocate_shared<PublishedSnapshot>(memory_.allocator<PublishedSnapshot>(),
                                                        memory_, std::move(manifest));
    next->extents = current->extents;
    apply_manifest(next->extents, next->head);
    next->location = location;
    next->page_logs = replacement != nullptr
                          ? std::allocate_shared<int>(memory_.allocator<int>(), 0)
                          : current->page_logs;
    next->storage = storage_of(next->extents, next->head.fields(), location);
    if (plus(next->storage, retained, "journal_storage_overflow") > storage_bytes_)
        fail("journal_storage_budget_exhausted");
    staged.head_bytes_.reserve(head_bytes);
    append_head(staged.head_bytes_, HeadPointer{location, next->head.digest()});
    if (new_log) {
        staged.log_header_.reserve(manifest_log_header_bytes);
        append_manifest_log_header(staged.log_header_, location.log_ordinal);
    }
    staged.next_ = std::move(next);
    staged.valid_ = true;
    return staged;
}

// Only writes and moves: the next snapshot and every byte to write were
// built and charged by `stage`. What is still allocated here is the
// file-name paths, whose size does not depend on the data.
// SWEGCA: user@2026-09-22:72-79
void JournalStore::publish(StagedGeneration&& staged) {
    std::lock_guard guard(publish_mutex_);
    reclaim_locked();
    publish_locked(std::move(staged));
}

// SWEGCA: user@2026-09-22:72-79
void JournalStore::publish_locked(StagedGeneration&& staged) {
    require_usable();
    if (!staged.valid_) fail("journal_staged_generation_invalid");
    StagedGeneration local(std::move(staged));  // one use: the source is now invalid
    const auto current = snapshot();
    if (local.parent_generation_ != current->head.fields().generation ||
        local.parent_manifest_digest_ != current->head.digest() ||
        !follows(current->location, local.next_->location))
        fail("journal_head_changed");
    // Checked again under the lock: a rewrite may have retired logs since
    // the generation was staged.
    if (plus(local.next_->storage, retained_bytes_.load(), "journal_storage_overflow") >
        storage_bytes_)
        fail("journal_storage_budget_exhausted");
    const auto& location = local.next_->location;
    const bool new_log = location.log_ordinal != current->location.log_ordinal;

    try {
        bool new_entries = new_log;
        for (const auto& piece : local.pieces_) {
            const auto path = segment_path(directory_, piece.ordinal);
            if (piece.new_file) {
                io::publish_file(path, piece.bytes);
                new_entries = true;
            } else {
                io::append_at_published_end(path, piece.offset, piece.bytes);
            }
        }
        for (const auto& piece : local.page_pieces_) {
            const auto path = page_log_path(directory_, piece.log_ordinal);
            if (piece.new_file) {
                io::publish_file_parts(path, piece.parts);
                new_entries = true;
            } else {
                io::append_parts_at_published_end(path, piece.offset, piece.parts);
            }
        }
        const auto log_path = manifest_log_path(directory_, location.log_ordinal);
        if (new_log) {
            io::publish_file(log_path, local.log_header_, local.next_->head.bytes());
        } else {
            io::append_at_published_end(log_path, location.offset, local.next_->head.bytes());
        }
        if (new_entries) io::make_entries_durable(directory_);
        io::publish_file(directory_ / head_name, local.head_bytes_);  // the commit point
    } catch (...) {
        // Unpublished bytes may now sit on disk uncharged; only a reopen,
        // which removes them, may continue.
        poisoned_.store(true);
        throw;
    }
    // Until the entry is durable the durable head is unknown.
    poisoned_.store(true);
    io::make_entries_durable(directory_);
    std::shared_ptr<const PublishedSnapshot> published = std::move(local.next_);
    snapshot_.store(published);  // noexcept
    poisoned_.store(false);
}

// SWEGCA: user@2026-09-22:72-79
bool JournalStore::view_available() const {
    require_usable();
    return !snapshot()->view_unavailable;
}

// SWEGCA: user@2026-09-22:72-79
std::optional<RecordPosition> JournalStore::resolve(const ExperienceAddress& address) const {
    require_usable();
    const auto current = snapshot();
    return resolve_in(*current, address.value());
}

// Lookup within one snapshot; the caller holds it, and with it the lease on
// the page logs its view reaches.
// SWEGCA: user@2026-09-22:72-79
std::optional<RecordPosition> JournalStore::resolve_in(const PublishedSnapshot& current,
                                                       std::string_view key) const {
    if (current.view_unavailable) fail("journal_view_unavailable");
    const auto& view = current.head.fields().view_pages.addresses;
    if (view.entry_count == 0) return std::nullopt;
    PageRef ref = view.root;
    for (std::uint32_t height = view.height;; --height) {
        const auto page = load_page(directory_, memory_, ref);
        if (height == 0 || page.view.leaf != (height == 1)) fail("journal_address_view_corrupt");
        if (page.view.leaf) {
            const auto& leaves = page.view.leaves;
            const auto found = std::lower_bound(
                leaves.begin(), leaves.end(), key,
                [](const AddressLeafItem& item, std::string_view wanted) { return item.address < wanted; });
            if (found == leaves.end() || found->address != key) return std::nullopt;
            return found->position;
        }
        const auto& children = page.view.children;
        const auto after = std::upper_bound(
            children.begin(), children.end(), key,
            [](std::string_view wanted, const AddressChildItem& child) {
                return wanted < child.first_address;
            });
        if (after == children.begin()) return std::nullopt;
        ref = std::prev(after)->page;
    }
}

// One snapshot for the whole replay: the view that resolves the address and
// the extents the record is read from belong to the same generation.
// SWEGCA: user@2026-09-22:72-79
PublishedRecord JournalStore::replay(const ExperienceAddress& address) const {
    require_usable();
    const auto current = snapshot();
    const auto position = resolve_in(*current, address.value());
    if (!position) fail("journal_address_unknown");
    auto record = read_in(*current, *position);
    if (record.view().address != address.value()) fail("journal_address_view_mismatch");
    return record;
}

// Reads the record at a position the view resolved; the record's digest must
// be the one the view names.
// SWEGCA: user@2026-09-22:72-79
PublishedRecord JournalStore::read_in(const PublishedSnapshot& current,
                                      const RecordPosition& position) const {
    const auto found = current.extents.find(position.segment_ordinal);
    if (found == current.extents.end()) fail("journal_position_unknown_segment");
    const auto& extent = found->second;
    if (position.sequence < extent.first_sequence ||
        position.sequence - extent.first_sequence >= extent.record_count ||
        position.byte_offset < segment_header_bytes ||
        position.byte_offset > extent.byte_length - minimum_record_bytes)
        fail("journal_position_invalid");
    const auto path = segment_path(directory_, extent.ordinal);
    std::array<std::byte, record_prefix_bytes> prefix{};
    io::read_range(path, extent.byte_length, position.byte_offset, prefix,
                   "journal_published_segment_missing");
    ByteReader prefix_reader(prefix);
    (void)prefix_reader.raw(4);
    (void)prefix_reader.u16();
    const std::uint64_t length = prefix_reader.u32();
    if (length < minimum_record_bytes || length > extent.byte_length - position.byte_offset)
        fail("journal_position_invalid");
    LedgerBytes bytes(static_cast<std::size_t>(length), memory_.allocator<std::byte>());
    io::read_range(path, extent.byte_length, position.byte_offset, bytes,
                   "journal_published_segment_missing");
    return PublishedRecord(std::move(bytes), position);
}

// SWEGCA: user@2026-09-22:72-79
void JournalStore::for_each_record(
    const std::function<void(const RecordView&, const RecordPosition&)>& visit) const {
    require_usable();
    const auto current = snapshot();
    Digest entering = zero_digest;
    for (const auto& [ordinal, extent] : current->extents) {
        LedgerBytes bytes(static_cast<std::size_t>(extent.byte_length),
                          memory_.allocator<std::byte>());
        io::read_range(segment_path(directory_, ordinal), extent.byte_length, 0, bytes,
                       "journal_published_segment_missing");
        const std::uint64_t segment = ordinal;
        const RecordVisitor forward = [&visit, segment](const RecordView& record,
                                                         std::uint64_t offset) {
            visit(record, RecordPosition{segment, offset, record.sequence, record.record_digest});
        };
        decode_segment_range(bytes, 0, extent, extent.first_sequence, extent.record_count,
                             entering, extent.last_record_digest, &forward);
        entering = extent.last_record_digest;
    }
}

// The keys of one cue are contiguous from (cue, separator); the cursor
// starts at the first of them and stops at the first key past them.
// SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:475-507
void JournalStore::for_each_cue_match(
    std::string_view cue,
    const std::function<bool(std::string_view, const RecordPosition&)>& visit) const {
    require_usable();
    if (!is_cue_text(cue) || cue.size() >= detail::identity_text_max_bytes)
        fail("journal_cue_invalid");
    const auto current = snapshot();
    if (current->view_unavailable) fail("journal_view_unavailable");
    const auto& tree = current->head.fields().view_pages.cues;
    if (tree.entry_count == 0) return;
    LedgerBytes bound(memory_.allocator<std::byte>());
    bound.reserve(cue.size() + 1);
    const auto* bytes = reinterpret_cast<const std::byte*>(cue.data());
    bound.insert(bound.end(), bytes, bytes + cue.size());
    bound.push_back(static_cast<std::byte>(cue_separator));
    const std::string_view from(reinterpret_cast<const char*>(bound.data()), bound.size());
    for (LeafCursor cursor(directory_, memory_, BuiltTree{tree.entry_count, tree.height, tree.root}, from);
         cursor.valid(); cursor.next()) {
        const auto& item = cursor.item();
        if (!item.address.starts_with(from)) return;
        if (!visit(item.address.substr(from.size()), item.position)) return;
    }
}

// The generation that publishes `view`, a rewrite of the current view over
// the same entries: no records, the parent's state generation and derived
// views, and the old view's logs still charged (they stay until reclaimed).
// SWEGCA: user@2026-09-22:72-79
StagedGeneration JournalStore::stage_view(const std::shared_ptr<const PublishedSnapshot>& current,
                                          const ViewPages& view) const {
    const auto& head = current->head;
    LedgerVector<ViewGeneration> views(memory_.allocator<ViewGeneration>());
    views.reserve(head.view_count());
    for (std::size_t at = 0; at < head.view_count(); ++at) views.push_back(head.view(at));
    const auto& fields = head.fields();
    const StateGeneration state(fields.state_generation_ordinal,
                                Digest256(fields.state_generation_digest));
    const auto retained = plus(retained_bytes_.load(), page_log_charge(fields.view_pages),
                               "journal_storage_overflow");
    return stage_from(current, {}, state, views, &view, retained);
}

// Publishes a view rewrite and retires the old view's logs under its lease.
// They are counted as retained before HEAD moves, so no concurrent stage
// can see the new head without their charge; if publication fails the store
// is poisoned and a reopen recounts.
// SWEGCA: user@2026-09-22:72-79
void JournalStore::publish_replacing_view(StagedGeneration&& staged,
                                          const std::shared_ptr<const PublishedSnapshot>& old) {
    const auto& view = old->head.fields().view_pages;
    if (view.page_log_ordinal == 0) {
        publish_locked(std::move(staged));
        return;
    }
    const auto charge = page_log_charge(view);
    // Capacity was reserved before the new logs were written; nothing here
    // allocates, and a failure before HEAD lets the caller remove them.
    if (retired_.size() == retired_.capacity()) fail("journal_retired_capacity_missing");
    retired_.push_back(RetiredLogs{view.first_page_log, view.page_log_ordinal, charge, old->page_logs});
    retained_bytes_ += charge;
    try {
        publish_locked(std::move(staged));
    } catch (...) {
        // Unless the store is poisoned, HEAD did not move and the old view is
        // still the published one: its logs must not stay retired.
        if (!poisoned_.load()) {
            retired_.pop_back();
            retained_bytes_ -= charge;
        }
        throw;
    }
}

// SWEGCA: user@2026-09-22:72-79
void JournalStore::reserve_retired() {
    if (retired_.size() == retired_.capacity())
        retired_.reserve(std::max<std::size_t>(4, retired_.capacity() * 2));
}

// SWEGCA: user@2026-09-22:72-79
void JournalStore::reclaim_retired() {
    std::lock_guard guard(publish_mutex_);
    reclaim_locked();
}

// Removes retired logs whose lease no snapshot holds. A log that cannot be
// removed now stays retired and charged, and is tried again next time.
// SWEGCA: user@2026-09-22:72-79
void JournalStore::reclaim_locked() noexcept {
    auto kept = retired_.begin();
    for (auto logs = retired_.begin(); logs != retired_.end(); ++logs) {
        if (logs->lease.expired() && remove_page_logs(*logs)) {
            retained_bytes_ -= logs->charge;
            continue;
        }
        if (kept != logs) *kept = std::move(*logs);
        ++kept;
    }
    retired_.erase(kept, retired_.end());
}

// SWEGCA: user@2026-09-22:72-79
bool JournalStore::remove_page_logs(const RetiredLogs& logs) const noexcept {
    try {
        for (auto ordinal = logs.first;; ++ordinal) {
            std::error_code error;
            fs::remove(page_log_path(directory_, ordinal), error);
            if (error) return false;
            if (ordinal == logs.last) return true;
        }
    } catch (...) {
        return false;
    }
}

// SWEGCA: user@2026-09-22:72-79
bool JournalStore::compaction_due() const {
    require_usable();
    const auto current = snapshot();
    const auto& view = current->head.fields().view_pages;
    const auto live = view.live_page_bytes();
    if (view.page_log_bytes <= live) return false;
    return view.page_log_bytes - live > live + max_page_log_bytes;
}

namespace {

// Writes `tree` again through `writer`, in key order.
// SWEGCA: user@2026-09-22:72-79
BuiltTree rewrite_tree(StreamWriter& writer, const fs::path& directory,
                       const MemoryLedger::Account& memory, const ViewTree& tree) {
    TreeBuilder builder(writer, memory);
    const BuiltTree previous{tree.entry_count, tree.height, tree.root};
    merge_into(builder, directory, memory, &previous, {});
    const auto built = builder.finish();
    if (built.count != tree.entry_count) fail("journal_address_view_corrupt");
    return built;
}

}  // namespace

// A failure before HEAD removes the new logs (and poisons the store only
// if they cannot be removed); a failure in publication itself has already
// poisoned it, and a reopen removes what was not published. Once this
// rewrite's own snapshot is released, the old logs are reclaimed at once
// when no reader holds them.
// SWEGCA: user@2026-09-22:72-79
void JournalStore::compact_view() {
    std::lock_guard guard(publish_mutex_);
    require_usable();
    reclaim_locked();
    auto current = snapshot();
    if (current->view_unavailable) fail("journal_view_unavailable");
    const auto view = current->head.fields().view_pages;
    if (view.addresses.entry_count == 0) return;  // no records, so no cues either
    reserve_retired();
    StreamWriter writer(directory_, memory_, next_page_log(view), allowance(*current),
                        allocation_unit_);
    try {
        const auto addresses = rewrite_tree(writer, directory_, memory_, view.addresses);
        const auto address_bytes = writer.page_bytes();
        const auto cues = rewrite_tree(writer, directory_, memory_, view.cues);
        writer.close();
        io::make_entries_durable(directory_);
        const auto pages = writer.pages(
            tree_of(addresses.count, addresses.height, addresses.root, address_bytes),
            tree_of(cues.count, cues.height, cues.root, writer.page_bytes() - address_bytes));
        publish_replacing_view(stage_view(current, pages), current);
    } catch (...) {
        if (!poisoned_.load() && !writer.remove_written()) poisoned_.store(true);
        throw;
    }
    current.reset();
    reclaim_locked();
}

// SWEGCA: user@2026-09-22:72-79
void JournalStore::rebuild_view() {
    std::lock_guard guard(publish_mutex_);
    require_usable();
    reclaim_locked();
    auto current = snapshot();
    const auto& fields = current->head.fields();
    reserve_retired();
    // Batches never exceed what the journal holds, so a small journal
    // reserves little: a record is at least `minimum_record_bytes`, and each
    // of its cues at least five bytes of it.
    std::uint64_t segment_bytes = 0;
    for (const auto& [ordinal, extent] : current->extents)
        segment_bytes = plus(segment_bytes, extent.byte_length, "journal_storage_overflow");
    // One collector per tree: its key buffer and batch (both never grow) and
    // the sorted runs written so far.
    struct Collector {
        LedgerBytes keys;
        LedgerVector<AddressLeafItem> batch;
        LedgerVector<Run> runs;
    };
    const auto key_room = static_cast<std::size_t>(
        std::min<std::uint64_t>(rebuild_batch_key_bytes, segment_bytes));
    const auto collector = [&](std::uint64_t most_items) {
        Collector out{LedgerBytes(memory_.allocator<std::byte>()),
                      LedgerVector<AddressLeafItem>(memory_.allocator<AddressLeafItem>()),
                      LedgerVector<Run>(memory_.allocator<Run>())};
        out.keys.reserve(key_room);
        out.batch.reserve(static_cast<std::size_t>(std::min<std::uint64_t>(rebuild_batch_items, most_items)));
        return out;
    };
    Collector addresses = collector(fields.tail_sequence);
    Collector cues = collector(segment_bytes / 5);
    std::optional<StreamWriter> merged;
    const auto room = allowance(*current);
    std::uint64_t held = 0;  // charge of the logs written so far
    auto next_log = next_page_log(fields.view_pages);
    const auto remove_all = [&]() noexcept {
        bool removed = true;
        for (const auto* each : {&addresses, &cues})
            for (const auto& run : each->runs) removed = run.writer.remove_written() && removed;
        if (merged) removed = merged->remove_written() && removed;
        return removed;
    };
    try {
        const auto write_run = [&](Collector& into) {
            if (into.runs.size() == into.runs.capacity())
                into.runs.reserve(std::max<std::size_t>(4, into.runs.capacity() * 2));
            std::sort(into.batch.begin(), into.batch.end(),
                      [](const AddressLeafItem& left, const AddressLeafItem& right) {
                          return left.address < right.address;
                      });
            StreamWriter writer(directory_, memory_, next_log, room - std::min(room, held),
                                allocation_unit_);
            BuiltTree tree;
            try {
                TreeBuilder builder(writer, memory_);
                for (const auto& item : into.batch) builder.add(item);
                tree = builder.finish();
                writer.close();
            } catch (...) {
                if (!writer.remove_written()) poisoned_.store(true);
                throw;
            }
            next_log = writer.next_ordinal();
            held += writer.charged();
            into.runs.push_back(Run{tree, std::move(writer)});  // capacity reserved above
            into.keys.clear();
            into.batch.clear();
        };
        // Adds one entry (an address key when `cue` is empty, else a cue
        // key), its key copied into the collector's buffer.
        const auto collect = [&](Collector& into, std::string_view cue, std::string_view address,
                                 const RecordPosition& position) {
            const auto size = cue.empty() ? address.size() : cue_key_size(cue, address);
            const auto fits = [&] {
                return into.batch.size() < into.batch.capacity() &&
                       size <= into.keys.capacity() - into.keys.size();
            };
            if (!fits()) write_run(into);
            if (!fits()) fail("journal_rebuild_batch_invalid");  // more than the journal holds
            into.batch.push_back(AddressLeafItem{append_view_key(into.keys, cue, address), position});
        };
        Digest entering = zero_digest;
        for (const auto& [ordinal, extent] : current->extents) {
            LedgerBytes bytes(static_cast<std::size_t>(extent.byte_length),
                              memory_.allocator<std::byte>());
            io::read_range(segment_path(directory_, ordinal), extent.byte_length, 0, bytes,
                           "journal_published_segment_missing");
            const std::uint64_t segment = ordinal;
            const RecordVisitor visit = [&](const RecordView& record, std::uint64_t offset) {
                const RecordPosition position{segment, offset, record.sequence, record.record_digest};
                collect(addresses, {}, record.address, position);
                for_each_cue(record, [&](std::string_view cue) {
                    collect(cues, cue, record.address, position);
                });
            };
            decode_segment_range(bytes, 0, extent, extent.first_sequence, extent.record_count,
                                 entering, extent.last_record_digest, &visit);
            entering = extent.last_record_digest;
        }
        if (!addresses.batch.empty()) write_run(addresses);
        if (!cues.batch.empty()) write_run(cues);

        // Both trees are merged into one set of new logs, so the published
        // view names one contiguous log range; no snapshot ever named a run.
        merged.emplace(directory_, memory_, next_log, room - std::min(room, held),
                       allocation_unit_);
        const auto merge = [&](const Collector& from) {
            TreeBuilder builder(*merged, memory_);
            merge_runs(builder, directory_, memory_, from.runs);
            return builder.finish();
        };
        const auto built_addresses = merge(addresses);
        const auto address_bytes = merged->page_bytes();
        const auto built_cues = merge(cues);
        merged->close();
        for (const auto* each : {&addresses, &cues})
            for (const auto& run : each->runs)
                if (!run.writer.remove_written()) fail("journal_page_log_remove_failed");
        if (built_addresses.count != fields.tail_sequence) fail("journal_address_view_count_mismatch");
        io::make_entries_durable(directory_);
        const auto pages = merged->pages(
            tree_of(built_addresses.count, built_addresses.height, built_addresses.root, address_bytes),
            tree_of(built_cues.count, built_cues.height, built_cues.root,
                    merged->page_bytes() - address_bytes));
        publish_replacing_view(stage_view(current, pages), current);
    } catch (...) {
        if (!poisoned_.load() && !remove_all()) poisoned_.store(true);
        throw;
    }
    current.reset();
    reclaim_locked();
}

}  // namespace swegca::architecture::journal
