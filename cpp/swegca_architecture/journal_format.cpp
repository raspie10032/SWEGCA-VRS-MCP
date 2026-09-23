#include "swegca_architecture/journal_format.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace swegca::architecture::journal {
namespace {

constexpr std::array<std::byte, 4> record_magic{
    std::byte{'S'}, std::byte{'W'}, std::byte{'J'}, std::byte{'R'}};
constexpr std::array<std::byte, 4> segment_magic{
    std::byte{'S'}, std::byte{'W'}, std::byte{'J'}, std::byte{'S'}};
constexpr std::array<std::byte, 4> manifest_magic{
    std::byte{'S'}, std::byte{'W'}, std::byte{'J'}, std::byte{'M'}};
constexpr std::array<std::byte, 4> manifest_log_magic{
    std::byte{'S'}, std::byte{'W'}, std::byte{'J'}, std::byte{'L'}};
constexpr std::array<std::byte, 4> head_magic{
    std::byte{'S'}, std::byte{'W'}, std::byte{'J'}, std::byte{'H'}};
constexpr std::array<std::byte, 4> address_page_magic{
    std::byte{'S'}, std::byte{'W'}, std::byte{'J'}, std::byte{'A'}};
constexpr std::array<std::byte, 4> page_log_magic{
    std::byte{'S'}, std::byte{'W'}, std::byte{'J'}, std::byte{'P'}};
constexpr std::uint16_t format_version = 6;
constexpr std::uint8_t leaf_page_kind = 1;
constexpr std::uint8_t branch_page_kind = 2;
// Fixed part of a manifest: magic, version, identity length, generation,
// previous digest and location, checkpoint flag, checkpoint generation,
// recovery and manifest byte counters, state generation, tail, view pages,
// extent count, view count, digest.
constexpr std::size_t manifest_fixed_bytes = 4 + 2 + 4 + 8 + 32 + 3 * 8 + 1 + 3 * 8 + 8 + 32 +
                                             8 + 32 + 8 + encoded_view_pages_bytes + 4 + 4 + 32;
// Smallest encoded view: empty name is invalid, so a one-byte name.
constexpr std::size_t minimum_view_bytes = 4 + 1 + 8 + 32;

// SWEGCA: user@2026-09-22:60-61
[[noreturn]] void fail(const char* code) { throw std::runtime_error(code); }

// SWEGCA: user@2026-09-22:60-61
void require_magic(ByteReader& reader, const std::array<std::byte, 4>& magic,
                   const char* code) {
    const auto found = reader.raw(magic.size());
    if (std::memcmp(found.data(), magic.data(), magic.size()) != 0) fail(code);
}

// Checked addition; overflow is a malformed input, never a wrap.
// SWEGCA: user@2026-09-22:60-61
std::uint64_t add(std::uint64_t left, std::uint64_t right, const char* code) {
    if (right > std::numeric_limits<std::uint64_t>::max() - left) fail(code);
    return left + right;
}

// A required identity text viewed in place.
// SWEGCA: user@2026-09-22:60-61
std::string_view identity_text(ByteReader& reader, const char* code) {
    const auto text = reader.text_view(detail::identity_text_max_bytes);
    if (!detail::is_identity_text(text)) fail(code);
    return text;
}

// An optional identity text: empty on disk means absent.
// SWEGCA: user@2026-09-22:60-61
std::string_view optional_identity_text(ByteReader& reader, const char* code) {
    const auto text = reader.text_view(detail::identity_text_max_bytes);
    if (!text.empty() && !detail::is_identity_text(text)) fail(code);
    return text;
}

// A draft text, required to satisfy the identity rule for `field`.
// SWEGCA: user@2026-09-22:60-61
std::string_view checked_text(std::string_view text, std::string_view field) {
    detail::require_identity_text(text, field);
    return text;
}

// An optional draft text: empty when absent, checked when present.
// SWEGCA: user@2026-09-22:60-61
std::string_view checked_text(const std::optional<std::string_view>& text, std::string_view field) {
    return text ? checked_text(*text, field) : std::string_view();
}

// SWEGCA: user@2026-09-22:60-61
std::string_view optional_text(const std::optional<std::string_view>& value) noexcept {
    return value.value_or(std::string_view());
}

// SWEGCA: user@2026-09-22:72-79
void encode_location(ByteWriter& writer, const ManifestLocation& location) {
    writer.u64(location.log_ordinal);
    writer.u64(location.offset);
    writer.u64(location.length);
}

// SWEGCA: user@2026-09-22:72-79
ManifestLocation decode_location(ByteReader& reader) {
    ManifestLocation location;
    location.log_ordinal = reader.u64();
    location.offset = reader.u64();
    location.length = reader.u64();
    return location;
}

// SWEGCA: user@2026-09-22:72-79
void encode_page_ref(ByteWriter& writer, const PageRef& page) {
    writer.u64(page.log_ordinal);
    writer.u64(page.offset);
    writer.u32(page.length);
    writer.digest(page.digest);
}

// SWEGCA: user@2026-09-22:72-79
PageRef read_page_ref(ByteReader& reader) {
    PageRef page;
    page.log_ordinal = reader.u64();
    page.offset = reader.u64();
    page.length = reader.u32();
    page.digest = reader.digest();
    return page;
}

// A reference to a page that could exist: in a page log, past its header,
// and no longer than a page.
// SWEGCA: user@2026-09-22:72-79
bool page_ref_valid(const PageRef& page) noexcept {
    return page.log_ordinal != 0 && page.offset >= page_log_header_bytes &&
           page.length > address_page_header_bytes && page.length <= address_page_max_bytes &&
           page.offset <= max_page_log_bytes - page.length;
}

// Nonempty, strictly increasing identity texts; `key` gives each item's text.
// SWEGCA: user@2026-09-22:72-79
template <class Item, class Key>
void require_increasing(std::span<const Item> items, Key key, const char* code) {
    if (items.empty()) fail(code);
    for (std::size_t at = 0; at < items.size(); ++at) {
        const std::string_view current = key(items[at]);
        if (!detail::is_identity_text(current)) fail(code);
        if (at != 0 && !(key(items[at - 1]) < current)) fail(code);
    }
}

// SWEGCA: user@2026-09-22:72-79
void write_page_header(ByteWriter& writer, std::uint8_t kind, std::size_t count) {
    writer.raw(address_page_magic);
    writer.u16(format_version);
    writer.u8(kind);
    writer.u8(0);  // reserved
    writer.u32(static_cast<std::uint32_t>(count));
}

// SWEGCA: user@2026-09-22:72-79
void encode_view_tree(ByteWriter& writer, const ViewTree& tree) {
    writer.u64(tree.entry_count);
    writer.u32(tree.height);
    encode_page_ref(writer, tree.root);
    writer.u64(tree.live_page_bytes);
}

// SWEGCA: user@2026-09-22:72-79
ViewTree read_view_tree(ByteReader& reader) {
    ViewTree tree;
    tree.entry_count = reader.u64();
    tree.height = reader.u32();
    tree.root = read_page_ref(reader);
    tree.live_page_bytes = reader.u64();
    return tree;
}

// SWEGCA: user@2026-09-22:72-79
void encode_view_pages(ByteWriter& writer, const ViewPages& pages) {
    encode_view_tree(writer, pages.addresses);
    encode_view_tree(writer, pages.cues);
    writer.u64(pages.first_page_log);
    writer.u64(pages.page_log_ordinal);
    writer.u64(pages.page_log_end);
    writer.u64(pages.page_log_bytes);
}

// An empty tree has no root; any other tree's root sits in the page logs the
// view pages name, and its live pages include at least the root.
// SWEGCA: user@2026-09-22:72-79
bool view_tree_valid(const ViewTree& tree, const ViewPages& pages) noexcept {
    if (tree.entry_count == 0)
        return tree.height == 0 && tree.root == PageRef{} && tree.live_page_bytes == 0;
    return tree.height != 0 && tree.height <= max_address_height && page_ref_valid(tree.root) &&
           tree.live_page_bytes >= tree.root.length &&
           tree.root.log_ordinal >= pages.first_page_log &&
           tree.root.log_ordinal <= pages.page_log_ordinal &&
           (tree.root.log_ordinal != pages.page_log_ordinal ||
            tree.root.offset + tree.root.length <= pages.page_log_end);
}

// The address tree holds one entry per record, both roots sit in the page
// logs named, and there are logs exactly when a tree is nonempty.
// SWEGCA: user@2026-09-22:72-79
ViewPages decode_view_pages(ByteReader& reader, std::uint64_t tail_sequence) {
    ViewPages pages;
    pages.addresses = read_view_tree(reader);
    pages.cues = read_view_tree(reader);
    pages.first_page_log = reader.u64();
    pages.page_log_ordinal = reader.u64();
    pages.page_log_end = reader.u64();
    pages.page_log_bytes = reader.u64();
    if (pages.addresses.entry_count != tail_sequence) fail("journal_manifest_invalid:address_count");
    // A cue entry names a record, so there are none without records.
    if (pages.addresses.entry_count == 0 && pages.cues.entry_count != 0)
        fail("journal_manifest_invalid:cue_count");
    if (!view_tree_valid(pages.addresses, pages)) fail("journal_manifest_invalid:address_root");
    if (!view_tree_valid(pages.cues, pages)) fail("journal_manifest_invalid:cue_root");
    const bool empty = pages.addresses.entry_count == 0 && pages.cues.entry_count == 0;
    if (pages.page_log_ordinal == 0
            ? (pages.first_page_log != 0 || pages.page_log_end != 0 ||
               pages.page_log_bytes != 0 || !empty)
            : (pages.first_page_log == 0 || pages.first_page_log > pages.page_log_ordinal ||
               pages.page_log_end < page_log_header_bytes ||
               pages.page_log_end > max_page_log_bytes || pages.page_log_bytes < pages.page_log_end ||
               pages.addresses.live_page_bytes > pages.page_log_bytes ||
               pages.cues.live_page_bytes > pages.page_log_bytes - pages.addresses.live_page_bytes))
        fail("journal_manifest_invalid:page_logs");
    return pages;
}

}  // namespace

// SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:398-437
bool is_cue_text(std::string_view cue) noexcept {
    return detail::is_identity_text(cue) && cue.find(cue_separator) == std::string_view::npos;
}

// SWEGCA: user@2026-09-22:60-61
void ByteWriter::u8(std::uint8_t value) { out_->push_back(std::byte{value}); }

// SWEGCA: user@2026-09-22:60-61
void ByteWriter::u16(std::uint16_t value) {
    for (int shift = 0; shift < 16; shift += 8)
        out_->push_back(static_cast<std::byte>((value >> shift) & 0xff));
}

// SWEGCA: user@2026-09-22:60-61
void ByteWriter::u32(std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8)
        out_->push_back(static_cast<std::byte>((value >> shift) & 0xff));
}

// SWEGCA: user@2026-09-22:60-61
void ByteWriter::u64(std::uint64_t value) {
    for (int shift = 0; shift < 64; shift += 8)
        out_->push_back(static_cast<std::byte>((value >> shift) & 0xff));
}

// SWEGCA: user@2026-09-22:60-61
void ByteWriter::text(std::string_view value, std::size_t limit) {
    if (value.size() > limit) fail("journal_text_too_long");
    u32(static_cast<std::uint32_t>(value.size()));
    raw(std::as_bytes(std::span<const char>(value.data(), value.size())));
}

