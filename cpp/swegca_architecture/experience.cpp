#include "swegca_architecture/experience.hpp"

#include "swegca_architecture/sha256.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <utility>

namespace swegca::architecture {
namespace {

using journal::ByteReader;
using journal::ByteWriter;
using journal::LedgerBytes;
using journal::LedgerVector;

constexpr std::array<std::byte, 4> envelope_magic{std::byte{'S'}, std::byte{'W'}, std::byte{'X'},
                                                  std::byte{'P'}};
constexpr std::uint16_t envelope_version = 3;
// magic, version, observed step, uncertainty and contradiction bits, span
// flag, offset and length, context flag and digest, namespace flag, lineage
// count, resource count (a present namespace, the lineage, the resources and
// the blobs follow).
constexpr std::size_t envelope_fixed_bytes = 4 + 2 + 8 + 8 + 8 + 1 + 8 + 8 + 1 + 32 + 1 + 4 + 4;
constexpr std::size_t max_derived_from = 1024;
constexpr std::size_t max_resources = 1024;
constexpr std::size_t max_semantic_cues = 4096;
constexpr std::size_t digest_hex_bytes = 2 * digest256_width;
constexpr std::size_t address_bytes = experience_address_bytes;
// A part holds `experience_part_bytes` of a blob or that many bytes of the
// digest list one level below; the top list fits an inline blob.
constexpr std::uint64_t digests_per_part = experience_part_bytes / digest256_width;
constexpr std::uint64_t max_top_digests = experience_inline_blob_bytes / digest256_width;
static_assert(experience_part_bytes % digest256_width == 0 && experience_part_bytes <= journal::max_payload_bytes);
// Inline: mode, length, bytes. Parted: mode, size, digest, depth, top count,
// top digests.
constexpr std::size_t inline_blob_encoded_bytes = 1 + 4 + experience_inline_blob_bytes;
constexpr std::size_t parted_blob_encoded_bytes = 1 + 8 + 32 + 1 + 4 + max_top_digests * digest256_width;
constexpr std::size_t max_blob_encoded_bytes = std::max(inline_blob_encoded_bytes, parted_blob_encoded_bytes);
// Every experience's envelope fits one record whatever its bytes: four
// blobs (raw, structured, root sources, root contexts) at their largest
// encoding, and the
// namespace, lineage and resources at their limits.
static_assert(envelope_fixed_bytes + 4 + detail::identity_text_max_bytes +
                      max_derived_from * (4 + address_bytes) +
                      max_resources * (4 + detail::identity_text_max_bytes) + 4 * max_blob_encoded_bytes <=
                  journal::max_payload_bytes,
              "an experience's envelope must always fit one record");
// A part record's producer and revision: the part belongs to this module,
// and equal bytes are one part whoever observed them.
constexpr std::string_view part_source = "experience";
constexpr std::string_view part_source_revision = "part.v1";
// A cue entry keeps its token when its key (kind, token, separator,
// address) is an identity text; a longer token is kept by its digest ('h').
constexpr std::size_t max_inline_cue_bytes = detail::identity_text_max_bytes - 2 - address_bytes;
// Every entry an experience can carry fits one record: the tokens of the
// source and of its revision (at most one per byte), the authored cues,
// lineage, resources, and source, content, revised address, namespace and
// transaction.
static_assert(2 * detail::identity_text_max_bytes + max_semantic_cues + max_derived_from +
                      max_resources + 5 <=
                  journal::max_record_index_entries,
              "an experience's index entries must always fit one record");

// SWEGCA: user@2026-09-22:60-61
[[noreturn]] void fail(const char* code) { throw std::invalid_argument(code); }

// A fraction in [0, 1]; -0 is kept as +0 so equal values have equal bytes.
// SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:193-200
double unit_fraction(double value, const char* code) {
    if (!(std::isfinite(value) && value >= 0 && value <= 1)) fail(code);
    return value + 0.0;
}

// Length-prefixed field so no two field sequences hash the same bytes.
// SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:376-388
void hash_field(Sha256& hash, std::string_view text) {
    const auto length = static_cast<std::uint64_t>(text.size());
    std::array<std::byte, 8> prefix{};
    for (std::size_t at = 0; at < prefix.size(); ++at)
        prefix[at] = static_cast<std::byte>((length >> (8 * at)) & 0xff);
    hash.update(prefix);
    hash.update(text);
}

// SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:376-388
void hash_u64(Sha256& hash, std::uint64_t value) {
    std::array<std::byte, 8> bytes{};
    for (std::size_t at = 0; at < bytes.size(); ++at)
        bytes[at] = static_cast<std::byte>((value >> (8 * at)) & 0xff);
    hash.update(bytes);
}

// SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:376-388
void hash_optional(Sha256& hash, const std::optional<std::string_view>& text) {
    hash_u64(hash, text ? 1 : 0);
    hash_field(hash, text.value_or(std::string_view()));
}

// SWEGCA: src/swegca/mosaic_external_memory.py@5901a5a:15-26
DigestBytes text_digest(std::string_view text) {
    Sha256 hash;
    hash.update(text);
    return hash.finish();
}

// Lowercase hex of a digest, without allocating.
// SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:31-35
std::array<char, digest_hex_bytes> hex_of(const DigestBytes& digest) noexcept {
    static constexpr char digits[] = "0123456789abcdef";
    std::array<char, digest_hex_bytes> out{};
    for (std::size_t at = 0; at < digest.size(); ++at) {
        const auto byte = std::to_integer<unsigned>(digest[at]);
        out[at * 2] = digits[byte >> 4];
        out[at * 2 + 1] = digits[byte & 0x0f];
    }
    return out;
}

// SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:31-35
bool is_hex_digest(std::string_view text) noexcept {
    return text.size() == digest_hex_bytes &&
           std::all_of(text.begin(), text.end(),
                       [](char value) { return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f'); });
}

// SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:31-35
bool is_experience_address(std::string_view text) noexcept {
    return text.size() == address_bytes && text.starts_with(experience_address_prefix) &&
           is_hex_digest(text.substr(experience_address_prefix.size()));
}

// The address of the part whose bytes have `digest`.
// SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:31-35
std::array<char, experience_part_address_bytes> part_address_of(const DigestBytes& digest) noexcept {
    const auto hex = hex_of(digest);
    std::array<char, experience_part_address_bytes> out{};
    std::copy(experience_part_address_prefix.begin(), experience_part_address_prefix.end(), out.begin());
    std::copy(hex.begin(), hex.end(), out.begin() + experience_part_address_prefix.size());
    return out;
}

// The 32 bytes at `at` of a digest list.
// SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:31-35
DigestBytes digest_at(std::span<const std::byte> list, std::uint64_t at) noexcept {
    DigestBytes out{};
    std::memcpy(out.data(), list.data() + at * digest256_width, digest256_width);
    return out;
}

// SWEGCA: user@2026-09-22:60-61
constexpr std::uint64_t ceil_div(std::uint64_t value, std::uint64_t by) noexcept {
    return value == 0 ? 0 : (value - 1) / by + 1;
}

// The part count of each level of a parted blob of `size` bytes: level 0
// cuts the bytes, each higher level the digest list below it, until the top
// list fits an inline blob. `levels[depth - 1]` is the top count.
struct PartLevels {
    std::array<std::uint64_t, 4> counts{};
    std::uint8_t depth = 0;
};

// SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:23-60
PartLevels part_levels(std::uint64_t size) noexcept {
    PartLevels out;
    auto count = ceil_div(size, experience_part_bytes);
    out.counts[out.depth++] = count;
    while (count > max_top_digests) {
        count = ceil_div(count, digests_per_part);
        out.counts[out.depth++] = count;
    }
    return out;
}
static_assert(ceil_div(ceil_div(ceil_div(std::numeric_limits<std::uint64_t>::max(), experience_part_bytes),
                                digests_per_part),
                       digests_per_part) <= max_top_digests,
              "three part levels hold any u64 size");

// One code point of strict UTF-8 text at `at`, which moves past it.
// SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:413-418
char32_t decode_at(std::string_view text, std::size_t& at) noexcept {
    const auto lead = static_cast<unsigned char>(text[at]);
    const std::size_t width = lead < 0x80 ? 1 : lead < 0xe0 ? 2 : lead < 0xf0 ? 3 : 4;
    char32_t value = width == 1 ? lead : width == 2 ? (lead & 0x1f) : width == 3 ? (lead & 0x0f) : (lead & 0x07);
    for (std::size_t next = 1; next < width; ++next)
        value = (value << 6) | (static_cast<unsigned char>(text[at + next]) & 0x3f);
    at += width;
    return value;
}

// Non-ASCII letters of the cue rule: every code point from U+00C0 except
// the listed marks, punctuation, symbol, byte-order-mark and private blocks.
// SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:413-418
bool is_cue_letter(char32_t value) noexcept {
    static constexpr std::array<std::pair<char32_t, char32_t>, 17> separators{{
        {0x00d7, 0x00d7},    {0x00f7, 0x00f7},    {0x0300, 0x036f},    {0x2000, 0x2bff},
        {0x2e00, 0x2e7f},    {0x3000, 0x303f},    {0xfe10, 0xfe1f},    {0xfe30, 0xfe6f},
        {0xfeff, 0xfeff},    {0xff00, 0xff20},    {0xff3b, 0xff40},    {0xff5b, 0xff65},
        {0xfff0, 0xffff},    {0x1f000, 0x1faff},  {0xe0000, 0xe007f},  {0xf0000, 0x10ffff},
        {0xd800, 0xdfff},
    }};
    if (value < 0xc0) return false;
    for (const auto& [first, last] : separators)
        if (value >= first && value <= last) return false;
    return true;
}

// Calls `emit(begin, end)` for each token of lowered `text` in order.
// SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:413-418
template <class Emit>
void scan_cue_tokens(std::string_view text, Emit emit) {
    const auto size = text.size();
    const auto digit = [&](std::size_t at) { return at < size && text[at] >= '0' && text[at] <= '9'; };
    const auto lower = [&](std::size_t at) { return at < size && text[at] >= 'a' && text[at] <= 'z'; };
    const auto letter_at = [&](std::size_t at, std::size_t& next) {
        if (at >= size || static_cast<unsigned char>(text[at]) < 0x80) return false;
        next = at;
        return is_cue_letter(decode_at(text, next));
    };
    std::size_t at = 0;
    while (at < size) {
        std::size_t end = at;
        if ((text[at] == 'n' || text[at] == 'r') && digit(at + 1)) {
            end = at + 1;
            while (digit(end)) ++end;
        } else if (lower(at)) {
            while (lower(end)) ++end;
        } else if (digit(at)) {
            while (digit(end)) ++end;
        } else if (std::size_t next = 0; letter_at(at, next)) {
            end = next;
            for (;;) {
                if (lower(end)) {
                    ++end;
                } else if (letter_at(end, next)) {
                    end = next;
                } else {
                    break;
                }
            }
        } else {
            if (static_cast<unsigned char>(text[at]) < 0x80) {
                ++at;
            } else {
                (void)decode_at(text, at);
            }
            continue;
        }
        emit(at, end);
        at = end;
    }
}

// True when `text` is exactly one token of the cue rule (so it is already
// lowered): what an authored cue and a cue-view key must be.
// SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:411-431
bool is_single_token(const AllocationContext& memory, std::string_view text) {
    if (!detail::is_identity_text(text)) return false;
    const CueTokens tokens(memory, text);
    return tokens.tokens().size() == 1 && tokens.tokens().front() == text;
}

// The cue-view entry of one token: the token itself, or its digest when its
// key would not be an identity text.
// SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:293-339
template <class Visit>
void with_cue_entry(std::string_view token, Visit&& visit) {
    if (token.size() <= max_inline_cue_bytes) {
        visit('c', token);
        return;
    }
    const auto hex = hex_of(text_digest(token));
    visit('h', std::string_view(hex.data(), hex.size()));
}

// The index entries of one experience, copied into one charged buffer and
// viewed, sorted and unique, once complete.
class IndexEntries final {
public:
    // SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@cefdc3f:122-123
    explicit IndexEntries(const AllocationContext& memory)
        : bytes_(memory.allocator<std::byte>()),
          spans_(memory.allocator<std::pair<std::size_t, std::size_t>>()),
          entries_(memory.allocator<std::string_view>()) {}
    IndexEntries(IndexEntries&&) noexcept = default;
    IndexEntries& operator=(IndexEntries&&) = delete;
    IndexEntries(const IndexEntries&) = delete;
    IndexEntries& operator=(const IndexEntries&) = delete;
    ~IndexEntries() = default;

    // SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@cefdc3f:122-123
    void add(char kind, std::string_view value) {
        const auto at = bytes_.size();
        bytes_.push_back(static_cast<std::byte>(kind));
        const auto* data = reinterpret_cast<const std::byte*>(value.data());
        bytes_.insert(bytes_.end(), data, data + value.size());
        spans_.emplace_back(at, value.size() + 1);
    }

    // SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@cefdc3f:122-123
    void add_digest(char kind, const DigestBytes& digest) {
        const auto hex = hex_of(digest);
        add(kind, std::string_view(hex.data(), hex.size()));
    }

    // SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:293-339
    void add_cue(std::string_view token) {
        with_cue_entry(token, [this](char kind, std::string_view value) { add(kind, value); });
    }

    // Views every entry, sorted and unique; nothing is added after this.
    // SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@cefdc3f:122-123
    std::span<const std::string_view> finish() {
        entries_.reserve(spans_.size());
        for (const auto& [at, size] : spans_)
            entries_.emplace_back(reinterpret_cast<const char*>(bytes_.data()) + at, size);
        std::sort(entries_.begin(), entries_.end());
        entries_.erase(std::unique(entries_.begin(), entries_.end()), entries_.end());
        return entries_;
    }

    // SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@cefdc3f:122-123
    [[nodiscard]] std::span<const std::string_view> entries() const noexcept { return entries_; }

    // Hand the finished entries and the bytes they view to a new owner. Both
    // are moved by construction, which keeps each buffer, so the views stay
    // valid; this object is empty afterwards.
    // SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@cefdc3f:122-123
    [[nodiscard]] LedgerVector<std::string_view> take_entries() noexcept { return std::move(entries_); }
    // SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@cefdc3f:122-123
    [[nodiscard]] LedgerBytes take_bytes() noexcept { return std::move(bytes_); }

private:
    LedgerBytes bytes_;
    LedgerVector<std::pair<std::size_t, std::size_t>> spans_;
    LedgerVector<std::string_view> entries_;  // views `bytes_`, which a move keeps
};

// What an experience's automatic index entries are derived from.
struct IndexFields {
    std::string_view source;
    std::string_view source_revision;
    std::optional<std::string_view> previous;
    std::span<const std::string_view> derived_from;
    std::optional<std::string_view> name_space;
    std::span<const std::string_view> resources;
    const DigestBytes& raw_digest;
    std::optional<std::string_view> transaction;
};

// The entries every experience carries, derived from its fields alone: the
// cue tokens of its source and revision, and its source, content, lineage,
// revised address, namespace, resources and transaction.
// SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@cefdc3f:122-123
void add_automatic(IndexEntries& index, const AllocationContext& memory, const IndexFields& fields) {
    for (const auto text : {fields.source, fields.source_revision}) {
        const CueTokens tokens(memory, text);
        for (const auto token : tokens.tokens()) index.add_cue(token);
    }
    index.add_digest(static_cast<char>(ExperienceView::source), text_digest(fields.source));
    index.add_digest(static_cast<char>(ExperienceView::content), fields.raw_digest);
    for (const auto address : fields.derived_from) index.add(static_cast<char>(ExperienceView::lineage), address);
    if (fields.previous) index.add(static_cast<char>(ExperienceView::successor), *fields.previous);
    if (fields.name_space)
        index.add_digest(static_cast<char>(ExperienceView::name_space), text_digest(*fields.name_space));
    for (const auto resource : fields.resources)
        index.add_digest(static_cast<char>(ExperienceView::resource), text_digest(resource));
    if (fields.transaction)
        index.add_digest(static_cast<char>(ExperienceView::transaction), text_digest(*fields.transaction));
}

// Equal index entries apart from the transaction, which records where an
// observation was first appended and is not part of what was observed.
// SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@cefdc3f:122-123
bool same_observed_index(std::span<const std::string_view> left, std::span<const std::string_view> right) noexcept {
    const auto skip = [](std::span<const std::string_view> entries, std::size_t& at) {
        while (at < entries.size() && entries[at].front() == static_cast<char>(ExperienceView::transaction))
            ++at;
    };
    std::size_t left_at = 0;
    std::size_t right_at = 0;
    for (;;) {
        skip(left, left_at);
        skip(right, right_at);
        if (left_at == left.size() || right_at == right.size())
            return left_at == left.size() && right_at == right.size();
        if (left[left_at] != right[right_at]) return false;
        ++left_at;
        ++right_at;
    }
}

// An entry no field derives: only an authored cue can be one. A digest
// entry ('h') is checked by form only: which token it hashes is not
// recoverable from the record.
// SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:411-431
bool is_authored_cue_entry(const AllocationContext& memory, std::string_view entry) {
    const auto value = entry.substr(1);
    if (entry.front() == 'c') return value.size() <= max_inline_cue_bytes && is_single_token(memory, value);
    return entry.front() == 'h' && is_hex_digest(value);
}

// The record identity an address is the digest of: kind, source, revision,
// revised address, outcome and payload digest. Index entries are derived
// from these fields or authored, never identity.
// SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:31-35
std::array<char, address_bytes> address_of(std::uint16_t kind, std::string_view source,
                                           std::string_view source_revision,
                                           const std::optional<std::string_view>& previous,
                                           const std::optional<std::string_view>& outcome,
                                           const DigestBytes& payload_digest) {
    Sha256 hash;
    hash_field(hash, "swegca.experience_address.v2");
    hash_u64(hash, kind);
    hash_field(hash, source);
    hash_field(hash, source_revision);
    hash_optional(hash, previous);
    hash_optional(hash, outcome);
    hash.update(payload_digest);
    const auto hex = hex_of(hash.finish());
    std::array<char, address_bytes> out{};
    std::copy(experience_address_prefix.begin(), experience_address_prefix.end(), out.begin());
    std::copy(hex.begin(), hex.end(), out.begin() + experience_address_prefix.size());
    return out;
}

// SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:31-35
template <std::size_t N>
std::string_view view_of(const std::array<char, N>& address) noexcept {
    return std::string_view(address.data(), address.size());
}

// A record text as an optional: empty on disk means absent.
// SWEGCA: user@2026-09-22:60-61
std::optional<std::string_view> present(std::string_view text) noexcept {
    return text.empty() ? std::nullopt : std::optional<std::string_view>(text);
}

// One part as appending finds it: its digest and where its bytes are (the
// caller's memory, a digest list this module built, or the caller's reader).
struct PartSlice {
    DigestBytes digest{};
    std::span<const std::byte> bytes;
    const BlobReader* reader = nullptr;
    std::uint64_t offset = 0;
    std::uint64_t length = 0;
};

// A blob as appending writes it: inline, or parted with its top digest list.
struct BlobPlan {
    std::uint64_t size = 0;
    DigestBytes digest{};
    std::uint8_t depth = 0;
    std::span<const std::byte> inline_bytes;
    std::span<const std::byte> top;  // views `owned`
};

// Hashes `input` and, when it does not fit inline, cuts it into parts level
// by level (see ExperienceBlob): each part goes to `parts`, each digest list
// to `owned` (parts of the next level view it). Bytes in memory are never
// copied; read bytes are read one part at a time into one buffer, and a read
// blob small enough to be inline is read into `owned`.
// SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:23-60
BlobPlan plan_blob(const AllocationContext& memory, const BlobInput& input,
                   LedgerVector<PartSlice>& parts, LedgerVector<LedgerBytes>& owned) {
    BlobPlan out;
    out.size = input.size;
    auto bytes = input.bytes;
    if (input.reader && input.size <= experience_inline_blob_bytes) {
        LedgerBytes copy(static_cast<std::size_t>(input.size), std::byte{}, memory.allocator<std::byte>());
        (*input.reader)(0, copy);
        owned.push_back(std::move(copy));
        bytes = owned.back();
    }
    if (out.size <= experience_inline_blob_bytes) {
        out.digest = Sha256::of(bytes);
        out.inline_bytes = bytes;
        return out;
    }
    const auto levels = part_levels(out.size);
    LedgerBytes list(memory.allocator<std::byte>());
    list.reserve(static_cast<std::size_t>(levels.counts[0] * digest256_width));
    Sha256 whole;
    if (input.reader) {
        LedgerBytes buffer(experience_part_bytes, std::byte{}, memory.allocator<std::byte>());
        for (std::uint64_t at = 0; at < levels.counts[0]; ++at) {
            const auto offset = at * experience_part_bytes;
            const auto length = std::min<std::uint64_t>(experience_part_bytes, out.size - offset);
            const std::span<std::byte> slice(buffer.data(), static_cast<std::size_t>(length));
            (*input.reader)(offset, slice);
            const auto digest = Sha256::of(slice);
            whole.update(slice);
            list.insert(list.end(), digest.begin(), digest.end());
            parts.push_back(PartSlice{digest, {}, &*input.reader, offset, length});
        }
    } else {
        for (std::uint64_t at = 0; at < levels.counts[0]; ++at) {
            const auto offset = at * experience_part_bytes;
            const auto slice = bytes.subspan(static_cast<std::size_t>(offset),
                                             static_cast<std::size_t>(std::min<std::uint64_t>(
                                                 experience_part_bytes, out.size - offset)));
            const auto digest = Sha256::of(slice);
            whole.update(slice);
            list.insert(list.end(), digest.begin(), digest.end());
            parts.push_back(PartSlice{digest, slice});
        }
    }
    out.digest = whole.finish();
    owned.push_back(std::move(list));  // a moved vector keeps its buffer: earlier slices stay valid
    std::span<const std::byte> below = owned.back();
    for (std::uint8_t level = 1; level < levels.depth; ++level) {
        LedgerBytes upper(memory.allocator<std::byte>());
        upper.reserve(static_cast<std::size_t>(levels.counts[level] * digest256_width));
        for (std::uint64_t at = 0; at < levels.counts[level]; ++at) {
            const auto offset = at * experience_part_bytes;
            const auto slice = below.subspan(static_cast<std::size_t>(offset),
                                             static_cast<std::size_t>(std::min<std::uint64_t>(
                                                 experience_part_bytes, below.size() - offset)));
            const auto digest = Sha256::of(slice);
            upper.insert(upper.end(), digest.begin(), digest.end());
            parts.push_back(PartSlice{digest, slice});
        }
        owned.push_back(std::move(upper));
        below = owned.back();
    }
    out.depth = levels.depth;
    out.top = below;
    return out;
}

// A digest set as the bytes of an inline or parted blob: sorted, unique.
// SWEGCA: paper/swegca/journal_submission_2026-08-25/COMPONENT_LEDGER.md@5901a5a:44-50
LedgerBytes digest_set_bytes(const AllocationContext& memory, LedgerVector<DigestBytes>& digests) {
    std::sort(digests.begin(), digests.end());
    digests.erase(std::unique(digests.begin(), digests.end()), digests.end());
    LedgerBytes bytes(memory.allocator<std::byte>());
    bytes.reserve(digests.size() * digest256_width);
    for (const auto& digest : digests) bytes.insert(bytes.end(), digest.begin(), digest.end());
    return bytes;
}

// SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:23-60
std::uint64_t encoded_blob_bytes(const BlobPlan& blob) noexcept {
    return blob.depth == 0 ? 1 + 4 + blob.size : 1 + 8 + 32 + 1 + 4 + blob.top.size();
}

// SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:23-60
void write_blob(ByteWriter& writer, const BlobPlan& blob) {
    if (blob.depth == 0) {
        writer.u8(0);
        writer.bytes(blob.inline_bytes, experience_inline_blob_bytes);
        return;
    }
    writer.u8(1);
    writer.u64(blob.size);
    writer.digest(blob.digest);
    writer.u8(blob.depth);
    writer.u32(static_cast<std::uint32_t>(blob.top.size() / digest256_width));
    writer.raw(blob.top);
}

// Reads one blob in its one form: inline up to the inline size, otherwise
// parted with exactly the depth and top count its size gives.
// SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:23-60
ExperienceBlob read_blob(ByteReader& reader) {
    ExperienceBlob out;
    const auto mode = reader.u8();
    if (mode == 0) {
        out.inline_bytes = reader.bytes_view(experience_inline_blob_bytes);
        out.size = out.inline_bytes.size();
        out.digest = Sha256::of(out.inline_bytes);
        return out;
    }
    if (mode != 1) fail("experience_envelope_invalid");
    out.size = reader.u64();
    out.digest = reader.digest();
    out.depth = reader.u8();
    const auto count = reader.u32();
    if (out.size <= experience_inline_blob_bytes) fail("experience_envelope_invalid");
    const auto levels = part_levels(out.size);
    if (out.depth != levels.depth || count != levels.counts[levels.depth - 1])
        fail("experience_envelope_invalid");
    out.top_digests = reader.raw(static_cast<std::size_t>(count) * digest256_width);
    return out;
}

// The experience payload: the observation's step, uncertainty,
// contradiction, source span, context, namespace, sorted lineage, sorted
// resources, and its raw, structured and (derived only) root-source and
// root-context blobs.
// SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:23-60
LedgerBytes encode_envelope(const AllocationContext& memory, const Observation& observation,
                            std::span<const std::string_view> derived_from,
                            std::span<const std::string_view> resources, const BlobPlan& raw,
                            const BlobPlan& structured, const BlobPlan* roots, const BlobPlan* contexts) {
    std::uint64_t total = envelope_fixed_bytes;
    if (observation.name_space) total += 4 + observation.name_space->size();
    for (const auto address : derived_from) total += 4 + address.size();
    for (const auto resource : resources) total += 4 + resource.size();
    total += encoded_blob_bytes(raw) + encoded_blob_bytes(structured);
    if (roots) total += encoded_blob_bytes(*roots) + encoded_blob_bytes(*contexts);
    if (total > journal::max_payload_bytes) fail("experience_envelope_too_large");
    LedgerBytes out(memory.allocator<std::byte>());
    out.reserve(static_cast<std::size_t>(total));
    ByteWriter writer(out);
    writer.raw(envelope_magic);
    writer.u16(envelope_version);
    writer.u64(observation.observed_at);
    writer.u64(std::bit_cast<std::uint64_t>(unit_fraction(observation.uncertainty, "experience_uncertainty_invalid")));
    writer.u64(std::bit_cast<std::uint64_t>(unit_fraction(observation.contradiction, "experience_contradiction_invalid")));
    writer.u8(observation.source_span ? 1 : 0);
    writer.u64(observation.source_span ? observation.source_span->offset : 0);
    writer.u64(observation.source_span ? observation.source_span->length : 0);
    writer.u8(observation.context ? 1 : 0);
    writer.digest(observation.context ? observation.context->bytes() : zero_digest_bytes);
    writer.u8(observation.name_space ? 1 : 0);
    if (observation.name_space) writer.text(*observation.name_space, detail::identity_text_max_bytes);
    writer.u32(static_cast<std::uint32_t>(derived_from.size()));
    for (const auto address : derived_from) writer.text(address, detail::identity_text_max_bytes);
    writer.u32(static_cast<std::uint32_t>(resources.size()));
    for (const auto resource : resources) writer.text(resource, detail::identity_text_max_bytes);
    write_blob(writer, raw);
    write_blob(writer, structured);
    if (roots) {
        write_blob(writer, *roots);
        write_blob(writer, *contexts);
    }
    if (out.size() != total) fail("experience_envelope_size_mismatch");
    return out;
}

// Reads `count` identity texts, strictly increasing, each accepted by `valid`.
// SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:23-60
template <class Valid>
LedgerVector<std::string_view> read_sorted_texts(ByteReader& reader, std::uint32_t count,
                                                 const AllocationContext& memory, Valid valid) {
    LedgerVector<std::string_view> out(memory.allocator<std::string_view>());
    out.reserve(count);
    for (std::uint32_t at = 0; at < count; ++at) {
        const auto text = reader.text_view(detail::identity_text_max_bytes);
        if (!detail::is_identity_text(text) || !valid(text) || (at != 0 && !(out.back() < text)))
            fail("experience_envelope_invalid");
        out.push_back(text);
    }
    return out;
}

// What one generation may still take, by the journal's two generation
// limits: record bytes, and index-view leaf items.
class GenerationBudget final {
public:
    // False, taking nothing, when `draft` does not fit.
    // SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@cefdc3f:282-283
    bool add(const journal::RecordDraft& draft) {
        const std::uint64_t size = journal::encoded_record_size(draft);
        std::uint64_t items = 0;
        for (const auto entry : draft.index)
            items += journal::index_key_size(entry, draft.address) + journal::encoded_leaf_item_size(std::string_view());
        if (size > journal::max_generation_bytes - records_ || items > journal::max_generation_bytes - index_)
            return false;
        records_ += size;
        index_ += items;
        return true;
    }

private:
    std::uint64_t records_ = 0;
    std::uint64_t index_ = 0;
};

}  // namespace

// SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:413-418
CueTokens::CueTokens(const AllocationContext& memory, std::string_view source)
    : text_(memory.allocator<std::byte>()), tokens_(memory.allocator<std::string_view>()) {
    if (!detail::is_strict_utf8(source)) fail("cue_text_not_utf8");
    text_.reserve(source.size());
    for (const char value : source)
        text_.push_back(static_cast<std::byte>(value >= 'A' && value <= 'Z' ? value - 'A' + 'a' : value));
    const std::string_view text(reinterpret_cast<const char*>(text_.data()), text_.size());
    std::size_t count = 0;
    scan_cue_tokens(text, [&count](std::size_t, std::size_t) { ++count; });
    tokens_.reserve(count);
    scan_cue_tokens(text, [&](std::size_t begin, std::size_t end) {
        tokens_.push_back(text.substr(begin, end - begin));
    });
}

// SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:23-60
ExperienceRecord::ExperienceRecord(journal::PublishedRecord record, const AllocationContext& memory,
                                   const journal::JournalStore& journal, LedgerVector<std::string_view> derived,
                                   LedgerVector<std::string_view> resources, LedgerVector<std::string_view> index)
    : record_(std::move(record)), journal_(&journal), memory_(memory),
      derived_from_(std::move(derived)), resources_(std::move(resources)), index_(std::move(index)) {}

// SWEGCA: user@2026-09-22:72-79
ExperienceRecord::ExperienceRecord(ExperienceRecord&& other) noexcept
    : record_(std::move(other.record_)), journal_(other.journal_), memory_(other.memory_),
      observed_at_(other.observed_at_), context_(other.context_), uncertainty_(other.uncertainty_),
      contradiction_(other.contradiction_), span_(other.span_), derived_from_(std::move(other.derived_from_)),
      name_space_(std::exchange(other.name_space_, std::nullopt)), resources_(std::move(other.resources_)),
      index_(std::move(other.index_)), raw_(std::exchange(other.raw_, {})),
      structured_(std::exchange(other.structured_, {})), roots_(std::exchange(other.roots_, {})),
      contexts_(std::exchange(other.contexts_, {})) {}

// Checks the kind, the envelope byte for byte, that the address is the
// digest of the record's identity (so it was issued by this module), and
// that the index entries are exactly the automatic ones plus authored cues.
// A parted blob's parts are checked when they are read.
// SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:23-60
ExperienceRecord ExperienceRecord::decode(journal::PublishedRecord published,
                                          const AllocationContext& memory,
                                          const journal::JournalStore& journal) {
    const auto& view = published.view();
    if (view.kind != original_experience_kind && view.kind != derived_experience_kind)
        fail("experience_kind_invalid");
    // An experience documents; it carries no authority and judges no claim.
    if (view.authority || !view.claim.empty()) fail("experience_record_invalid");
    if (!view.previous_revision_address.empty() && !is_experience_address(view.previous_revision_address))
        fail("experience_record_invalid");
    ByteReader reader(view.payload);
    const auto magic = reader.raw(envelope_magic.size());
    if (!std::equal(magic.begin(), magic.end(), envelope_magic.begin()))
        fail("experience_envelope_invalid");
    if (reader.u16() != envelope_version) fail("experience_envelope_version_invalid");
    const auto observed_at = reader.u64();
    const auto uncertainty = std::bit_cast<double>(reader.u64());
    const auto contradiction = std::bit_cast<double>(reader.u64());
    if (unit_fraction(uncertainty, "experience_envelope_invalid") != uncertainty ||
        std::signbit(uncertainty) ||
        unit_fraction(contradiction, "experience_envelope_invalid") != contradiction ||
        std::signbit(contradiction))
        fail("experience_envelope_invalid");
    const auto has_span = reader.u8();
    const SourceSpan span{reader.u64(), reader.u64()};
    if (has_span > 1 || (has_span == 0 && (span.offset != 0 || span.length != 0)))
        fail("experience_envelope_invalid");
    const auto has_context = reader.u8();
    const auto context = reader.digest();
    if (has_context > 1 || (has_context == 0 && context != zero_digest_bytes))
        fail("experience_envelope_invalid");
    const auto has_name_space = reader.u8();
    if (has_name_space > 1) fail("experience_envelope_invalid");
    std::optional<std::string_view> name_space;
    if (has_name_space == 1) {
        name_space = reader.text_view(detail::identity_text_max_bytes);
        if (!detail::is_identity_text(*name_space)) fail("experience_envelope_invalid");
    }
    const auto derived_count = reader.u32();
    if (derived_count > max_derived_from || (derived_count != 0) != (view.kind == derived_experience_kind))
        fail("experience_envelope_invalid");
    auto derived = read_sorted_texts(reader, derived_count, memory, is_experience_address);
    const auto resource_count = reader.u32();
    if (resource_count > max_resources) fail("experience_envelope_invalid");
    auto resources = read_sorted_texts(reader, resource_count, memory, [](std::string_view) { return true; });
    const auto raw = read_blob(reader);
    const auto structured = read_blob(reader);
    ExperienceBlob roots;
    ExperienceBlob contexts;
    if (view.kind == derived_experience_kind) {
        // Whole digests, increasing (checked here when inline, while
        // streaming when parted); at least one root source, and any
        // number of root contexts (originals may have none).
        const auto digest_set = [&reader](bool nonempty) {
            const auto blob = read_blob(reader);
            if ((nonempty && blob.size == 0) || blob.size % digest256_width != 0)
                fail("experience_envelope_invalid");
            for (std::uint64_t at = 1; !blob.parted() && at < blob.size / digest256_width; ++at)
                if (!(digest_at(blob.inline_bytes, at - 1) < digest_at(blob.inline_bytes, at)))
                    fail("experience_envelope_invalid");
            return blob;
        };
        roots = digest_set(true);
        contexts = digest_set(false);
    }
    if (reader.remaining() != 0) fail("experience_envelope_trailing_bytes");

    const auto address = address_of(view.kind, view.source, view.source_revision,
                                     present(view.previous_revision_address), present(view.outcome),
                                     view.payload_digest);
    if (view.address != view_of(address)) fail("experience_address_mismatch");

    // The record's entries against the automatic ones: every automatic one
    // present, and every other one an authored cue.
    LedgerVector<std::string_view> index(memory.allocator<std::string_view>());
    index.reserve(view.index_count);
    journal::for_each_index_entry(view, [&index](std::string_view entry) { index.push_back(entry); });
    IndexEntries automatic(memory);
    add_automatic(automatic, memory,
                  IndexFields{view.source, view.source_revision, present(view.previous_revision_address),
                              derived, name_space, resources, raw.digest, present(view.transaction_id)});
    const auto expected = automatic.finish();
    std::size_t next = 0;
    std::size_t authored = 0;
    for (const auto entry : index) {
        if (next < expected.size() && expected[next] == entry) {
            ++next;
            continue;
        }
        if (next < expected.size() && expected[next] < entry) fail("experience_index_incomplete");
        if (!is_authored_cue_entry(memory, entry) || ++authored > max_semantic_cues)
            fail("experience_index_invalid");
    }
    if (next != expected.size()) fail("experience_index_incomplete");

    ExperienceRecord out(std::move(published), memory, journal, std::move(derived), std::move(resources),
                         std::move(index));
    out.observed_at_ = observed_at;
    if (has_context == 1) out.context_.emplace(context);
    out.uncertainty_ = uncertainty;
    out.contradiction_ = contradiction;
    if (has_span == 1) out.span_ = span;
    out.name_space_ = name_space;
    out.raw_ = raw;
    out.structured_ = structured;
    out.roots_ = roots;
    out.contexts_ = contexts;
    return out;
}

// SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:23-60
std::span<const std::byte> ExperienceRecord::raw() const {
    if (raw_.parted()) fail("experience_blob_parted");
    return raw_.inline_bytes;
}

// SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:23-60
std::span<const std::byte> ExperienceRecord::structured() const {
    if (structured_.parted()) fail("experience_blob_parted");
    return structured_.inline_bytes;
}

// SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:137-155
void ExperienceRecord::for_each_raw_chunk(ChunkVisitor visit) const { for_each_chunk(raw_, visit); }

// SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:137-155
void ExperienceRecord::for_each_structured_chunk(ChunkVisitor visit) const { for_each_chunk(structured_, visit); }

// Walks the part tree depth first from the top list. A part must be a part
// record with no authority, claim or index entry, its payload the digest
// its parent names and exactly as long as its place in the tree gives (the
// last part of each level may be shorter). Each level-0 part's bytes are
// visited once checked; the whole blob's digest is checked after the last.
// SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:137-155
void ExperienceRecord::for_each_chunk(const ExperienceBlob& blob, ChunkVisitor visit) const {
    if (!blob.parted()) {
        (void)visit(blob.inline_bytes);
        return;
    }
    const auto levels = part_levels(blob.size);
    std::array<std::uint64_t, 4> next{};  // the next part of each level
    Sha256 whole;
    std::uint64_t seen = 0;
    bool going = true;
    const auto walk = [&](const auto& self, std::span<const std::byte> list, std::uint8_t level) -> void {
        const auto count = list.size() / digest256_width;
        for (std::uint64_t at = 0; going && at < count; ++at) {
            const auto digest = digest_at(list, at);
            const auto address = part_address_of(digest);
            const auto part = journal_->replay(ExperienceAddress(memory_, view_of(address)));
            const auto& view = part.view();
            const auto place = next[level]++;
            const std::uint64_t expected =
                level == 0 ? std::min<std::uint64_t>(experience_part_bytes, blob.size - place * experience_part_bytes)
                           : std::min<std::uint64_t>(digests_per_part, levels.counts[level - 1] - place * digests_per_part) *
                                 digest256_width;
            if (view.kind != experience_part_kind || view.authority || !view.claim.empty() || view.index_count != 0 ||
                view.payload_digest != digest || view.payload.size() != expected)
                fail("experience_part_invalid");
            if (level == 0) {
                whole.update(view.payload);
                seen += expected;
                going = visit(view.payload);
            } else {
                self(self, view.payload, static_cast<std::uint8_t>(level - 1));
            }
        }
    };
    walk(walk, blob.top_digests, static_cast<std::uint8_t>(blob.depth - 1));
    if (going && (seen != blob.size || whole.finish() != blob.digest)) fail("experience_blob_digest_mismatch");
}

// A digest-set blob, streamed: strictly increasing 32-byte digests.
// SWEGCA: paper/swegca/journal_submission_2026-08-25/COMPONENT_LEDGER.md@5901a5a:44-50
void ExperienceRecord::for_each_digest(const ExperienceBlob& blob, DigestVisitor visit) const {
    std::optional<DigestBytes> previous;
    bool going = true;
    for_each_chunk(blob, [&](std::span<const std::byte> chunk) {
        for (std::uint64_t at = 0; going && at < chunk.size() / digest256_width; ++at) {
            const auto digest = digest_at(chunk, at);
            if (previous && !(*previous < digest)) fail("experience_root_set_invalid");
            previous = digest;
            going = visit(digest);
        }
        return going;
    });
}

// An original rests on its own source; a derived one on the root sources
// recorded when it was appended.
// SWEGCA: paper/swegca/journal_submission_2026-08-25/COMPONENT_LEDGER.md@5901a5a:44-50
void ExperienceRecord::for_each_root_source(DigestVisitor visit) const {
    if (!derived()) {
        const auto digest = text_digest(record_.view().source);
        (void)visit(digest);
        return;
    }
    for_each_digest(roots_, visit);
}

// An original's own context (none without one); a derived one's root
// contexts recorded when it was appended.
// SWEGCA: paper/swegca/journal_submission_2026-08-25/COMPONENT_LEDGER.md@5901a5a:44-50
void ExperienceRecord::for_each_root_context(DigestVisitor visit) const {
    if (!derived()) {
        if (context_) (void)visit(context_->bytes());
        return;
    }
    for_each_digest(contexts_, visit);
}

// SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:137-155
void ExperienceRecord::verify_parts() const {
    const auto whole = [](std::span<const std::byte>) { return true; };
    const auto every = [](const DigestBytes&) { return true; };
    if (raw_.parted()) for_each_chunk(raw_, whole);
    if (structured_.parted()) for_each_chunk(structured_, whole);
    if (roots_.parted()) for_each_digest(roots_, every);
    if (contexts_.parted()) for_each_digest(contexts_, every);
}

// Everything is checked, encoded and cut into parts here, before anything
// is staged; a failure stages nothing. An observation already in the
// journal (or earlier in `observations`) gets the existing address and no
// second record, and must carry the same index entries apart from the
// transaction; its parts are not appended.
// SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@cefdc3f:282-283
ExperienceAppend::ExperienceAppend(const ExperienceJournal& journal, const AllocationContext& memory,
                                   std::span<const Observation> observations, std::string_view operation_id,
                                   std::optional<std::string_view> transaction_id)
    : journal_(&journal), memory_(memory), observations_(observations), operation_id_(operation_id),
      transaction_id_(transaction_id), heads_(memory.allocator<Head>()), parts_(memory.allocator<Part>()),
      owned_(memory.allocator<LedgerBytes>()), addresses_(memory.allocator<ExperienceAddress>()) {
    detail::require_identity_text(operation_id, journal::OperationIdTag::name);
    if (transaction_id) detail::require_identity_text(*transaction_id, TransactionIdTag::name);
    const auto& store = journal.journal_;

    // Every field, with lineage and resources sorted.
    struct Sorted {
        LedgerVector<std::string_view> derived;
        LedgerVector<std::string_view> resources;
    };
    LedgerVector<Sorted> sorted(memory_.allocator<Sorted>());
    sorted.reserve(observations.size());
    for (const auto& observation : observations) {
        detail::require_identity_text(observation.source, ProducerIdTag::name);
        detail::require_identity_text(observation.source_revision, journal::SourceRevisionTag::name);
        if (observation.previous_revision_address)
            detail::require_identity_text(*observation.previous_revision_address, ExperienceAddressTag::name);
        if (observation.outcome)
            detail::require_identity_text(*observation.outcome, journal::OutcomeTextTag::name);
        if (observation.name_space) detail::require_identity_text(*observation.name_space, "name_space");
        if (observation.derived_from.size() > max_derived_from) fail("experience_lineage_too_long");
        if (observation.resources.size() > max_resources) fail("experience_too_many_resources");
        if (observation.semantic_cues.size() > max_semantic_cues) fail("experience_too_many_cues");
        LedgerVector<std::string_view> derived(observation.derived_from.begin(), observation.derived_from.end(),
                                               memory_.allocator<std::string_view>());
        std::sort(derived.begin(), derived.end());
        for (std::size_t at = 0; at < derived.size(); ++at) {
            detail::require_identity_text(derived[at], ExperienceAddressTag::name);
            if (at != 0 && derived[at - 1] == derived[at]) fail("experience_lineage_duplicate");
        }
        LedgerVector<std::string_view> resources(observation.resources.begin(), observation.resources.end(),
                                                 memory_.allocator<std::string_view>());
        std::sort(resources.begin(), resources.end());
        for (std::size_t at = 0; at < resources.size(); ++at) {
            detail::require_identity_text(resources[at], "resource");
            if (at != 0 && resources[at - 1] == resources[at]) fail("experience_resource_duplicate");
        }
        for (const auto cue : observation.semantic_cues)
            if (!is_single_token(memory_, cue)) fail("experience_cue_not_a_token");
        sorted.push_back(Sorted{std::move(derived), std::move(resources)});
    }

    // Lineage must already be published experience: each distinct address
    // is replayed and decoded once, and the root sources of each one some
    // observation derives from are kept with its root contexts (O(n log n),
    // one lookup each).
    struct Earlier {
        std::string_view address;
        bool derives = false;
        std::size_t first = 0;  // its root sources in `pool`
        std::size_t count = 0;
        std::size_t context_first = 0;  // its root contexts in `context_pool`
        std::size_t context_count = 0;
    };
    LedgerVector<Earlier> lineage(memory_.allocator<Earlier>());
    for (std::size_t at = 0; at < observations.size(); ++at) {
        if (observations[at].previous_revision_address)
            lineage.push_back(Earlier{*observations[at].previous_revision_address});
        for (const auto address : sorted[at].derived) lineage.push_back(Earlier{address, true});
    }
    std::sort(lineage.begin(), lineage.end(), [](const Earlier& left, const Earlier& right) {
        return left.address != right.address ? left.address < right.address : left.derives > right.derives;
    });
    lineage.erase(std::unique(lineage.begin(), lineage.end(),
                              [](const Earlier& left, const Earlier& right) { return left.address == right.address; }),
                  lineage.end());
    LedgerVector<DigestBytes> pool(memory_.allocator<DigestBytes>());
    LedgerVector<DigestBytes> context_pool(memory_.allocator<DigestBytes>());
    for (auto& earlier : lineage) {
        const ExperienceAddress address(memory_, earlier.address);
        if (!store.resolve(address)) fail("experience_lineage_unknown");
        const auto record = ExperienceRecord::decode(store.replay(address), memory_, store);
        if (!earlier.derives) continue;
        earlier.first = pool.size();
        record.for_each_root_source([&pool](const DigestBytes& digest) {
            pool.push_back(digest);
            return true;
        });
        earlier.count = pool.size() - earlier.first;
        earlier.context_first = context_pool.size();
        record.for_each_root_context([&context_pool](const DigestBytes& digest) {
            context_pool.push_back(digest);
            return true;
        });
        earlier.context_count = context_pool.size() - earlier.context_first;
    }
    const auto roots_of = [&lineage](std::string_view address) -> const Earlier& {
        return *std::lower_bound(lineage.begin(), lineage.end(), address,
                                 [](const Earlier& earlier, std::string_view key) { return earlier.address < key; });
    };

    // Each observation: its root sources, blobs, envelope, address and
    // index entries; its parts are kept aside until it is known to be new.
    LedgerVector<LedgerVector<PartSlice>> slices(memory_.allocator<LedgerVector<PartSlice>>());
    slices.reserve(observations.size());
    heads_.reserve(observations.size());
    addresses_.reserve(observations.size());
    for (std::size_t at = 0; at < observations.size(); ++at) {
        const auto& observation = observations[at];
        const auto& fields = sorted[at];
        LedgerVector<PartSlice> own(memory_.allocator<PartSlice>());
        const auto raw = plan_blob(memory_, observation.raw, own, owned_);
        const auto structured = plan_blob(memory_, observation.structured, own, owned_);
        std::optional<BlobPlan> roots;
        std::optional<BlobPlan> contexts;
        if (!fields.derived.empty()) {
            LedgerVector<DigestBytes> sources(memory_.allocator<DigestBytes>());
            LedgerVector<DigestBytes> context_set(memory_.allocator<DigestBytes>());
            for (const auto address : fields.derived) {
                const auto& earlier = roots_of(address);
                sources.insert(sources.end(), pool.begin() + static_cast<std::ptrdiff_t>(earlier.first),
                               pool.begin() + static_cast<std::ptrdiff_t>(earlier.first + earlier.count));
                context_set.insert(
                    context_set.end(), context_pool.begin() + static_cast<std::ptrdiff_t>(earlier.context_first),
                    context_pool.begin() + static_cast<std::ptrdiff_t>(earlier.context_first + earlier.context_count));
            }
            owned_.push_back(digest_set_bytes(memory_, sources));
            roots = plan_blob(memory_, BlobInput(std::span<const std::byte>(owned_.back())), own, owned_);
            owned_.push_back(digest_set_bytes(memory_, context_set));
            contexts = plan_blob(memory_, BlobInput(std::span<const std::byte>(owned_.back())), own, owned_);
        }
        auto payload = encode_envelope(memory_, observation, fields.derived, fields.resources, raw, structured,
                                       roots ? &*roots : nullptr, contexts ? &*contexts : nullptr);
        const auto kind = fields.derived.empty() ? original_experience_kind : derived_experience_kind;
        IndexEntries index(memory_);
        add_automatic(index, memory_,
                      IndexFields{observation.source, observation.source_revision,
                                  observation.previous_revision_address, fields.derived, observation.name_space,
                                  fields.resources, raw.digest, transaction_id});
        for (const auto cue : observation.semantic_cues) index.add_cue(cue);
        (void)index.finish();
        const auto address = address_of(kind, observation.source, observation.source_revision,
                                        observation.previous_revision_address, observation.outcome,
                                        Sha256::of(payload));
        addresses_.emplace_back(memory_, view_of(address));
        heads_.push_back(Head{kind, at, address, std::move(payload), index.take_entries(), index.take_bytes(), false,
                              std::nullopt});
        slices.push_back(std::move(own));
    }

    // Of equal addresses only the first is a candidate; one the journal
    // already holds is not appended. Both must agree on the index entries.
    LedgerVector<std::size_t> order(memory_.allocator<std::size_t>());
    order.reserve(heads_.size());
    for (std::size_t at = 0; at < heads_.size(); ++at) order.push_back(at);
    std::sort(order.begin(), order.end(), [this](std::size_t left, std::size_t right) {
        return heads_[left].address != heads_[right].address ? heads_[left].address < heads_[right].address
                                                             : left < right;
    });
    for (std::size_t rank = 0; rank < order.size(); ++rank) {
        auto& head = heads_[order[rank]];
        if (rank != 0 && heads_[order[rank - 1]].address == head.address) {
            if (!same_observed_index(heads_[order[rank - 1]].index, head.index)) fail("experience_index_conflict");
            head.skip = true;
            continue;
        }
        if (store.resolve(addresses_[order[rank]])) {
            const auto existing = ExperienceRecord::decode(store.replay(addresses_[order[rank]]), memory_, store);
            if (!same_observed_index(existing.index_entries(), head.index)) fail("experience_index_conflict");
            head.skip = true;
        }
    }

    // The parts of the experiences to append, each once.
    for (std::size_t at = 0; at < heads_.size(); ++at) {
        if (heads_[at].skip) continue;
        for (const auto& slice : slices[at])
            parts_.push_back(Part{slice.digest, part_address_of(slice.digest), slice.bytes, slice.reader,
                                  slice.offset, slice.length, false, std::nullopt});
    }
    std::sort(parts_.begin(), parts_.end());
    parts_.erase(std::unique(parts_.begin(), parts_.end()), parts_.end());
}

// SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@cefdc3f:282-283
journal::RecordDraft ExperienceAppend::head_draft(const Head& head) const {
    const auto& observation = observations_[head.observation];
    journal::RecordDraft draft;
    draft.kind = head.kind;
    draft.address = view_of(head.address);
    draft.source = observation.source;
    draft.source_revision = observation.source_revision;
    draft.previous_revision_address = observation.previous_revision_address;
    draft.outcome = observation.outcome;
    draft.operation_id = operation_id_;
    draft.transaction_id = transaction_id_;
    draft.index = head.index;
    draft.payload = head.payload;
    return draft;
}

// Parts first, as many per generation as the journal's limits take, then
// the experience records once every part is published. A generation is
// confirmed at the next call: unless every record it staged is in the
// journal (its publication failed or never happened), the cursor goes back
// to its start and it is staged again. The cursors move only after the
// journal staged, so a failure leaves them where they were. A part read
// through a reader is read into a buffer held until the journal has copied
// it, and must hash to what staging read (`experience_source_changed`).
// SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@cefdc3f:282-283
std::optional<journal::StagedGeneration> ExperienceAppend::next(const StateGeneration& state,
                                                                std::span<const journal::ViewGeneration> views) {
    if (done_) return std::nullopt;
    const auto& store = journal_->journal_;
    // Whether the part's address resolves. It is this part without reading it
    // only when the published record is the very record this append staged
    // (its record digest); anything else there (another writer's record,
    // published while ours was not) is replayed once and must be exactly the
    // part (kind, no authority, claim or index entry, the digest and length)
    // or fails `experience_part_invalid`. Confirming proves identity only:
    // byte integrity is checked where bytes are consumed (the chain digests
    // on replay, verify_parts before evidence, Re-evidence and Bind, the
    // whole digest when parts are streamed), so a part damaged on disk is
    // never counted or bound. Select reads only the envelope: it may still
    // name such an experience as a no-authority candidate (codex 17:10).
    const auto published_part = [&](Part& part) {
        const ExperienceAddress address(memory_, view_of(part.address));
        const auto position = store.resolve(address);
        if (!position) return false;
        if (part.known) return true;
        if (part.staged && position->record_digest == *part.staged) {
            part.known = true;
            return true;
        }
        const auto record = store.replay(address);
        const auto& view = record.view();
        const auto length = part.reader ? part.length : part.bytes.size();
        if (view.kind != experience_part_kind || view.authority || !view.claim.empty() ||
            view.index_count != 0 || view.payload_digest != part.digest || view.payload.size() != length)
            fail("experience_part_invalid");
        part.known = true;
        return true;
    };
    if (pending_ == 1) {
        for (auto at = pending_from_; at < next_part_; ++at)
            if (!published_part(parts_[at])) {
                next_part_ = pending_from_;
                break;
            }
    } else if (pending_ == 2) {
        // A head staged here is confirmed without reading only when the
        // record published at its address is the one staged; another
        // writer's record there must carry the same index entries
        // (`experience_index_conflict`), as a head appended meanwhile must.
        // As for parts, this confirms identity; reads check the bytes.
        for (auto at = pending_from_; at < next_head_; ++at) {
            auto& head = heads_[at];
            if (!head.staged) continue;  // skipped, or already in the journal when staged
            const auto& address = addresses_[head.observation];
            const auto position = store.resolve(address);
            if (!position) {
                next_head_ = pending_from_;
                break;
            }
            if (position->record_digest != *head.staged) {
                const auto existing = ExperienceRecord::decode(store.replay(address), memory_, store);
                if (!same_observed_index(existing.index_entries(), head.index))
                    fail("experience_index_conflict");
            }
            head.staged.reset();
        }
    }
    pending_ = 0;

    LedgerVector<journal::RecordDraft> drafts(memory_.allocator<journal::RecordDraft>());
    LedgerVector<LedgerBytes> read(memory_.allocator<LedgerBytes>());  // read parts, until staged
    GenerationBudget budget;
    auto part_at = next_part_;
    for (; part_at < parts_.size(); ++part_at) {
        auto& part = parts_[part_at];
        if (published_part(part)) continue;
        journal::RecordDraft draft;
        draft.kind = experience_part_kind;
        draft.address = view_of(part.address);
        draft.source = part_source;
        draft.source_revision = part_source_revision;
        draft.operation_id = operation_id_;
        draft.transaction_id = transaction_id_;
        draft.payload = part.bytes;
        if (part.reader) {
            LedgerBytes bytes(static_cast<std::size_t>(part.length), std::byte{}, memory_.allocator<std::byte>());
            (*part.reader)(part.offset, bytes);
            if (Sha256::of(bytes) != part.digest) fail("experience_source_changed");
            read.push_back(std::move(bytes));  // a moved vector keeps its buffer
            draft.payload = read.back();
        }
        if (!budget.add(draft)) {
            if (drafts.empty()) fail("experience_record_too_large");
            if (part.reader) read.pop_back();
            break;
        }
        drafts.push_back(draft);
    }
    if (!drafts.empty()) {
        auto staged = store.stage_records(drafts, state, views);
        // Staged, not published: another writer may publish a record under
        // one of these addresses first and this generation then fail, so a
        // part is known only once the record published there is the one
        // staged here (published_part). Drafts and positions share order; a
        // part already published was skipped above, so they match one to one.
        const auto positions = staged.positions();
        std::size_t drafted = 0;
        for (auto at = next_part_; at < part_at; ++at)
            if (!parts_[at].known) parts_[at].staged = positions[drafted++].record_digest;
        pending_ = 1;
        pending_from_ = next_part_;
        next_part_ = part_at;
        return staged;
    }
    next_part_ = part_at;
    if (!parts_checked_) {
        for (auto& part : parts_)
            if (!published_part(part)) fail("experience_parts_unpublished");
        parts_checked_ = true;
    }
    LedgerVector<std::size_t> drafted(memory_.allocator<std::size_t>());  // heads with a draft
    auto head_at = next_head_;
    for (; head_at < heads_.size(); ++head_at) {
        const auto& head = heads_[head_at];
        if (head.skip) continue;
        const auto& address = addresses_[head.observation];
        if (store.resolve(address)) {  // appended meanwhile
            const auto existing = ExperienceRecord::decode(store.replay(address), memory_, store);
            if (!same_observed_index(existing.index_entries(), head.index)) fail("experience_index_conflict");
            continue;
        }
        const auto draft = head_draft(head);
        if (!budget.add(draft)) {
            if (drafts.empty()) fail("experience_record_too_large");
            break;
        }
        drafts.push_back(draft);
        drafted.push_back(head_at);
    }
    if (drafts.empty()) {
        next_head_ = head_at;
        done_ = true;
        return std::nullopt;
    }
    auto staged = store.stage_records(drafts, state, views);
    const auto positions = staged.positions();  // in draft order
    for (std::size_t at = 0; at < drafted.size(); ++at)
        heads_[drafted[at]].staged = positions[at].record_digest;
    pending_ = 2;
    pending_from_ = next_head_;
    next_head_ = head_at;
    return staged;
}

// SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@cefdc3f:282-283
ExperienceAppend ExperienceJournal::stage(std::span<const Observation> observations, std::string_view operation_id,
                                          std::optional<std::string_view> transaction_id) const {
    return ExperienceAppend(*this, memory_, observations, operation_id, transaction_id);
}

// SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:137-155
ExperienceRecord ExperienceJournal::replay(const ExperienceAddress& address) const {
    return ExperienceRecord::decode(journal_.replay(address), memory_, journal_);
}

// Each view's key becomes its entry value exactly as `add_automatic` wrote
// it: a cue token as its cue entry, a text by its SHA-256, a digest or an
// address as itself.
// SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@cefdc3f:122-123
void ExperienceJournal::for_each_in_view(ExperienceView view, std::string_view key,
                                         journal::IndexVisitor visit) const {
    const auto kind = static_cast<char>(view);
    switch (view) {
    case ExperienceView::cue:
        if (!is_single_token(memory_, key)) fail("experience_view_key_invalid");
        with_cue_entry(key, [&](char entry_kind, std::string_view value) {
            journal_.for_each_index_match(entry_kind, value, visit);
        });
        return;
    case ExperienceView::source:
    case ExperienceView::name_space:
    case ExperienceView::resource:
    case ExperienceView::transaction: {
        if (!detail::is_identity_text(key)) fail("experience_view_key_invalid");
        const auto hex = hex_of(text_digest(key));
        journal_.for_each_index_match(kind, std::string_view(hex.data(), hex.size()), visit);
        return;
    }
    case ExperienceView::content:
        if (!is_hex_digest(key)) fail("experience_view_key_invalid");
        journal_.for_each_index_match(kind, key, visit);
        return;
    case ExperienceView::lineage:
    case ExperienceView::successor:
        if (!is_experience_address(key)) fail("experience_view_key_invalid");
        journal_.for_each_index_match(kind, key, visit);
        return;
    }
    fail("experience_view_key_invalid");
}

// SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:179-205
void VerdictSink::record(const CandidateVerdict& verdict) {
    if (judgment_) fail("experience_judgment_repeated");
    if (verdict.address != candidate_.address) fail("experience_judgment_address_changed");
    LedgerVector<EvidenceText> evidence(memory_.allocator<EvidenceText>());
    evidence.reserve(verdict.rejection_evidence.size());
    for (const auto item : verdict.rejection_evidence) evidence.emplace_back(memory_, item);
    judgment_.emplace(CandidateJudgment{ExperienceAddress(memory_, verdict.address),
                                        candidate_.position,
                                        candidate_.matched_cues,
                                        verdict.selected,
                                        unit_fraction(verdict.relevance, "experience_relevance_invalid"),
                                        unit_fraction(verdict.contradiction, "experience_contradiction_invalid"),
                                        VerificationState(memory_, verdict.verification_state),
                                        RevisionText(memory_, verdict.revision),
                                        Rationale(memory_, verdict.rationale),
                                        std::move(evidence)});
}

// SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:208-243
SelectionReceipt<NoAuthority>::SelectionReceipt(QueryText query, const Digest256& context,
                                                const SelectionUniverse& universe,
                                                LedgerVector<CandidateJudgment> judgments,
                                                LedgerVector<SelectedExperience> selected)
    : query_(std::move(query)), context_(context), universe_(universe),
      judgments_(std::move(judgments)), selected_(std::move(selected)),
      digest_(compute_digest()) {}

// Covers the query, context, universe, method, rationale, every judgment,
// every selected experience in order, and the authority flags.
// SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:274-290
Digest256 SelectionReceipt<NoAuthority>::compute_digest() const {
    Sha256 hash;
    hash_field(hash, "swegca.selection_receipt.v3");
    hash_field(hash, query_.value());
    hash.update(context_.bytes());
    hash_u64(hash, universe_.generation);
    hash.update(universe_.manifest_digest);
    hash_u64(hash, universe_.record_count);
    hash_u64(hash, universe_.record_bytes);
    hash_field(hash, method());
    hash_field(hash, rationale());
    hash_u64(hash, judgments_.size());
    for (const auto& item : judgments_) {
        hash_field(hash, item.address.value());
        hash_u64(hash, item.position.segment_ordinal);
        hash_u64(hash, item.position.byte_offset);
        hash_u64(hash, item.position.sequence);
        hash.update(item.position.record_digest);
        hash_u64(hash, item.matched_cues);
        hash_u64(hash, item.selected ? 1 : 0);
        hash_u64(hash, std::bit_cast<std::uint64_t>(item.relevance));
        hash_u64(hash, std::bit_cast<std::uint64_t>(item.contradiction));
        hash_field(hash, item.verification_state.value());
        hash_field(hash, item.revision.value());
        hash_field(hash, item.rationale.value());
        hash_u64(hash, item.rejection_evidence.size());
        for (const auto& evidence : item.rejection_evidence) hash_field(hash, evidence.value());
    }
    hash_u64(hash, selected_.size());
    for (const auto& item : selected_) {
        hash_field(hash, item.address.value());
        hash_u64(hash, item.position.sequence);
        hash_u64(hash, item.byte_count);
        hash.update(item.raw_digest);
        hash.update(item.record_digest);
        hash_u64(hash, item.raw_parted ? 1 : 0);
        hash_field(hash, item.verification_state.value());
        hash_field(hash, item.revision.value());
    }
    for (const bool flag : {external_action_authorized, memory_write_authorized, world_write_authorized,
                            training_write_authorized, p3_promotion_authorized})
        hash_u64(hash, flag ? 1 : 0);
    return Digest256(hash.finish());
}

// U is read first; retrieval keeps only entries of records in U (the index
// view may already hold later ones). Every retrieved entry is kept until the
// policy bound, which fails closed instead of cutting.
// SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:475-537
SelectionReceipt<NoAuthority> ExperienceSelector::select(const SelectionQuery& query,
                                                         SelectionJudge judge) const {
    const auto& memory = experience_.memory_;
    const auto& journal = experience_.journal_;
    QueryText text(memory, query.text);
    const auto universe = journal.universe();

    CueTokens tokens(memory, query.text);
    LedgerVector<std::string_view> cues(tokens.tokens().begin(), tokens.tokens().end(),
                                        memory.allocator<std::string_view>());
    std::sort(cues.begin(), cues.end());
    cues.erase(std::unique(cues.begin(), cues.end()), cues.end());

    struct Hit {
        std::size_t key = 0;  // offset of the address in `keys`
        std::size_t size = 0;
        journal::RecordPosition position;
    };
    LedgerBytes keys(memory.allocator<std::byte>());
    LedgerVector<Hit> hits(memory.allocator<Hit>());
    for (const auto cue : cues) {
        with_cue_entry(cue, [&](char kind, std::string_view value) {
            journal.for_each_index_match(kind, value, [&](std::string_view address,
                                                          const journal::RecordPosition& position) {
                if (position.sequence > universe.record_count) return true;  // after U
                if (hits.size() >= policy_.max_retrieved) fail("experience_select_over_policy");
                const auto at = keys.size();
                const auto* bytes = reinterpret_cast<const std::byte*>(address.data());
                keys.insert(keys.end(), bytes, bytes + address.size());
                hits.push_back(Hit{at, address.size(), position});
                return true;
            });
        });
    }
    const auto key_of = [&keys](const Hit& hit) {
        return std::string_view(reinterpret_cast<const char*>(keys.data()) + hit.key, hit.size);
    };
    std::sort(hits.begin(), hits.end(),
              [&](const Hit& left, const Hit& right) { return key_of(left) < key_of(right); });

    LedgerVector<CandidateJudgment> judgments(memory.allocator<CandidateJudgment>());
    LedgerVector<SelectedExperience> selected(memory.allocator<SelectedExperience>());
    for (std::size_t first = 0; first < hits.size();) {
        std::size_t last = first + 1;
        while (last < hits.size() && key_of(hits[last]) == key_of(hits[first])) ++last;
        const auto address = key_of(hits[first]);
        const auto& position = hits[first].position;
        for (std::size_t same = first + 1; same < last; ++same)
            if (hits[same].position.sequence != position.sequence ||
                hits[same].position.record_digest != position.record_digest)
                fail("experience_select_index_inconsistent");
        const SelectionCandidate candidate{address, position, static_cast<std::uint32_t>(last - first)};
        VerdictSink sink(memory, candidate);
        judge(candidate, query, sink);
        if (!sink.judgment_) fail("experience_judgment_missing");
        auto judgment = std::move(*sink.judgment_);
        if (judgment.selected) {
            const auto record = experience_.replay(judgment.address);
            if (record.position().sequence != position.sequence ||
                record.record().record_digest != position.record_digest)
                fail("experience_select_position_changed");
            selected.push_back(SelectedExperience{ExperienceAddress(memory, address), position,
                                                  record.raw_blob().size, record.raw_digest(),
                                                  position.record_digest, record.raw_blob().parted(),
                                                  judgment.verification_state, judgment.revision});
        }
        judgments.push_back(std::move(judgment));
        first = last;
    }
    if (judgments.empty()) fail("experience_select_no_candidates");
    if (selected.empty()) fail("experience_select_nothing_selected");
    return SelectionReceipt<NoAuthority>(std::move(text), query.context, universe,
                                         std::move(judgments), std::move(selected));
}

}  // namespace swegca::architecture