// SWEGCA: user@2026-09-22:60-61
void ByteWriter::bytes(std::span<const std::byte> value, std::size_t limit) {
    if (value.size() > limit) fail("journal_bytes_too_long");
    u32(static_cast<std::uint32_t>(value.size()));
    raw(value);
}

// SWEGCA: user@2026-09-22:60-61
void ByteWriter::digest(const Digest& value) { raw(value); }

// SWEGCA: user@2026-09-22:60-61
void ByteWriter::raw(std::span<const std::byte> value) {
    out_->insert(out_->end(), value.begin(), value.end());
}

// SWEGCA: user@2026-09-22:60-61
std::span<const std::byte> ByteReader::raw(std::size_t count) {
    if (count > remaining()) fail("journal_truncated");
    const auto out = data_.subspan(offset_, count);
    offset_ += count;
    return out;
}

// SWEGCA: user@2026-09-22:60-61
std::uint8_t ByteReader::u8() { return std::to_integer<std::uint8_t>(raw(1)[0]); }

// SWEGCA: user@2026-09-22:60-61
std::uint16_t ByteReader::u16() {
    const auto bytes = raw(2);
    return static_cast<std::uint16_t>(std::to_integer<unsigned>(bytes[0]) |
                                      (std::to_integer<unsigned>(bytes[1]) << 8));
}

// SWEGCA: user@2026-09-22:60-61
std::uint32_t ByteReader::u32() {
    const auto bytes = raw(4);
    std::uint32_t value = 0;
    for (int at = 3; at >= 0; --at)
        value = (value << 8) | std::to_integer<std::uint32_t>(bytes[at]);
    return value;
}

// SWEGCA: user@2026-09-22:60-61
std::uint64_t ByteReader::u64() {
    const auto bytes = raw(8);
    std::uint64_t value = 0;
    for (int at = 7; at >= 0; --at)
        value = (value << 8) | std::to_integer<std::uint64_t>(bytes[at]);
    return value;
}

// SWEGCA: user@2026-09-22:60-61
std::string_view ByteReader::text_view(std::size_t limit) {
    const auto size = u32();
    if (size > limit) fail("journal_text_too_long");
    const auto bytes = raw(size);
    const std::string_view out(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    if (!detail::is_strict_utf8(out)) fail("journal_text_not_utf8");
    return out;
}

// SWEGCA: user@2026-09-22:60-61
std::span<const std::byte> ByteReader::bytes_view(std::size_t limit) {
    const auto size = u32();
    if (size > limit) fail("journal_bytes_too_long");
    return raw(size);
}

// SWEGCA: user@2026-09-22:60-61
Digest ByteReader::digest() {
    const auto bytes = raw(32);
    Digest out{};
    std::memcpy(out.data(), bytes.data(), out.size());
    return out;
}

// SWEGCA: user@2026-09-22:60-61
std::span<const std::byte> ByteReader::consumed_since(std::size_t start) const {
    if (start > offset_) fail("journal_reader_offset_invalid");
    return data_.subspan(start, offset_ - start);
}

// SWEGCA: user@2026-09-22:60-61
std::size_t encoded_record_size(const RecordDraft& draft) {
    if (draft.kind == 0) fail("journal_record_invalid:kind");
    if (draft.payload.size() > max_payload_bytes) fail("journal_record_invalid:payload");
    constexpr auto limit = detail::identity_text_max_bytes;
    std::size_t total = minimum_record_bytes + draft.payload.size();
    for (const auto text :
         {checked_text(draft.address, ExperienceAddressTag::name),
          checked_text(draft.source, ProducerIdTag::name),
          checked_text(draft.source_revision, SourceRevisionTag::name),
          checked_text(draft.previous_revision_address, ExperienceAddressTag::name),
          checked_text(draft.claim, ClaimIdTag::name), checked_text(draft.outcome, OutcomeTextTag::name),
          checked_text(draft.operation_id, OperationIdTag::name),
          checked_text(draft.transaction_id, TransactionIdTag::name)}) {
        if (text.size() > limit) fail("journal_text_too_long");
        total += text.size();
    }
    if (draft.cues.size() > max_record_cues) fail("journal_record_invalid:cues");
    for (std::size_t at = 0; at < draft.cues.size(); ++at) {
        const auto cue = draft.cues[at];
        if (!is_cue_text(cue) || (at != 0 && !(draft.cues[at - 1] < cue)) ||
            cue_key_size(cue, draft.address) > limit)
            fail("journal_record_invalid:cue");
        total += 4 + cue.size();
    }
    if (total > std::numeric_limits<std::uint32_t>::max()) fail("journal_record_invalid:length");
    return total;
}

// The record is written in place behind whatever `out` already holds; its
// length field is known up front, so nothing is patched afterwards.
// SWEGCA: user@2026-09-22:60-61
void append_record(LedgerBytes& out, const RecordDraft& draft, std::uint64_t sequence,
                   const Digest& previous_record_digest, Digest& record_digest) {
    if (sequence == 0) fail("journal_record_invalid:sequence");
    const auto total = encoded_record_size(draft);
    constexpr auto limit = detail::identity_text_max_bytes;
    const auto start = out.size();
    ByteWriter writer(out);
    writer.raw(record_magic);
    writer.u16(format_version);
    writer.u32(static_cast<std::uint32_t>(total));
    writer.u16(draft.kind);
    writer.u8(draft.authority ? static_cast<std::uint8_t>(*draft.authority) : 0);
    writer.u8(0);  // reserved
    writer.u64(sequence);
    writer.text(draft.address, limit);
    writer.text(draft.source, limit);
    writer.text(draft.source_revision, limit);
    writer.text(optional_text(draft.previous_revision_address), limit);
    writer.text(optional_text(draft.claim), limit);
    writer.text(optional_text(draft.outcome), limit);
    writer.text(draft.operation_id, limit);
    writer.text(optional_text(draft.transaction_id), limit);
    writer.u32(static_cast<std::uint32_t>(draft.cues.size()));
    for (const auto cue : draft.cues) writer.text(cue, limit);
    writer.bytes(draft.payload, max_payload_bytes);
    writer.digest(Sha256::of(draft.payload));
    writer.digest(previous_record_digest);
    record_digest = Sha256::of(std::span<const std::byte>(out).subspan(start));
    writer.digest(record_digest);
    if (out.size() - start != total) fail("journal_record_size_mismatch");
}

// SWEGCA: user@2026-09-22:60-61
RecordView decode_record(ByteReader& reader) {
    const auto start = reader.offset();
    require_magic(reader, record_magic, "journal_record_magic_invalid");
    if (reader.u16() != format_version) fail("journal_record_version_invalid");
    const auto declared = reader.u32();
    if (declared < minimum_record_bytes || declared - 10 > reader.remaining())
        fail("journal_record_length_invalid");
    RecordView out;
    out.kind = reader.u16();
    if (out.kind == 0) fail("journal_record_invalid:kind");
    const auto authority_code = reader.u8();
    if (authority_code > static_cast<std::uint8_t>(AuthorityDomain::p3_promotion))
        fail("journal_record_invalid:authority");
    if (authority_code != 0) out.authority = static_cast<AuthorityDomain>(authority_code);
    if (reader.u8() != 0) fail("journal_record_reserved_invalid");
    out.sequence = reader.u64();
    if (out.sequence == 0) fail("journal_record_invalid:sequence");
    out.address = identity_text(reader, "journal_record_invalid:address");
    out.source = identity_text(reader, "journal_record_invalid:source");
    out.source_revision = identity_text(reader, "journal_record_invalid:source_revision");
    out.previous_revision_address =
        optional_identity_text(reader, "journal_record_invalid:previous_revision_address");
    out.claim = optional_identity_text(reader, "journal_record_invalid:claim");
    out.outcome = optional_identity_text(reader, "journal_record_invalid:outcome");
    out.operation_id = identity_text(reader, "journal_record_invalid:operation_id");
    out.transaction_id = optional_identity_text(reader, "journal_record_invalid:transaction_id");
    out.cue_count = reader.u32();
    if (out.cue_count > max_record_cues) fail("journal_record_invalid:cues");
    const auto cues_start = reader.offset();
    std::string_view previous_cue;
    for (std::uint32_t at = 0; at < out.cue_count; ++at) {
        const auto cue = reader.text_view(detail::identity_text_max_bytes);
        if (!is_cue_text(cue) || (at != 0 && !(previous_cue < cue)) ||
            cue_key_size(cue, out.address) > detail::identity_text_max_bytes)
            fail("journal_record_invalid:cue");
        previous_cue = cue;
    }
    out.cues = reader.consumed_since(cues_start);
    out.payload = reader.bytes_view(max_payload_bytes);
    out.payload_digest = reader.digest();
    if (Sha256::of(out.payload) != out.payload_digest)
        fail("journal_record_payload_digest_invalid");
    out.previous_record_digest = reader.digest();
    const auto body_bytes = reader.consumed_since(start);
    out.record_digest = reader.digest();
    if (reader.offset() - start != declared) fail("journal_record_length_invalid");
    if (Sha256::of(body_bytes) != out.record_digest) fail("journal_record_digest_invalid");
    return out;
}

// SWEGCA: user@2026-09-22:72-79
void append_segment_header(LedgerBytes& out, std::uint64_t ordinal, std::uint64_t first_sequence) {
    ByteWriter writer(out);
    writer.raw(segment_magic);
    writer.u16(format_version);
    writer.u64(ordinal);
    writer.u64(first_sequence);
}

// SWEGCA: user@2026-09-22:72-79
bool follows(const ManifestLocation& previous, const ManifestLocation& next) noexcept {
    if (previous.log_ordinal == 0 || next.length == 0) return false;
    if (next.log_ordinal == previous.log_ordinal)
        return previous.offset <= std::numeric_limits<std::uint64_t>::max() - previous.length &&
               next.offset == previous.offset + previous.length;
    return previous.log_ordinal != std::numeric_limits<std::uint64_t>::max() &&
           next.log_ordinal == previous.log_ordinal + 1 &&
           next.offset == manifest_log_header_bytes;
}

// SWEGCA: user@2026-09-22:72-79
void decode_segment_range(std::span<const std::byte> bytes, std::uint64_t base_offset,
                          const SegmentExtent& extent, std::uint64_t first_sequence,
                          std::uint64_t record_count, const Digest& entering,
                          const Digest& expected_last, const RecordVisitor* visit) {
    // Every record needs at least minimum_record_bytes, so a count the bytes
    // cannot hold is rejected before any work is sized from it.
    if (record_count > bytes.size() / minimum_record_bytes)
        fail("journal_segment_count_invalid");
    (void)add(first_sequence, record_count, "journal_segment_sequence_overflow");
    ByteReader reader(bytes);
    if (base_offset == 0) {
        require_magic(reader, segment_magic, "journal_segment_magic_invalid");
        if (reader.u16() != format_version) fail("journal_segment_version_invalid");
        if (reader.u64() != extent.ordinal || reader.u64() != extent.first_sequence)
            fail("journal_segment_header_invalid");
    } else if (base_offset < segment_header_bytes) {
        fail("journal_segment_offset_invalid");
    }
    auto chain = entering;
    for (std::uint64_t at = 0; at < record_count; ++at) {
        const auto offset = add(base_offset, reader.offset(), "journal_segment_offset_overflow");
        const auto record = decode_record(reader);
        if (record.sequence != first_sequence + at) fail("journal_segment_sequence_invalid");
        if (record.previous_record_digest != chain) fail("journal_segment_chain_invalid");
        chain = record.record_digest;
        if (visit != nullptr && *visit) (*visit)(record, offset);
    }
    if (reader.remaining() != 0) fail("journal_segment_trailing_bytes");
    if (chain != expected_last) fail("journal_segment_tail_invalid");
}

// SWEGCA: user@2026-09-22:72-79
void append_leaf_page(LedgerBytes& out, std::span<const AddressLeafItem> items) {
    require_increasing(items, [](const AddressLeafItem& item) { return item.address; },
                       "journal_address_page_invalid");
    std::size_t total = address_page_header_bytes;
    for (const auto& item : items) total += encoded_leaf_item_size(item.address);
    if (total > address_page_max_bytes) fail("journal_address_page_too_large");
    const auto start = out.size();
    ByteWriter writer(out);
    write_page_header(writer, leaf_page_kind, items.size());
    for (const auto& item : items) {
        writer.text(item.address, detail::identity_text_max_bytes);
        writer.u64(item.position.segment_ordinal);
        writer.u64(item.position.byte_offset);
        writer.u64(item.position.sequence);
        writer.digest(item.position.record_digest);
    }
    if (out.size() - start != total) fail("journal_address_page_size_mismatch");
}

// SWEGCA: user@2026-09-22:72-79
void append_branch_page(LedgerBytes& out, std::span<const AddressChildItem> items) {
    require_increasing(items, [](const AddressChildItem& item) { return item.first_address; },
                       "journal_address_page_invalid");
    std::size_t total = address_page_header_bytes;
    for (const auto& item : items) {
        if (!page_ref_valid(item.page)) fail("journal_address_page_invalid");
        total += encoded_child_item_size(item.first_address);
    }
    if (total > address_page_max_bytes) fail("journal_address_page_too_large");
    const auto start = out.size();
    ByteWriter writer(out);
    write_page_header(writer, branch_page_kind, items.size());
    for (const auto& item : items) {
        writer.text(item.first_address, detail::identity_text_max_bytes);
        encode_page_ref(writer, item.page);
    }
    if (out.size() - start != total) fail("journal_address_page_size_mismatch");
}

// SWEGCA: user@2026-09-22:72-79
AddressPageView decode_address_page(std::span<const std::byte> bytes,
                                    const MemoryLedger::Account& memory) {
    if (bytes.size() <= address_page_header_bytes || bytes.size() > address_page_max_bytes)
        fail("journal_address_page_invalid");
    ByteReader reader(bytes);
    require_magic(reader, address_page_magic, "journal_address_page_magic_invalid");
    if (reader.u16() != format_version) fail("journal_address_page_version_invalid");
    const auto kind = reader.u8();
    if (reader.u8() != 0) fail("journal_address_page_reserved_invalid");
    const auto count = reader.u32();
    // The smallest item is a one-byte key with a page reference.
    if (count == 0 || count > reader.remaining() / encoded_child_item_size("x"))
        fail("journal_address_page_invalid");
    AddressPageView out(memory);
    if (kind == leaf_page_kind) {
        out.leaf = true;
        out.leaves.reserve(count);
        for (std::uint32_t at = 0; at < count; ++at) {
            AddressLeafItem item;
            item.address = reader.text_view(detail::identity_text_max_bytes);
            item.position.segment_ordinal = reader.u64();
            item.position.byte_offset = reader.u64();
            item.position.sequence = reader.u64();
            item.position.record_digest = reader.digest();
            if (item.position.segment_ordinal == 0 || item.position.sequence == 0 ||
                item.position.byte_offset < segment_header_bytes ||
                item.position.byte_offset >= max_segment_bytes)
                fail("journal_address_page_invalid");
            out.leaves.push_back(item);
        }
        require_increasing(std::span<const AddressLeafItem>(out.leaves),
                           [](const AddressLeafItem& item) { return item.address; },
                           "journal_address_page_invalid");
    } else if (kind == branch_page_kind) {
        out.leaf = false;
        out.children.reserve(count);
        for (std::uint32_t at = 0; at < count; ++at) {
            AddressChildItem item;
            item.first_address = reader.text_view(detail::identity_text_max_bytes);
            item.page = read_page_ref(reader);
            if (!page_ref_valid(item.page)) fail("journal_address_page_invalid");
            out.children.push_back(item);
        }
        require_increasing(std::span<const AddressChildItem>(out.children),
                           [](const AddressChildItem& item) { return item.first_address; },
                           "journal_address_page_invalid");
    } else {
        fail("journal_address_page_kind_invalid");
    }
    if (reader.remaining() != 0) fail("journal_address_page_trailing_bytes");
    return out;
}

// SWEGCA: user@2026-09-22:72-79
void append_page_log_header(LedgerBytes& out, std::uint64_t log_ordinal) {
    ByteWriter writer(out);
    writer.raw(page_log_magic);
    writer.u16(format_version);
    writer.u64(log_ordinal);
}

// SWEGCA: user@2026-09-22:72-79
void check_page_log_header(std::span<const std::byte> bytes, std::uint64_t log_ordinal) {
    ByteReader reader(bytes);
    require_magic(reader, page_log_magic, "journal_page_log_magic_invalid");
    if (reader.u16() != format_version) fail("journal_page_log_version_invalid");
    if (reader.u64() != log_ordinal) fail("journal_page_log_ordinal_invalid");
}

// SWEGCA: user@2026-09-22:72-79
std::size_t encoded_manifest_size(std::string_view journal_identity, std::size_t extent_count,
                                  std::span<const ViewGeneration> views) {
    if (!detail::is_identity_text(journal_identity)) fail("journal_manifest_invalid:identity");
    if (extent_count > max_extents) fail("journal_manifest_invalid:extents");
    if (views.size() > max_manifest_views) fail("journal_manifest_invalid:views");
    std::size_t total = manifest_fixed_bytes + journal_identity.size();
    total += extent_count * encoded_extent_bytes;
    for (std::size_t at = 0; at < views.size(); ++at) {
        const auto name = views[at].name;
        // Strictly increasing names: no duplicate, and no set to build.
        if (!detail::is_identity_text(name) || (at != 0 && !(views[at - 1].name < name)))
            fail("journal_manifest_invalid:view");
        total += 4 + name.size() + 8 + 32;
    }
    if (total > max_manifest_bytes) fail("journal_manifest_invalid:size");
    return total;
}

// SWEGCA: user@2026-09-22:72-79
Manifest Manifest::encode(const ManifestFields& fields, std::string_view journal_identity,
                          std::span<const SegmentExtent> extents,
                          std::span<const ViewGeneration> views,
                          const MemoryLedger::Account& memory) {
    const auto total = encoded_manifest_size(journal_identity, extents.size(), views);
    LedgerBytes bytes(memory.allocator<std::byte>());
    bytes.reserve(total);
    ByteWriter writer(bytes);
    writer.raw(manifest_magic);
    writer.u16(format_version);
    writer.text(journal_identity, detail::identity_text_max_bytes);
    writer.u64(fields.generation);
    writer.digest(fields.previous_manifest_digest);
    encode_location(writer, fields.previous_location);
    writer.u8(fields.checkpoint ? 1 : 0);
    writer.u64(fields.checkpoint_generation);
    writer.u64(fields.recovery_bytes_before);
    writer.u64(fields.manifest_bytes_before);
    writer.u64(fields.state_generation_ordinal);
    writer.digest(fields.state_generation_digest);
    writer.u64(fields.tail_sequence);
    writer.digest(fields.tail_record_digest);
    writer.u64(fields.tail_segment_ordinal);
    encode_view_pages(writer, fields.view_pages);
    writer.u32(static_cast<std::uint32_t>(extents.size()));
    for (const auto& extent : extents) {
        writer.u64(extent.ordinal);
        writer.u64(extent.first_sequence);
        writer.u64(extent.record_count);
        writer.u64(extent.byte_length);
        writer.digest(extent.last_record_digest);
    }
    writer.u32(static_cast<std::uint32_t>(views.size()));
    for (const auto& view : views) {
        writer.text(view.name, detail::identity_text_max_bytes);
        writer.u64(view.generation);
        writer.digest(view.digest);
    }
    writer.digest(Sha256::of(bytes));
    if (bytes.size() != total) fail("journal_manifest_size_mismatch");
    return decode(std::move(bytes), memory);
}

// SWEGCA: user@2026-09-22:72-79
Manifest Manifest::decode(LedgerBytes bytes, const MemoryLedger::Account& memory) {
    if (bytes.size() > max_manifest_bytes) fail("journal_manifest_invalid:size");
    const std::span<const std::byte> all(bytes);
    ByteReader reader(all);
    require_magic(reader, manifest_magic, "journal_manifest_magic_invalid");
    if (reader.u16() != format_version) fail("journal_manifest_version_invalid");
    const auto identity = reader.text_view(detail::identity_text_max_bytes);
    if (!detail::is_identity_text(identity)) fail("journal_manifest_invalid:identity");
    const auto identity_offset = reader.offset() - identity.size();
    ManifestFields out;
    out.generation = reader.u64();
    out.previous_manifest_digest = reader.digest();
    out.previous_location = decode_location(reader);
    const auto checkpoint = reader.u8();
    if (checkpoint > 1) fail("journal_manifest_invalid:checkpoint");
    out.checkpoint = checkpoint == 1;
    out.checkpoint_generation = reader.u64();
    out.recovery_bytes_before = reader.u64();
    out.manifest_bytes_before = reader.u64();
    out.state_generation_ordinal = reader.u64();
    out.state_generation_digest = reader.digest();
    out.tail_sequence = reader.u64();
    out.tail_record_digest = reader.digest();
    out.tail_segment_ordinal = reader.u64();
    out.view_pages = decode_view_pages(reader, out.tail_sequence);

    const auto extent_count = reader.u32();
    if (extent_count > max_extents || extent_count > reader.remaining() / encoded_extent_bytes)
        fail("journal_manifest_invalid:extents");
    const auto extents_offset = reader.offset();
    SegmentExtent first{};
    SegmentExtent last{};
    for (std::uint32_t at = 0; at < extent_count; ++at) {
        SegmentExtent extent;
        extent.ordinal = reader.u64();
        extent.first_sequence = reader.u64();
        extent.record_count = reader.u64();
        extent.byte_length = reader.u64();
        extent.last_record_digest = reader.digest();
        if (extent.ordinal == 0 || extent.first_sequence == 0 || extent.record_count == 0 ||
            extent.byte_length > max_segment_bytes ||
            extent.record_count > (extent.byte_length - std::min<std::uint64_t>(
                                                            extent.byte_length,
                                                            segment_header_bytes)) /
                                      minimum_record_bytes)
            fail("journal_manifest_invalid:extent");
        if (at != 0 &&
            (extent.ordinal != add(last.ordinal, 1, "journal_manifest_invalid:ordinal") ||
             extent.first_sequence !=
                 add(last.first_sequence, last.record_count, "journal_manifest_invalid:sequence")))
            fail("journal_manifest_invalid:extent_order");
        (void)add(extent.first_sequence, extent.record_count - 1,
                  "journal_manifest_invalid:sequence");
        if (at == 0) first = extent;
        last = extent;
    }

    const auto view_count = reader.u32();
    if (view_count > max_manifest_views || view_count > reader.remaining() / minimum_view_bytes)
        fail("journal_manifest_invalid:views");
    LedgerVector<std::uint32_t> view_offsets(memory.allocator<std::uint32_t>());
    view_offsets.reserve(view_count);
    std::string_view previous_name;
    for (std::uint32_t at = 0; at < view_count; ++at) {
        view_offsets.push_back(static_cast<std::uint32_t>(reader.offset()));
        const auto name = reader.text_view(detail::identity_text_max_bytes);
        (void)reader.u64();
        (void)reader.digest();
        // Strictly increasing names: no duplicate, and no set to build.
        if (!detail::is_identity_text(name) || (at != 0 && !(previous_name < name)))
            fail("journal_manifest_invalid:view");
        previous_name = name;
    }
    const auto body = reader.consumed_since(0);
    const auto digest = reader.digest();
    if (reader.remaining() != 0) fail("journal_manifest_trailing_bytes");
    if (Sha256::of(body) != digest) fail("journal_manifest_digest_invalid");

    const bool genesis = out.generation == 0;
    if (genesis != (out.previous_manifest_digest == zero_digest) ||
        genesis != (out.previous_location.length == 0))
        fail("journal_manifest_invalid:previous");
    const bool empty = out.tail_segment_ordinal == 0;
    if (empty != (out.tail_sequence == 0) || empty != (out.tail_record_digest == zero_digest))
        fail("journal_manifest_invalid:tail");
    if (extent_count != 0 &&
        (last.ordinal != out.tail_segment_ordinal ||
         last.first_sequence + last.record_count - 1 != out.tail_sequence ||
         last.last_record_digest != out.tail_record_digest))
        fail("journal_manifest_invalid:tail");
    if (out.checkpoint) {
        // A checkpoint names every extent from ordinal 1 to the tail and
        // starts a new recovery chain.
        if (out.checkpoint_generation != out.generation || out.recovery_bytes_before != 0)
            fail("journal_manifest_invalid:checkpoint");
        if (empty ? extent_count != 0
                  : (extent_count == 0 || first.ordinal != 1 || first.first_sequence != 1))
            fail("journal_manifest_invalid:checkpoint");
    } else {
        // Recovery from here reads the chain back to a checkpoint that is at
        // most `checkpoint_interval - 1` generations and `max_recovery_bytes`
        // (this manifest included) away.
        if (genesis || out.checkpoint_generation >= out.generation ||
            out.generation - out.checkpoint_generation >= checkpoint_interval ||
            out.recovery_bytes_before == 0 ||
            out.recovery_bytes_before > max_recovery_bytes - all.size())
            fail("journal_manifest_invalid:recovery");
    }
    if (genesis && out.manifest_bytes_before != 0) fail("journal_manifest_invalid:previous");

    Manifest manifest(std::move(bytes), std::move(view_offsets));
    manifest.fields_ = out;
    manifest.identity_offset_ = identity_offset;
    manifest.identity_size_ = identity.size();
    manifest.extents_offset_ = extents_offset;
    manifest.extent_count_ = extent_count;
    manifest.digest_ = digest;
    return manifest;
}

// SWEGCA: user@2026-09-22:72-79
std::string_view Manifest::journal_identity() const noexcept {
    return std::string_view(reinterpret_cast<const char*>(bytes_.data()) + identity_offset_,
                            identity_size_);
}

// SWEGCA: user@2026-09-22:72-79
SegmentExtent Manifest::extent(std::size_t index) const {
    if (index >= extent_count_) fail("journal_manifest_extent_index_invalid");
    ByteReader reader(std::span<const std::byte>(bytes_).subspan(
        extents_offset_ + index * encoded_extent_bytes, encoded_extent_bytes));
    SegmentExtent extent;
    extent.ordinal = reader.u64();
    extent.first_sequence = reader.u64();
    extent.record_count = reader.u64();
    extent.byte_length = reader.u64();
    extent.last_record_digest = reader.digest();
    return extent;
}

// SWEGCA: user@2026-09-22:72-79
ViewGeneration Manifest::view(std::size_t index) const {
    if (index >= view_offsets_.size()) fail("journal_manifest_view_index_invalid");
    ByteReader reader(std::span<const std::byte>(bytes_).subspan(view_offsets_[index]));
    ViewGeneration view;
    view.name = reader.text_view(detail::identity_text_max_bytes);
    view.generation = reader.u64();
    view.digest = reader.digest();
    return view;
}

// SWEGCA: user@2026-09-22:72-79
void append_manifest_log_header(LedgerBytes& out, std::uint64_t log_ordinal) {
    ByteWriter writer(out);
    writer.raw(manifest_log_magic);
    writer.u16(format_version);
    writer.u64(log_ordinal);
}

// SWEGCA: user@2026-09-22:72-79
void check_manifest_log_header(std::span<const std::byte> bytes, std::uint64_t log_ordinal) {
    ByteReader reader(bytes);
    require_magic(reader, manifest_log_magic, "journal_manifest_log_magic_invalid");
    if (reader.u16() != format_version) fail("journal_manifest_log_version_invalid");
    if (reader.u64() != log_ordinal) fail("journal_manifest_log_ordinal_invalid");
}

// SWEGCA: user@2026-09-22:72-79
void append_head(LedgerBytes& out, const HeadPointer& head) {
    const auto start = out.size();
    ByteWriter writer(out);
    writer.raw(head_magic);
    writer.u16(format_version);
    encode_location(writer, head.location);
    writer.digest(head.manifest_digest);
    writer.digest(Sha256::of(std::span<const std::byte>(out).subspan(start)));
}

// SWEGCA: user@2026-09-22:72-79
HeadPointer decode_head(std::span<const std::byte> bytes) {
    if (bytes.size() != head_bytes) fail("journal_head_length_invalid");
    ByteReader reader(bytes);
    require_magic(reader, head_magic, "journal_head_magic_invalid");
    if (reader.u16() != format_version) fail("journal_head_version_invalid");
    HeadPointer head;
    head.location = decode_location(reader);
    head.manifest_digest = reader.digest();
    const auto body = reader.consumed_since(0);
    if (Sha256::of(body) != reader.digest()) fail("journal_head_digest_invalid");
    if (head.location.log_ordinal == 0 || head.location.length == 0 ||
        head.location.offset < manifest_log_header_bytes)
        fail("journal_head_location_invalid");
    return head;
}

}  // namespace swegca::architecture::journal
