#include "swegca_architecture/experience.hpp"

#include "swegca_architecture/sha256.hpp"
#include "swegca_architecture/unicode_casefold.hpp"

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
constexpr std::uint16_t envelope_version = 4;
// magic, version, observed step, uncertainty and contradiction bits, span
// flag, offset and length, context flag and digest, namespace flag, lineage
// count, resource count (a present namespace, the lineage, the resources
// each with its listed flag, the blobs and the typed section follow).
constexpr std::size_t envelope_fixed_bytes = 4 + 2 + 8 + 8 + 8 + 1 + 8 + 8 + 1 + 32 + 1 + 4 + 4;
constexpr std::size_t max_derived_from = 1024;
constexpr std::size_t max_resources = 1024;
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
// Every experience's envelope fits one record whatever its bytes: five
// blobs (raw, structured, root sources, root contexts, and the typed section
// after its flag) at their largest encoding, and the namespace, lineage and
// flagged resources at their limits.
static_assert(envelope_fixed_bytes + 4 + detail::identity_text_max_bytes +
                      max_derived_from * (4 + address_bytes) +
                      max_resources * (4 + detail::identity_text_max_bytes + 1) + 1 + 5 * max_blob_encoded_bytes <=
                  journal::max_payload_bytes,
              "an experience's envelope must always fit one record");
// A part record's producer and revision: the part belongs to this module,
// and equal bytes are one part whoever observed them.
constexpr std::string_view part_source = "experience";
constexpr std::string_view part_source_revision = "part.v1";
// A cue entry keeps its token when its key (kind, token, separator,
// address) is an identity text under the address of the record carrying
// it; a longer token is kept by its digest ('h'). A memory keeps tokens up
// to the first bound (as it always has), a cue binding, whose address is
// longer, up to the second; a lookup of a token between them asks both.
constexpr std::size_t max_inline_cue_bytes = detail::identity_text_max_bytes - 2 - address_bytes;
constexpr std::size_t max_inline_bound_cue_bytes = detail::identity_text_max_bytes - 2 - cue_binding_address_bytes;
// A cue binding's payload: magic, version, target, the target record's
// exact position (segment ordinal, byte offset, sequence, record digest:
// where it is, so rebuild reads it exactly and checks it against the
// address view it rebuilt), cue count (the cues follow).
constexpr std::array<std::byte, 4> binding_magic{std::byte{'S'}, std::byte{'W'}, std::byte{'X'},
                                                 std::byte{'C'}};
constexpr std::uint16_t binding_version = 1;
static_assert(4 + 2 + 4 + address_bytes + 3 * 8 + 32 + 4 + max_bound_cues * (4 + detail::identity_text_max_bytes) <=
                  journal::max_payload_bytes,
              "a cue binding must always fit one record");
static_assert(max_bound_cues <= journal::max_record_index_entries,
              "a cue binding's index entries must always fit one record");
// Every entry an experience can carry fits one record: the tokens of the
// source and of its revision (at most one per byte), lineage, resources,
// and source, content, revised address, namespace and transaction.
static_assert(2 * detail::identity_text_max_bytes + max_derived_from + max_resources + 5 <=
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

// Whitespace of the bound-cue rule: what Python's str.split and `\s` treat
// as space (ASCII controls 09-0D and 1C-1F, space, U+0085, U+00A0, U+1680,
// U+2000-200A, U+2028, U+2029, U+202F, U+205F, U+3000), listed here as this
// module's own rule.
// SWEGCA: src/tinylm_slicer/mosaic_memory_activation.py@3bddcb7:34-35
bool is_cue_space(char32_t value) noexcept {
    return (value >= 0x09 && value <= 0x0d) || (value >= 0x1c && value <= 0x20) || value == 0x85 ||
           value == 0xa0 || value == 0x1680 || (value >= 0x2000 && value <= 0x200a) || value == 0x2028 ||
           value == 0x2029 || value == 0x202f || value == 0x205f || value == 0x3000;
}

// The user's `_text` test: strict UTF-8 holding a code point that Python's
// str.strip keeps (its whitespace is the set above). The text itself is kept
// as given, as the user's steps keep it.
// SWEGCA: src/tinylm_slicer/mosaic_memory_activation.py@3bddcb7:27-31
bool is_step_text(std::string_view text) noexcept {
    if (!detail::is_strict_utf8(text)) return false;
    for (std::size_t at = 0; at < text.size();)
        if (!is_cue_space(decode_at(text, at))) return true;
    return false;
}

// Strict UTF-8 fed in pieces that may cut a code point: counts code points
// (Python's len of the text the user's text anchor spans) and stays valid only
// while every completed sequence is strict UTF-8. Streaming it part by part is
// C++ infrastructure under the approved hard memory limit (:91-92).
// SWEGCA: src/tinylm_slicer/mosaic_semantic_encoding.py@3bddcb7:109-112
// SWEGCA: user@2026-09-22:91-92
class Utf8Count final {
public:
    // SWEGCA: user@2026-09-22:91-92
    void feed(std::span<const std::byte> chunk) noexcept {
        std::size_t at = 0;
        if (held_ != 0) {
            const auto need = sequence_width(carry_[0]) - held_;
            while (at < chunk.size() && at < need) carry_[held_++] = static_cast<char>(chunk[at++]);
            if (held_ < sequence_width(carry_[0])) return;
            const std::string_view sequence(carry_.data(), held_);
            valid_ = valid_ && detail::is_strict_utf8(sequence);
            if (valid_ && !nonspace_) {
                std::size_t from = 0;
                nonspace_ = !is_cue_space(decode_at(sequence, from));
            }
            ++count_;
            held_ = 0;
        }
        auto end = chunk.size();
        for (std::size_t back = 1; back <= 3 && back <= end - at; ++back) {
            const auto byte = static_cast<char>(chunk[end - back]);
            if ((static_cast<unsigned char>(byte) & 0xc0) == 0x80) continue;
            if (sequence_width(byte) > back) {
                for (auto from = end - back; from < end; ++from) carry_[held_++] = static_cast<char>(chunk[from]);
                end -= back;
            }
            break;
        }
        const std::string_view body(reinterpret_cast<const char*>(chunk.data()) + at, end - at);
        valid_ = valid_ && detail::is_strict_utf8(body);
        for (const char byte : body) count_ += (static_cast<unsigned char>(byte) & 0xc0) != 0x80 ? 1 : 0;
        for (std::size_t from = 0; valid_ && !nonspace_ && from < body.size();)
            nonspace_ = !is_cue_space(decode_at(body, from));
    }

    // SWEGCA: user@2026-09-22:91-92
    [[nodiscard]] bool valid() const noexcept { return valid_ && held_ == 0; }
    // SWEGCA: user@2026-09-22:91-92
    [[nodiscard]] std::uint64_t count() const noexcept { return count_; }
    // Whether a code point Python's str.strip keeps was seen (the user's
    // `_text` test, streamed).
    // SWEGCA: src/tinylm_slicer/mosaic_memory_activation.py@3bddcb7:27-31
    [[nodiscard]] bool nonspace() const noexcept { return nonspace_; }

private:
    // SWEGCA: user@2026-09-22:91-92
    static std::size_t sequence_width(char lead) noexcept {
        const auto value = static_cast<unsigned char>(lead);
        return value < 0xc0 ? 1 : value < 0xe0 ? 2 : value < 0xf0 ? 3 : value < 0xf8 ? 4 : 1;
    }

    std::array<char, 4> carry_{};
    std::size_t held_ = 0;
    std::uint64_t count_ = 0;
    bool valid_ = true;
    bool nonspace_ = false;
};

// The exact 128-bit product of two u64 values (high word, low word). It keeps
// the user's audio binding frames * 10**9 // sample_rate exact: Python
// integers are unbounded, a u64 product is not.
// SWEGCA: src/tinylm_slicer/mosaic_media_atoms.py@3bddcb7:163-164
struct WideProduct {
    std::uint64_t high = 0;
    std::uint64_t low = 0;
    auto operator<=>(const WideProduct&) const = default;
};

// SWEGCA: src/tinylm_slicer/mosaic_media_atoms.py@3bddcb7:163-164
WideProduct wide_product(std::uint64_t left, std::uint64_t right) noexcept {
    const std::uint64_t mask = 0xffffffffu;
    const std::uint64_t ll = (left & mask) * (right & mask);
    const std::uint64_t lh = (left & mask) * (right >> 32);
    const std::uint64_t hl = (left >> 32) * (right & mask);
    const std::uint64_t hh = (left >> 32) * (right >> 32);
    const std::uint64_t middle = (ll >> 32) + (lh & mask) + (hl & mask);
    return WideProduct{hh + (lh >> 32) + (hl >> 32) + (middle >> 32), (middle << 32) | (ll & mask)};
}

// A resource descriptor in its one form: the fields of its modality within
// their bounds (the user's anchors and media extents), every other field zero.
// Audio has the user's two forms: the anchor's duration alone, or a waveform
// whose duration is bound to its frames and sample rate.
// SWEGCA: src/tinylm_slicer/mosaic_semantic_encoding.py@3bddcb7:109-136
// SWEGCA: src/tinylm_slicer/mosaic_media_atoms.py@3bddcb7:20-71
// SWEGCA: src/tinylm_slicer/mosaic_media_atoms.py@3bddcb7:160-166
bool valid_descriptor(const ResourceDescriptor& d) noexcept {
    const bool no_size = d.width == 0 && d.height == 0 && d.frame == CoordinateFrame::native_source_pixels;
    const bool no_samples = d.frames == 0 && d.sample_rate == 0;
    switch (d.modality) {
    case ResourceModality::text:
        return no_size && no_samples && d.duration_ns == 0;
    case ResourceModality::image:
        return d.width > 0 && d.height > 0 && d.code_points == 0 && no_samples && d.duration_ns == 0 &&
               (d.frame == CoordinateFrame::native_source_pixels || d.frame == CoordinateFrame::oriented_source_pixels);
    case ResourceModality::audio: {
        if (d.code_points != 0 || !no_size) return false;
        if (no_samples) return true;
        if (d.frames == 0 || d.sample_rate == 0) return false;
        // duration_ns = frames * 10^9 // sample_rate (media_atoms :163-164),
        // exactly: duration * rate <= frames * 10^9 < (duration + 1) * rate.
        const auto samples = wide_product(d.frames, 1'000'000'000u);
        const auto below = wide_product(d.duration_ns, d.sample_rate);
        auto above = below;
        above.low += d.sample_rate;
        above.high += above.low < d.sample_rate ? 1 : 0;
        return below <= samples && samples < above;
    }
    case ResourceModality::video:
        return d.width > 0 && d.height > 0 && d.code_points == 0 && no_samples &&
               (d.frame == CoordinateFrame::native_source_pixels || d.frame == CoordinateFrame::oriented_source_pixels);
    }
    return false;
}

// The full case folding of one code point (Unicode status C and F, the
// folding Python's str.casefold applies), or empty when it folds to itself.
// SWEGCA: src/tinylm_slicer/mosaic_memory_activation.py@3bddcb7:34-35
std::span<const char32_t> case_fold(char32_t value) noexcept {
    const auto& table = detail::case_foldings;
    const auto found = std::lower_bound(table.begin(), table.end(), value,
                                        [](const detail::CaseFolding& row, char32_t key) {
                                            return row.from < key;
                                        });
    if (found == table.end() || found->from != value) return {};
    return std::span<const char32_t>(found->to.data(), found->count);
}

// Appends one code point as UTF-8.
// SWEGCA: src/tinylm_slicer/mosaic_memory_activation.py@3bddcb7:34-35
void append_utf8(LedgerBytes& out, char32_t value) {
    const auto put = [&out](std::uint32_t byte) { out.push_back(static_cast<std::byte>(byte)); };
    if (value < 0x80) {
        put(value);
    } else if (value < 0x800) {
        put(0xc0 | (value >> 6));
        put(0x80 | (value & 0x3f));
    } else if (value < 0x10000) {
        put(0xe0 | (value >> 12));
        put(0x80 | ((value >> 6) & 0x3f));
        put(0x80 | (value & 0x3f));
    } else {
        put(0xf0 | (value >> 18));
        put(0x80 | ((value >> 12) & 0x3f));
        put(0x80 | ((value >> 6) & 0x3f));
        put(0x80 | (value & 0x3f));
    }
}

// A bound cue as the user's rule keeps it (`_cue`): the whole phrase,
// stripped, each run of whitespace one space, then case folded in full by
// the Unicode table (user 2026-09-23 「유니코드 casefold 그대로」; the
// table is Unicode 16.0.0, the host python3's). Empty when nothing is left.
// SWEGCA: src/tinylm_slicer/mosaic_memory_activation.py@3bddcb7:34-35
LedgerBytes normalized_cue(const AllocationContext& memory, std::string_view text) {
    if (!detail::is_strict_utf8(text)) fail("cue_text_not_utf8");
    LedgerBytes out(memory.allocator<std::byte>());
    out.reserve(text.size());
    bool space = false;
    for (std::size_t at = 0; at < text.size();) {
        const auto begin = at;
        const auto value = decode_at(text, at);
        if (is_cue_space(value)) {
            space = !out.empty();
            continue;
        }
        if (space) out.push_back(std::byte{' '});
        space = false;
        const auto folded = case_fold(value);
        if (!folded.empty()) {
            for (const auto each : folded) append_utf8(out, each);
            continue;
        }
        const auto* bytes = reinterpret_cast<const std::byte*>(text.data() + begin);
        out.insert(out.end(), bytes, bytes + (at - begin));
    }
    return out;
}

// SWEGCA: src/tinylm_slicer/mosaic_memory_activation.py@3bddcb7:34-35
std::string_view text_of(const LedgerBytes& bytes) noexcept {
    return std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

// Whether `text` is a bound cue already in its kept form.
// SWEGCA: src/tinylm_slicer/mosaic_memory_activation.py@3bddcb7:34-35
bool is_normalized_cue(const AllocationContext& memory, std::string_view text) {
    return detail::is_identity_text(text) && text_of(normalized_cue(memory, text)) == text;
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


// The cue-view entry of one token in a record whose address is
// `address_size` bytes: the token itself, or its digest when its key would
// not be an identity text.
// SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:293-339
template <class Visit>
void with_cue_entry(std::string_view token, std::size_t address_size, Visit&& visit) {
    if (token.size() + 2 + address_size <= detail::identity_text_max_bytes) {
        visit('c', token);
        return;
    }
    const auto hex = hex_of(text_digest(token));
    visit('h', std::string_view(hex.data(), hex.size()));
}

// Every entry a lookup of one token asks: the token for memories and
// bindings alike while both keep it inline, both forms between the bounds,
// the digest past them.
// SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:293-339
template <class Visit>
void with_cue_lookup(std::string_view token, Visit&& visit) {
    if (token.size() <= max_inline_bound_cue_bytes) {
        visit('c', token);
        return;
    }
    const auto hex = hex_of(text_digest(token));
    if (token.size() <= max_inline_cue_bytes) visit('c', token);
    visit('h', std::string_view(hex.data(), hex.size()));
}

// The index entries of one experience, copied into one charged buffer and
// viewed, sorted and unique, once complete.
class IndexEntries final {
public:
    // C++ infrastructure for the derived addresses (approved flow :62);
    // no direct Python counterpart.
    // SWEGCA: user@2026-09-22:62
    explicit IndexEntries(const AllocationContext& memory)
        : bytes_(memory.allocator<std::byte>()),
          spans_(memory.allocator<std::pair<std::size_t, std::size_t>>()),
          entries_(memory.allocator<std::string_view>()) {}
    IndexEntries(IndexEntries&&) noexcept = default;
    IndexEntries& operator=(IndexEntries&&) = delete;
    IndexEntries(const IndexEntries&) = delete;
    IndexEntries& operator=(const IndexEntries&) = delete;
    ~IndexEntries() = default;

    // SWEGCA: user@2026-09-22:62
    void add(char kind, std::string_view value) {
        const auto at = bytes_.size();
        bytes_.push_back(static_cast<std::byte>(kind));
        const auto* data = reinterpret_cast<const std::byte*>(value.data());
        bytes_.insert(bytes_.end(), data, data + value.size());
        spans_.emplace_back(at, value.size() + 1);
    }

    // SWEGCA: user@2026-09-22:62
    void add_digest(char kind, const DigestBytes& digest) {
        const auto hex = hex_of(digest);
        add(kind, std::string_view(hex.data(), hex.size()));
    }

    // SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:293-339
    void add_cue(std::string_view token, std::size_t address_size) {
        with_cue_entry(token, address_size, [this](char kind, std::string_view value) { add(kind, value); });
    }

    // Views every entry, sorted and unique; nothing is added after this.
    // Weak: the user's postings are sorted per key; sorting and dropping
    // repeats of whole entries is C++'s.
    // SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:428-430
    std::span<const std::string_view> finish() {
        entries_.reserve(spans_.size());
        for (const auto& [at, size] : spans_)
            entries_.emplace_back(reinterpret_cast<const char*>(bytes_.data()) + at, size);
        std::sort(entries_.begin(), entries_.end());
        entries_.erase(std::unique(entries_.begin(), entries_.end()), entries_.end());
        return entries_;
    }

    // SWEGCA: user@2026-09-22:62
    [[nodiscard]] std::span<const std::string_view> entries() const noexcept { return entries_; }

    // Hand the finished entries and the bytes they view to a new owner. Both
    // are moved by construction, which keeps each buffer, so the views stay
    // valid; this object is empty afterwards.
    // SWEGCA: user@2026-09-22:62
    [[nodiscard]] LedgerVector<std::string_view> take_entries() noexcept { return std::move(entries_); }
    // SWEGCA: user@2026-09-22:62
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
// The user's automatic keys: derived from the observation, never supplied;
// here from its source, revision and fields rather than a file path.
// SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:410-431
void add_automatic(IndexEntries& index, const AllocationContext& memory, const IndexFields& fields) {
    for (const auto text : {fields.source, fields.source_revision}) {
        const CueTokens tokens(memory, text);
        for (const auto token : tokens.tokens()) index.add_cue(token, address_bytes);
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
// C++ infrastructure for admitting an observation once (approved flow :61);
// no direct Python counterpart.
// SWEGCA: user@2026-09-22:61
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

// The record identity an address is the digest of: kind, source, revision,
// revised address, outcome and payload digest. Index entries are derived
// from these fields, never identity.
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

// A cue binding's address: its memory's address, the infix, and the digest
// of everything it binds (target, author, revision and the distinct cues in
// increasing order), so equal bindings are one record.
// SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:31-35
std::array<char, cue_binding_address_bytes> binding_address_of(std::string_view target, std::string_view source,
                                                               std::string_view source_revision,
                                                               std::span<const std::string_view> cues) {
    Sha256 hash;
    hash_field(hash, "swegca.cue_binding.v1");
    hash_u64(hash, cue_binding_kind);
    hash_field(hash, target);
    hash_field(hash, source);
    hash_field(hash, source_revision);
    hash_u64(hash, cues.size());
    for (const auto cue : cues) hash_field(hash, cue);
    const auto hex = hex_of(hash.finish());
    std::array<char, cue_binding_address_bytes> out{};
    auto at = std::copy(target.begin(), target.end(), out.begin());
    at = std::copy(cue_binding_infix.begin(), cue_binding_infix.end(), at);
    std::copy(hex.begin(), hex.end(), at);
    return out;
}

// SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:398-436
LedgerBytes encode_binding(const AllocationContext& memory, std::string_view target,
                           const journal::RecordPosition& at, std::span<const std::string_view> cues) {
    LedgerBytes out(memory.allocator<std::byte>());
    ByteWriter writer(out);
    writer.raw(binding_magic);
    writer.u16(binding_version);
    writer.text(target, detail::identity_text_max_bytes);
    writer.u64(at.segment_ordinal);
    writer.u64(at.byte_offset);
    writer.u64(at.sequence);
    writer.digest(at.record_digest);
    writer.u32(static_cast<std::uint32_t>(cues.size()));
    for (const auto cue : cues) writer.text(cue, detail::identity_text_max_bytes);
    return out;
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
// blob small enough to be inline is read into `owned`. `text`, when given, is
// fed every byte once as it is hashed.
// SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:23-60
BlobPlan plan_blob(const AllocationContext& memory, const BlobInput& input,
                   LedgerVector<PartSlice>& parts, LedgerVector<LedgerBytes>& owned, Utf8Count* text = nullptr) {
    BlobPlan out;
    out.size = input.size;
    auto bytes = input.bytes;
    if (!input.reader && bytes.size() != input.size) fail("experience_blob_size_mismatch");
    if (input.reader && input.size <= experience_inline_blob_bytes) {
        LedgerBytes copy(static_cast<std::size_t>(input.size), std::byte{}, memory.allocator<std::byte>());
        (*input.reader)(0, copy);
        owned.push_back(std::move(copy));
        bytes = owned.back();
    }
    if (out.size <= experience_inline_blob_bytes) {
        if (text) text->feed(bytes);
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
            if (text) text->feed(slice);
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
            if (text) text->feed(slice);
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

// A part of a parted blob, replayed and checked: a part record with no
// authority, claim or index entry, its payload the digest its parent names
// and exactly `expected` bytes long.
// SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:137-155
journal::PublishedRecord replay_part(const journal::JournalStore& journal, const AllocationContext& memory,
                                     const DigestBytes& digest, std::uint64_t expected) {
    const auto address = part_address_of(digest);
    auto part = journal.replay(ExperienceAddress(memory, view_of(address)));
    const auto& view = part.view();
    if (view.kind != experience_part_kind || view.authority || !view.claim.empty() || view.index_count != 0 ||
        view.payload_digest != digest || view.payload.size() != expected)
        fail("experience_part_invalid");
    return part;
}

// The length of part `place` of `level` in a parted blob of `size` bytes: a
// full part, or what is left for the last one of its level.
// SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:23-60
std::uint64_t part_length(const PartLevels& levels, std::uint64_t size, std::uint8_t level,
                          std::uint64_t place) noexcept {
    if (level == 0) return std::min<std::uint64_t>(experience_part_bytes, size - place * experience_part_bytes);
    return std::min<std::uint64_t>(digests_per_part, levels.counts[level - 1] - place * digests_per_part) *
           digest256_width;
}

// Reads a blob forward: an inline one from the record, a parted one part by
// part, holding one part per level (each replayed and checked by
// `replay_part`); `finish` checks, once every byte was read, the whole
// blob's digest. A read past the end fails with `experience_section_invalid`.
// C++ infrastructure for reading blobs larger than memory holds (approved
// flow :91-92); no direct Python counterpart.
// SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:137-155
// SWEGCA: user@2026-09-22:91-92
class BlobCursor final {
public:
    // SWEGCA: user@2026-09-22:91-92
    BlobCursor(const journal::JournalStore& journal, const AllocationContext& memory, const ExperienceBlob& blob)
        : journal_(&journal), memory_(memory), blob_(blob), levels_(part_levels(blob.size)) {
        if (!blob.parted()) {
            chunk_ = blob.inline_bytes;
            return;
        }
        lists_[blob.depth - 1].list = blob.top_digests;
    }

    // SWEGCA: user@2026-09-22:91-92
    [[nodiscard]] std::uint64_t remaining() const noexcept { return blob_.size - read_; }
    // Whether what `next` returns stays valid after the next read (an inline
    // blob's bytes are the record's; a part is released when passed).
    // SWEGCA: user@2026-09-22:91-92
    [[nodiscard]] bool stable() const noexcept { return !blob_.parted(); }

    // Up to `limit` of the bytes that follow, at least one: the rest of the
    // current part at most.
    // SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:137-155
    std::span<const std::byte> next(std::uint64_t limit) {
        if (limit == 0 || remaining() == 0) fail("experience_section_invalid");
        if (chunk_.empty()) load();
        const auto take = static_cast<std::size_t>(std::min<std::uint64_t>(limit, chunk_.size()));
        const auto out = chunk_.first(take);
        chunk_ = chunk_.subspan(take);
        read_ += take;
        if (blob_.parted()) whole_.update(out);
        return out;
    }

    // SWEGCA: user@2026-09-22:60-61
    std::uint8_t u8() {
        std::array<std::byte, 1> bytes{};
        fixed(bytes);
        return std::to_integer<std::uint8_t>(bytes[0]);
    }
    // SWEGCA: user@2026-09-22:60-61
    std::uint32_t u32() { return static_cast<std::uint32_t>(little_endian<4>()); }
    // SWEGCA: user@2026-09-22:60-61
    std::uint64_t u64() { return little_endian<8>(); }
    // SWEGCA: user@2026-09-22:60-61
    DigestBytes digest() {
        DigestBytes out{};
        fixed(out);
        return out;
    }

    // Every byte read; a parted blob's bytes are its digest.
    // SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:137-155
    void finish() {
        if (remaining() != 0) fail("experience_section_invalid");
        if (blob_.parted() && whole_.finish() != blob_.digest) fail("experience_blob_digest_mismatch");
    }

private:
    struct Level {
        std::optional<journal::PublishedRecord> part;  // the part the list is, below the top
        std::span<const std::byte> list;               // digests of the parts of this level
        std::uint64_t at = 0;                          // the next one
    };

    // SWEGCA: user@2026-09-22:60-61
    template <std::size_t N>
    std::uint64_t little_endian() {
        std::array<std::byte, N> bytes{};
        fixed(bytes);
        std::uint64_t value = 0;
        for (std::size_t at = N; at-- > 0;) value = (value << 8) | std::to_integer<std::uint64_t>(bytes[at]);
        return value;
    }

    // SWEGCA: user@2026-09-22:60-61
    void fixed(std::span<std::byte> out) {
        if (out.size() > remaining()) fail("experience_section_invalid");
        for (std::size_t at = 0; at < out.size();) {
            const auto piece = next(out.size() - at);
            std::memcpy(out.data() + at, piece.data(), piece.size());
            at += piece.size();
        }
    }

    // The next level-0 part: up from the lowest level with a digest left,
    // then down, each list part replacing the one before it at its level.
    // SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:137-155
    void load() {
        std::uint8_t level = 0;
        while (level < blob_.depth && lists_[level].at * digest256_width == lists_[level].list.size()) ++level;
        if (level == blob_.depth) fail("experience_part_invalid");
        while (true) {
            auto& list = lists_[level];
            const auto digest = digest_at(list.list, list.at++);
            auto part = replay_part(*journal_, memory_, digest,
                                    part_length(levels_, blob_.size, level, placed_[level]++));
            if (level == 0) {
                leaf_.reset();
                leaf_.emplace(std::move(part));
                chunk_ = leaf_->view().payload;
                return;
            }
            --level;
            auto& below = lists_[level];
            below.part.reset();
            below.part.emplace(std::move(part));
            below.list = below.part->view().payload;
            below.at = 0;
        }
    }

    const journal::JournalStore* journal_;
    AllocationContext memory_;
    ExperienceBlob blob_;
    PartLevels levels_;
    std::array<Level, 3> lists_{};
    std::array<std::uint64_t, 3> placed_{};  // parts met so far per level
    std::optional<journal::PublishedRecord> leaf_;
    std::span<const std::byte> chunk_;  // what is left of the current level-0 part
    std::uint64_t read_ = 0;
    Sha256 whole_;
};

// `length` bytes that follow, whole: a view when they come in one piece that
// stays valid, else a copy in `buffer`.
// SWEGCA: user@2026-09-22:91-92
std::span<const std::byte> held_bytes(BlobCursor& in, std::uint64_t length, LedgerBytes& buffer) {
    if (length > in.remaining()) fail("experience_section_invalid");
    if (length == 0) return {};
    const auto first = in.next(length);
    if (first.size() == length && in.stable()) return first;
    buffer.assign(first.begin(), first.end());
    while (buffer.size() < length) {
        const auto piece = in.next(length - buffer.size());
        buffer.insert(buffer.end(), piece.begin(), piece.end());
    }
    return buffer;
}

// The fewest bytes a list item of the typed section takes: a length-prefixed
// field; a step (phase, observation, relation count, judgment, outcome,
// evidence count); a resource (id length, descriptor, digest, item and
// locator flags, metadata length, bytes flag).
constexpr std::uint64_t least_field_bytes = 8;
constexpr std::uint64_t least_step_bytes = 8 + 8 + 8 + 8 + 1 + 8;
constexpr std::uint64_t descriptor_encoded_bytes = 1 + 8 + 8 + 8 + 1 + 8 + 8 + 8;
constexpr std::uint64_t least_resource_bytes = 8 + descriptor_encoded_bytes + 1 + 1 + 1 + 8 + 1;

// Reads the typed section in order, checking every field, and hands each to
// `emit` as it comes (a field in pieces, validated after its last piece):
// - the user's episode and step rules (MemoryEpisode, MemoryStep): identity
//   texts and step texts hold something Python's str.strip keeps, source
//   addresses are strict UTF-8 and distinct, at least one source, step and
//   evidence ref, one of the six outcomes; relations strict UTF-8, unchecked
//   otherwise; an observation's bytes as given;
// - each resource: its id an identity text in the record's resource view,
//   no id twice (the source's key is the id within its memory); its
//   descriptor in its one form; a locator strict UTF-8; metadata as given;
//   received bytes as a standard blob with their digest, which is the
//   resource's digest, and a text's inline bytes strict UTF-8 of its code
//   point count (parted ones are read by whoever reads them);
// - every unlisted entry of the resource view is a resource here, and the
//   section is not empty.
// Returns false when `emit` stopped it, true when all was read and checked.
// SWEGCA: src/tinylm_slicer/mosaic_memory_activation.py@3bddcb7:63-106
// SWEGCA: src/swegca/mosaic_external_memory.py@5901a5a:33-41
// SWEGCA: src/swegca/mosaic_external_memory.py@5901a5a:111
bool read_section(BlobCursor& in, std::span<const std::string_view> resources,
                  std::span<const std::string_view> listed, const AllocationContext& memory, SectionVisitor emit) {
    const auto counted = [&in](std::uint64_t count, std::uint64_t least) {
        if (count > in.remaining() / least) fail("experience_section_invalid");
        return count;
    };
    SectionEvent event;
    // One length-prefixed field as events of `part`; `text` and `hash`, when
    // given, see each byte once.
    const auto field = [&](SectionPart part, Utf8Count* text, Sha256* hash) {
        const auto length = in.u64();
        if (length > in.remaining()) fail("experience_section_invalid");
        event.part = part;
        auto left = length;
        do {
            const auto piece = left == 0 ? std::span<const std::byte>() : in.next(left);
            left -= piece.size();
            if (text) text->feed(piece);
            if (hash) hash->update(piece);
            event.piece = piece;
            event.last = left == 0;
            if (!emit(event)) return false;
        } while (left != 0);
        event.piece = {};
        return true;
    };
    // The user's `_text`: strict UTF-8 holding something str.strip keeps.
    const auto step_text = [&](SectionPart part) {
        Utf8Count text;
        if (!field(part, &text, nullptr)) return false;
        if (!text.valid() || !text.nonspace()) fail("experience_section_invalid");
        return true;
    };
    const auto utf8_field = [&](SectionPart part) {
        Utf8Count text;
        if (!field(part, &text, nullptr)) return false;
        if (!text.valid()) fail("experience_section_invalid");
        return true;
    };

    const auto has_episode = in.u8();
    if (has_episode > 1) fail("experience_section_invalid");
    if (has_episode == 1) {
        if (!step_text(SectionPart::episode_id) || !step_text(SectionPart::revision) ||
            !step_text(SectionPart::verification_state))
            return false;
        const auto sources = counted(in.u64(), least_field_bytes);
        if (sources == 0) fail("experience_section_invalid");
        // Distinct by content: the SHA-256 of each, so any length is compared.
        LedgerVector<DigestBytes> seen(memory.allocator<DigestBytes>());
        for (std::uint64_t at = 0; at < sources; ++at) {
            event.item = at;
            Utf8Count text;
            Sha256 hash;
            if (!field(SectionPart::source_address, &text, &hash)) return false;
            if (!text.valid()) fail("experience_section_invalid");
            seen.push_back(hash.finish());
        }
        std::sort(seen.begin(), seen.end());
        if (std::adjacent_find(seen.begin(), seen.end()) != seen.end()) fail("experience_section_invalid");
        const auto steps = counted(in.u64(), least_step_bytes);
        if (steps == 0) fail("experience_section_invalid");
        for (std::uint64_t step = 0; step < steps; ++step) {
            event.step = step;
            event.item = 0;
            if (!step_text(SectionPart::phase) || !field(SectionPart::observation, nullptr, nullptr)) return false;
            const auto relations = counted(in.u64(), least_field_bytes);
            for (std::uint64_t at = 0; at < relations; ++at) {
                event.item = at;
                if (!utf8_field(SectionPart::relation)) return false;
            }
            event.item = 0;
            if (!step_text(SectionPart::judgment)) return false;
            const auto outcome = in.u8();
            if (outcome > static_cast<std::uint8_t>(StepOutcome::pending)) fail("experience_section_invalid");
            event.part = SectionPart::outcome;
            event.outcome = static_cast<StepOutcome>(outcome);
            event.last = true;
            if (!emit(event)) return false;
            const auto refs = counted(in.u64(), least_field_bytes);
            if (refs == 0) fail("experience_section_invalid");
            for (std::uint64_t at = 0; at < refs; ++at) {
                event.item = at;
                if (!step_text(SectionPart::evidence_ref)) return false;
            }
        }
    }

    const auto count = counted(in.u64(), least_resource_bytes);
    if (count > max_resources || (has_episode == 0 && count == 0)) fail("experience_section_invalid");
    LedgerVector<std::uint8_t> met(resources.size(), 0, memory.allocator<std::uint8_t>());
    LedgerBytes id_buffer(memory.allocator<std::byte>());
    LedgerBytes top_buffer(memory.allocator<std::byte>());
    for (std::uint64_t at = 0; at < count; ++at) {
        event = SectionEvent{};
        event.resource = at;
        const auto id_length = in.u64();
        if (id_length > detail::identity_text_max_bytes) fail("experience_section_invalid");
        const auto id_bytes = held_bytes(in, id_length, id_buffer);
        const std::string_view id(reinterpret_cast<const char*>(id_bytes.data()), id_bytes.size());
        if (!detail::is_identity_text(id)) fail("experience_section_invalid");
        const auto found = std::lower_bound(resources.begin(), resources.end(), id);
        if (found == resources.end() || *found != id) fail("experience_section_invalid");
        auto& mark = met[static_cast<std::size_t>(found - resources.begin())];
        if (mark != 0) fail("experience_section_invalid");
        mark = 1;
        auto& d = event.descriptor;
        const auto modality = in.u8();
        d.code_points = in.u64();
        d.width = in.u64();
        d.height = in.u64();
        const auto frame = in.u8();
        d.frames = in.u64();
        d.sample_rate = in.u64();
        d.duration_ns = in.u64();
        if (modality > static_cast<std::uint8_t>(ResourceModality::video) ||
            frame > static_cast<std::uint8_t>(CoordinateFrame::oriented_source_pixels))
            fail("experience_section_invalid");
        d.modality = static_cast<ResourceModality>(modality);
        d.frame = static_cast<CoordinateFrame>(frame);
        if (!valid_descriptor(d)) fail("experience_section_invalid");
        const auto has_digest = in.u8();
        if (has_digest > 1) fail("experience_section_invalid");
        if (has_digest == 1) event.content_digest = in.digest();
        const auto has_item = in.u8();
        if (has_item > 1) fail("experience_section_invalid");
        if (has_item == 1) event.item_index = in.u64();
        event.part = SectionPart::resource;
        event.resource_id = id;
        if (!emit(event)) return false;
        const auto has_locator = in.u8();
        if (has_locator > 1) fail("experience_section_invalid");
        if (has_locator == 1 && !utf8_field(SectionPart::storage_locator)) return false;
        if (!field(SectionPart::metadata, nullptr, nullptr)) return false;
        const auto has_bytes = in.u8();
        if (has_bytes > 1) fail("experience_section_invalid");
        if (has_bytes == 0) continue;
        // Received bytes keep the digest computed from them.
        if (!event.content_digest) fail("experience_section_invalid");
        event.part = SectionPart::resource_bytes;
        const auto mode = in.u8();
        if (mode == 0) {
            const auto length = in.u32();
            if (length > experience_inline_blob_bytes || length > in.remaining()) fail("experience_section_invalid");
            event.size = length;
            Sha256 hash;
            Utf8Count text;
            std::uint64_t left = length;
            do {
                const auto piece = left == 0 ? std::span<const std::byte>() : in.next(left);
                left -= piece.size();
                hash.update(piece);
                text.feed(piece);
                event.piece = piece;
                event.last = left == 0;
                if (!emit(event)) return false;
            } while (left != 0);
            if (hash.finish() != *event.content_digest) fail("experience_section_invalid");
            if (d.modality == ResourceModality::text && (!text.valid() || text.count() != d.code_points))
                fail("experience_section_invalid");
        } else if (mode == 1) {
            event.size = in.u64();
            const auto digest = in.digest();
            const auto depth = in.u8();
            const auto tops = in.u32();
            if (event.size <= experience_inline_blob_bytes || digest != *event.content_digest)
                fail("experience_section_invalid");
            const auto levels = part_levels(event.size);
            if (depth != levels.depth || tops != levels.counts[levels.depth - 1]) fail("experience_section_invalid");
            event.parted = true;
            event.piece = held_bytes(in, static_cast<std::uint64_t>(tops) * digest256_width, top_buffer);
            event.last = true;
            if (!emit(event)) return false;
        } else {
            fail("experience_section_invalid");
        }
    }
    for (std::size_t at = 0; at < resources.size(); ++at)
        if (met[at] == 0 && !std::binary_search(listed.begin(), listed.end(), resources[at]))
            fail("experience_section_invalid");
    in.finish();
    return true;
}

// The user's MemoryEpisode and MemoryStep checks, the texts kept as given.
// SWEGCA: src/tinylm_slicer/mosaic_memory_activation.py@3bddcb7:63-106
void check_episode(const AllocationContext& memory, const MemoryEpisodeInput& episode) {
    if (!is_step_text(episode.episode_id) || !is_step_text(episode.revision) ||
        !is_step_text(episode.verification_state))
        fail("experience_episode_text_invalid");
    if (episode.steps.empty() || episode.source_addresses.empty()) fail("experience_episode_incomplete");
    LedgerVector<std::string_view> sources(episode.source_addresses.begin(), episode.source_addresses.end(),
                                           memory.allocator<std::string_view>());
    for (const auto source : sources)
        if (!detail::is_strict_utf8(source)) fail("experience_episode_text_invalid");
    std::sort(sources.begin(), sources.end());
    if (std::adjacent_find(sources.begin(), sources.end()) != sources.end())
        fail("experience_episode_source_duplicate");
    for (const auto& step : episode.steps) {
        if (!is_step_text(step.phase) || !is_step_text(step.judgment)) fail("experience_step_text_invalid");
        if (step.outcome > StepOutcome::pending) fail("experience_step_outcome_invalid");
        if (step.evidence_refs.empty()) fail("experience_step_evidence_missing");
        for (const auto ref : step.evidence_refs)
            if (!is_step_text(ref)) fail("experience_step_evidence_missing");
        for (const auto relation : step.relations)
            if (!detail::is_strict_utf8(relation)) fail("experience_step_text_invalid");
    }
}

// The typed section of an observation, laid out in `out` for reading as one
// blob: an episode flag and the episode (its three texts, source addresses
// and steps, each list after its u64 count, each text and byte field after
// its u64 length), then the observed resources in the producer's order (id,
// descriptor, digest, item index, locator, metadata, and received bytes as
// a standard blob). The caller's texts and bytes are not copied: `out`
// keeps its own fields and prefixes and names the caller's spans between
// them. Received bytes are planned as every blob (their parts go to `own`,
// their lists to `owned`); a text resource's must be strict UTF-8 of its
// code point count, and a digest the source names must be theirs.
// SWEGCA: src/tinylm_slicer/mosaic_memory_activation.py@3bddcb7:63-106
// SWEGCA: src/swegca/mosaic_external_memory.py@5901a5a:33-41
void build_section(const AllocationContext& memory, const Observation& observation, detail::SectionSource& out,
                   LedgerVector<PartSlice>& own, LedgerVector<LedgerBytes>& owned) {
    ByteWriter writer(out.encoded);
    std::size_t mark = 0;
    // The encoded bytes since `mark`, then the caller's `bytes`.
    const auto caller = [&](std::span<const std::byte> bytes) {
        if (out.encoded.size() > mark) {
            const auto length = out.encoded.size() - mark;
            out.pieces.push_back(detail::SectionSource::Piece{out.size, mark, length, {}});
            out.size += length;
            mark = out.encoded.size();
        }
        if (bytes.empty()) return;
        out.pieces.push_back(detail::SectionSource::Piece{out.size, 0, bytes.size(), bytes});
        out.size += bytes.size();
    };
    const auto field = [&](std::span<const std::byte> bytes) {
        writer.u64(bytes.size());
        caller(bytes);
    };
    const auto text = [&](std::string_view value) {
        field(std::as_bytes(std::span<const char>(value.data(), value.size())));
    };
    writer.u8(observation.episode ? 1 : 0);
    if (observation.episode) {
        const auto& episode = *observation.episode;
        text(episode.episode_id);
        text(episode.revision);
        text(episode.verification_state);
        writer.u64(episode.source_addresses.size());
        for (const auto source : episode.source_addresses) text(source);
        writer.u64(episode.steps.size());
        for (const auto& step : episode.steps) {
            text(step.phase);
            field(step.observation);
            writer.u64(step.relations.size());
            for (const auto relation : step.relations) text(relation);
            text(step.judgment);
            writer.u8(static_cast<std::uint8_t>(step.outcome));
            writer.u64(step.evidence_refs.size());
            for (const auto ref : step.evidence_refs) text(ref);
        }
    }
    writer.u64(observation.observed_resources.size());
    for (const auto& resource : observation.observed_resources) {
        std::optional<BlobPlan> bytes;
        auto digest = resource.content_digest;
        if (resource.bytes) {
            const bool is_text = resource.descriptor.modality == ResourceModality::text;
            Utf8Count count;
            bytes = plan_blob(memory, *resource.bytes, own, owned, is_text ? &count : nullptr);
            if (is_text && (!count.valid() || count.count() != resource.descriptor.code_points))
                fail("experience_resource_text_invalid");
            if (resource.content_digest && *resource.content_digest != bytes->digest)
                fail("experience_resource_digest_mismatch");
            digest = bytes->digest;
        }
        const auto& d = resource.descriptor;
        text(resource.resource_id);
        writer.u8(static_cast<std::uint8_t>(d.modality));
        writer.u64(d.code_points);
        writer.u64(d.width);
        writer.u64(d.height);
        writer.u8(static_cast<std::uint8_t>(d.frame));
        writer.u64(d.frames);
        writer.u64(d.sample_rate);
        writer.u64(d.duration_ns);
        writer.u8(digest ? 1 : 0);
        if (digest) writer.digest(*digest);
        writer.u8(resource.item_index ? 1 : 0);
        if (resource.item_index) writer.u64(*resource.item_index);
        writer.u8(resource.storage_locator ? 1 : 0);
        if (resource.storage_locator) text(*resource.storage_locator);
        field(resource.metadata);
        writer.u8(bytes ? 1 : 0);
        if (!bytes) continue;
        if (bytes->depth == 0) {
            writer.u8(0);
            writer.u32(static_cast<std::uint32_t>(bytes->size));
            caller(bytes->inline_bytes);
            continue;
        }
        writer.u8(1);
        writer.u64(bytes->size);
        writer.digest(bytes->digest);
        writer.u8(bytes->depth);
        writer.u32(static_cast<std::uint32_t>(bytes->top.size() / digest256_width));
        caller(bytes->top);
    }
    caller({});
}

// The experience payload: the observation's step, uncertainty,
// contradiction, source span, context, namespace, sorted lineage, sorted
// resources each with its listed flag, its raw, structured and (derived
// only) root-source and root-context blobs, and its typed section blob after
// a flag.
// SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:23-60
LedgerBytes encode_envelope(const AllocationContext& memory, const Observation& observation,
                            std::span<const std::string_view> derived_from,
                            std::span<const std::string_view> resources, std::span<const std::uint8_t> listed,
                            const BlobPlan& raw, const BlobPlan& structured, const BlobPlan* roots,
                            const BlobPlan* contexts, const BlobPlan* section) {
    std::uint64_t total = envelope_fixed_bytes;
    if (observation.name_space) total += 4 + observation.name_space->size();
    for (const auto address : derived_from) total += 4 + address.size();
    for (const auto resource : resources) total += 4 + resource.size() + 1;
    total += encoded_blob_bytes(raw) + encoded_blob_bytes(structured);
    if (roots) total += encoded_blob_bytes(*roots) + encoded_blob_bytes(*contexts);
    total += 1;
    if (section) total += encoded_blob_bytes(*section);
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
    for (std::size_t at = 0; at < resources.size(); ++at) {
        writer.text(resources[at], detail::identity_text_max_bytes);
        writer.u8(listed[at]);
    }
    write_blob(writer, raw);
    write_blob(writer, structured);
    if (roots) {
        write_blob(writer, *roots);
        write_blob(writer, *contexts);
    }
    writer.u8(section ? 1 : 0);
    if (section) write_blob(writer, *section);
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
    // C++ infrastructure: memory and disk budgets stay hard limits (approved
    // flow :91-92); no direct Python counterpart.
    // SWEGCA: user@2026-09-22:91-92
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

namespace detail {

// Copies the pieces that hold `out`: this section's own fields and prefixes
// from `encoded`, the caller's texts and bytes from where they are.
// SWEGCA: user@2026-09-22:91-92
void SectionSource::operator()(std::uint64_t offset, std::span<std::byte> out) const {
    if (offset > size || out.size() > size - offset) throw std::out_of_range("experience_section_read_out_of_range");
    if (out.empty()) return;
    auto piece = std::upper_bound(pieces.begin(), pieces.end(), offset,
                                  [](std::uint64_t at, const Piece& each) { return at < each.at; });
    std::size_t done = 0;
    for (--piece; done < out.size(); ++piece) {
        const auto from = static_cast<std::size_t>(offset + done - piece->at);
        const auto take = static_cast<std::size_t>(std::min<std::uint64_t>(piece->length - from, out.size() - done));
        const auto* bytes = piece->caller.empty() ? encoded.data() + piece->encoded_from : piece->caller.data();
        std::memcpy(out.data() + done, bytes + from, take);
        done += take;
    }
}

}  // namespace detail

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
      derived_from_(std::move(derived)), resources_(std::move(resources)), index_(std::move(index)),
      listed_(memory.allocator<std::string_view>()), section_texts_(memory.allocator<std::string_view>()),
      steps_(memory.allocator<StepView>()), observed_(memory.allocator<ResourceView>()) {}

// Moving the record keeps every view valid (its bytes and lists keep their
// buffers); the source keeps none. C++ infrastructure; no direct Python
// counterpart.
// SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:23-60
ExperienceRecord::ExperienceRecord(ExperienceRecord&& other) noexcept : record_(std::move(other.record_)),
      journal_(other.journal_), memory_(other.memory_), observed_at_(other.observed_at_), context_(other.context_),
      uncertainty_(other.uncertainty_), contradiction_(other.contradiction_), span_(other.span_),
      derived_from_(std::move(other.derived_from_)), name_space_(std::exchange(other.name_space_, std::nullopt)),
      resources_(std::move(other.resources_)), index_(std::move(other.index_)), raw_(std::exchange(other.raw_, {})),
      structured_(std::exchange(other.structured_, {})), roots_(std::exchange(other.roots_, {})),
      contexts_(std::exchange(other.contexts_, {})), listed_(std::move(other.listed_)),
      has_section_(std::exchange(other.has_section_, false)), section_(std::exchange(other.section_, {})),
      section_texts_(std::move(other.section_texts_)), episode_(std::exchange(other.episode_, std::nullopt)),
      steps_(std::move(other.steps_)), observed_(std::move(other.observed_)) {}

// Checks the kind, the envelope byte for byte, that the address is the
// digest of the record's identity (so it was issued by this module), and
// that the index entries are exactly the automatic ones: a memory record
// carries no authored cue (user 2026-09-23 18:0x; the author's artifact has
// none, cues are the caller's).
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
    LedgerVector<std::string_view> resources(memory.allocator<std::string_view>());
    LedgerVector<std::string_view> listed(memory.allocator<std::string_view>());
    resources.reserve(resource_count);
    for (std::uint32_t at = 0; at < resource_count; ++at) {
        const auto text = reader.text_view(detail::identity_text_max_bytes);
        const auto flag = reader.u8();
        if (!detail::is_identity_text(text) || flag > 1 || (at != 0 && !(resources.back() < text)))
            fail("experience_envelope_invalid");
        resources.push_back(text);
        if (flag == 1) listed.push_back(text);
    }
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
    const auto has_section = reader.u8();
    if (has_section > 1) fail("experience_envelope_invalid");
    ExperienceBlob section;
    if (has_section == 1) section = read_blob(reader);
    if (reader.remaining() != 0) fail("experience_envelope_trailing_bytes");
    // An entry the producer did not list is a section resource's id, so a
    // memory without a section lists every entry.
    if (has_section == 0 && listed.size() != resources.size()) fail("experience_envelope_invalid");

    // A typed section kept in the record is read and checked whole here
    // (`read_section`) and viewed: each of its fields comes in one piece
    // viewing the record's bytes. Step lists are gathered as positions in
    // `texts` and viewed once the record holds `texts`. A parted section is
    // checked when it is read (`for_each_section`, `verify_parts`).
    struct StepSlots {
        std::string_view phase;
        std::span<const std::byte> observation;
        std::size_t relations = 0;
        std::size_t relation_count = 0;
        std::string_view judgment;
        StepOutcome outcome = StepOutcome::pending;
        std::size_t refs = 0;
        std::size_t ref_count = 0;
    };
    LedgerVector<std::string_view> texts(memory.allocator<std::string_view>());
    LedgerVector<StepSlots> step_slots(memory.allocator<StepSlots>());
    LedgerVector<ResourceView> observed(memory.allocator<ResourceView>());
    std::optional<EpisodeView> episode;
    std::size_t source_count = 0;
    if (has_section == 1 && !section.parted()) {
        BlobCursor in(journal, memory, section);
        (void)read_section(in, resources, listed, memory, [&](const SectionEvent& event) {
            const std::string_view text(reinterpret_cast<const char*>(event.piece.data()), event.piece.size());
            switch (event.part) {
            case SectionPart::episode_id:
                episode.emplace();
                episode->episode_id = text;
                break;
            case SectionPart::revision:
                episode->revision = text;
                break;
            case SectionPart::verification_state:
                episode->verification_state = text;
                break;
            case SectionPart::source_address:
                texts.push_back(text);
                ++source_count;
                break;
            case SectionPart::phase:
                step_slots.push_back(StepSlots{});
                step_slots.back().phase = text;
                break;
            case SectionPart::observation:
                step_slots.back().observation = event.piece;
                break;
            case SectionPart::relation:
                if (step_slots.back().relation_count++ == 0) step_slots.back().relations = texts.size();
                texts.push_back(text);
                break;
            case SectionPart::judgment:
                step_slots.back().judgment = text;
                break;
            case SectionPart::outcome:
                step_slots.back().outcome = event.outcome;
                break;
            case SectionPart::evidence_ref:
                if (step_slots.back().ref_count++ == 0) step_slots.back().refs = texts.size();
                texts.push_back(text);
                break;
            case SectionPart::resource:
                observed.push_back(ResourceView{event.resource_id, event.descriptor, event.content_digest,
                                                std::nullopt, event.item_index, {}, std::nullopt});
                break;
            case SectionPart::storage_locator:
                observed.back().storage_locator = text;
                break;
            case SectionPart::metadata:
                observed.back().metadata = event.piece;
                break;
            case SectionPart::resource_bytes:
                observed.back().bytes =
                    event.parted
                        ? ExperienceBlob{event.size, *event.content_digest, part_levels(event.size).depth, {}, event.piece}
                        : ExperienceBlob{event.size, *event.content_digest, 0, event.piece, {}};
                break;
            }
            return true;
        });
    }

    const auto address = address_of(view.kind, view.source, view.source_revision,
                                     present(view.previous_revision_address), present(view.outcome),
                                     view.payload_digest);
    if (view.address != view_of(address)) fail("experience_address_mismatch");

    // The record's entries against the automatic ones: every automatic one
    // present, and no other.
    LedgerVector<std::string_view> index(memory.allocator<std::string_view>());
    index.reserve(view.index_count);
    journal::for_each_index_entry(view, [&index](std::string_view entry) { index.push_back(entry); });
    IndexEntries automatic(memory);
    add_automatic(automatic, memory,
                  IndexFields{view.source, view.source_revision, present(view.previous_revision_address),
                              derived, name_space, resources, raw.digest, present(view.transaction_id)});
    const auto expected = automatic.finish();
    std::size_t next = 0;
    for (const auto entry : index) {
        if (next < expected.size() && expected[next] == entry) {
            ++next;
            continue;
        }
        if (next < expected.size() && expected[next] < entry) fail("experience_index_incomplete");
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
    out.listed_ = std::move(listed);
    out.has_section_ = has_section == 1;
    out.section_ = section;
    out.observed_ = std::move(observed);
    // Step and source lists view the record's own `section_texts_`, so they
    // are made only once it holds them.
    out.section_texts_ = std::move(texts);
    const std::span<const std::string_view> held(out.section_texts_);
    if (episode) {
        episode->source_addresses = held.subspan(0, source_count);
        out.episode_ = episode;
    }
    out.steps_.reserve(step_slots.size());
    for (const auto& step : step_slots)
        out.steps_.push_back(StepView{step.phase, step.observation, held.subspan(step.relations, step.relation_count),
                                      step.judgment, step.outcome, held.subspan(step.refs, step.ref_count)});
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

// Reads the blob part by part (`BlobCursor`): a part must be a part record
// with no authority, claim or index entry, its payload the digest its parent
// names and exactly as long as its place in the tree gives (the last part of
// each level may be shorter). Each level-0 part's bytes are visited once
// checked; the whole blob's digest is checked after the last.
// SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:137-155
void ExperienceRecord::for_each_chunk(const ExperienceBlob& blob, ChunkVisitor visit) const {
    if (!blob.parted()) {
        (void)visit(blob.inline_bytes);
        return;
    }
    BlobCursor in(*journal_, memory_, blob);
    while (in.remaining() != 0)
        if (!visit(in.next(in.remaining()))) return;
    in.finish();
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
    if (!has_section_) return;
    // The section, read and checked whole; each resource's parted bytes read
    // to their end, a text resource's as strict UTF-8 of its code point count.
    BlobCursor in(*journal_, memory_, section_);
    (void)read_section(in, resources_, listed_, memory_, [this, &whole](const SectionEvent& event) {
        if (event.part != SectionPart::resource_bytes || !event.parted) return true;
        const ExperienceBlob bytes{event.size, *event.content_digest, part_levels(event.size).depth, {}, event.piece};
        if (event.descriptor.modality != ResourceModality::text) {
            for_each_chunk(bytes, whole);
            return true;
        }
        Utf8Count count;
        for_each_chunk(bytes, [&count](std::span<const std::byte> chunk) {
            count.feed(chunk);
            return true;
        });
        if (!count.valid() || count.count() != event.descriptor.code_points)
            fail("experience_resource_text_invalid");
        return true;
    });
}

// SWEGCA: src/tinylm_slicer/mosaic_memory_activation.py@3bddcb7:63-106
void ExperienceRecord::for_each_section(SectionVisitor visit) const {
    if (!has_section_) return;
    BlobCursor in(*journal_, memory_, section_);
    (void)read_section(in, resources_, listed_, memory_, visit);
}

// A section kept in the record gives the resource's bytes directly; a parted
// one is read up to the resource and on through its bytes (inline ones to
// their digest check, parted ones as every blob).
// SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:137-155
void ExperienceRecord::for_each_resource_chunk(std::size_t resource, ChunkVisitor visit) const {
    if (!section_parted()) {
        if (resource >= observed_.size()) throw std::out_of_range("experience_resource_index_out_of_range");
        if (!observed_[resource].bytes) fail("experience_resource_bytes_absent");
        for_each_chunk(*observed_[resource].bytes, visit);
        return;
    }
    bool found = false;
    bool received = false;
    bool done = false;
    BlobCursor in(*journal_, memory_, section_);
    (void)read_section(in, resources_, listed_, memory_, [&](const SectionEvent& event) {
        if (done) return false;
        if (event.part != SectionPart::resource && event.part != SectionPart::storage_locator &&
            event.part != SectionPart::metadata && event.part != SectionPart::resource_bytes)
            return true;
        if (event.resource != resource) return event.resource < resource;
        found = true;
        if (event.part != SectionPart::resource_bytes) return true;
        received = true;
        if (event.parted) {
            for_each_chunk(
                ExperienceBlob{event.size, *event.content_digest, part_levels(event.size).depth, {}, event.piece},
                visit);
            return false;
        }
        if (!visit(event.piece)) return false;
        done = event.last;
        return true;
    });
    if (!found) throw std::out_of_range("experience_resource_index_out_of_range");
    if (!received) fail("experience_resource_bytes_absent");
}

namespace {

// What a binding record names once checked: its target and its cues,
// both viewing the record's bytes.
struct CheckedBinding {
    std::string_view target;
    LedgerVector<std::string_view> cues;
};

// Checks the kind, the payload byte for byte, the address against the
// binding's digest under its target, the entries against its cues, and
// that the target is a memory held before the binding (the author refuses
// keys for an unknown address, mosaic_unrestricted_experience.py@5901a5a:408-410).
// `resolve` gives the target's position, `kind_of` its record kind: from
// the published journal when decoding, from the rebuilt view when a
// rebuild validates the record before it is published.
// SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:398-436
template <class Resolve, class KindOf>
CheckedBinding checked_binding(const journal::RecordView& view, const journal::RecordPosition& at,
                               const AllocationContext& memory, const Resolve& resolve, const KindOf& kind_of) {
    if (view.kind != cue_binding_kind) fail("experience_cue_binding_kind_invalid");
    if (view.authority || !view.claim.empty() || !view.previous_revision_address.empty() || !view.outcome.empty())
        fail("experience_cue_binding_invalid");
    ByteReader reader(view.payload);
    const auto magic = reader.raw(binding_magic.size());
    if (!std::equal(magic.begin(), magic.end(), binding_magic.begin()) || reader.u16() != binding_version)
        fail("experience_cue_binding_invalid");
    const auto target = reader.text_view(detail::identity_text_max_bytes);
    if (!is_experience_address(target)) fail("experience_cue_binding_invalid");
    journal::RecordPosition named;
    named.segment_ordinal = reader.u64();
    named.byte_offset = reader.u64();
    named.sequence = reader.u64();
    named.record_digest = reader.digest();
    const auto count = reader.u32();
    if (count == 0 || count > max_bound_cues) fail("experience_cue_binding_invalid");
    auto cues = read_sorted_texts(reader, count, memory,
                                  [&memory](std::string_view cue) { return is_normalized_cue(memory, cue); });
    if (reader.remaining() != 0) fail("experience_cue_binding_invalid");
    if (view.address != view_of(binding_address_of(target, view.source, view.source_revision, cues)))
        fail("experience_cue_binding_address_mismatch");

    IndexEntries expected(memory);
    for (const auto cue : cues) expected.add_cue(cue, cue_binding_address_bytes);
    const auto entries = expected.finish();
    std::size_t next = 0;
    bool exact = true;
    journal::for_each_index_entry(view, [&](std::string_view entry) {
        exact = exact && next < entries.size() && entries[next] == entry;
        ++next;
    });
    if (!exact || next != entries.size()) fail("experience_cue_binding_index_invalid");

    const std::optional<journal::RecordPosition> target_at = resolve(target);
    // The target is the memory at the position the binding names, published
    // before it.
    if (!target_at || target_at->segment_ordinal != named.segment_ordinal ||
        target_at->byte_offset != named.byte_offset || target_at->sequence != named.sequence ||
        target_at->record_digest != named.record_digest || !(named.sequence < at.sequence))
        fail("experience_cue_binding_target_unknown");
    const std::uint16_t kind = kind_of(target);
    if (kind != original_experience_kind && kind != derived_experience_kind)
        fail("experience_cue_binding_target_unknown");
    return CheckedBinding{target, std::move(cues)};
}

}  // namespace

// SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:398-436
CueBindingRecord CueBindingRecord::decode(journal::PublishedRecord published, const AllocationContext& memory,
                                          const journal::JournalStore& journal) {
    auto checked = checked_binding(
        published.view(), published.position(), memory,
        [&](std::string_view target) { return journal.resolve(ExperienceAddress(memory, target)); },
        [&](std::string_view target) {
            const auto held = journal.replay(ExperienceAddress(memory, target));
            return held.view().kind;
        });
    return CueBindingRecord(std::move(published), checked.target, std::move(checked.cues));
}

// The same checks for a binding a view rebuild meets before publishing it:
// its target is resolved and read in the rebuilt view, so a target that the
// rebuild has not seen earlier in record order fails.
// SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:398-436
void CueBindingRecord::validate_rebuilt(const journal::RecordView& record, const journal::RecordPosition& position,
                                        const journal::RebuildReader& reader, const AllocationContext& memory) {
    (void)checked_binding(
        record, position, memory, [&reader](std::string_view target) { return reader.resolve(target); },
        [&reader](std::string_view target) {
            const auto held = reader.replay(target);
            return held.view().kind;
        });
}

// Everything is checked, encoded and cut into parts here, before anything
// is staged; a failure stages nothing. An observation already in the
// journal (or earlier in `observations`) gets the existing address and no
// second record, and must carry the same index entries apart from the
// transaction; its parts are not appended. A cue binding equal to one the
// journal holds (or an earlier one here) is not appended again.
// C++ infrastructure for admitting observations (approved flow :61);
// no direct Python counterpart.
// SWEGCA: user@2026-09-22:61
ExperienceAppend::ExperienceAppend(const ExperienceJournal& journal, const AllocationContext& memory,
                                   std::span<const Observation> observations, std::span<const CueBinding> bindings,
                                   std::string_view operation_id, std::optional<std::string_view> transaction_id)
    : journal_(&journal), memory_(memory), observations_(observations), operation_id_(operation_id),
      transaction_id_(transaction_id), heads_(memory.allocator<Head>()), parts_(memory.allocator<Part>()),
      owned_(memory.allocator<LedgerBytes>()), sections_(memory.allocator<detail::SectionSource>()),
      addresses_(memory.allocator<ExperienceAddress>()),
      binding_inputs_(bindings), bindings_(memory.allocator<Binding>()) {
    detail::require_identity_text(operation_id, journal::OperationIdTag::name);
    if (transaction_id) detail::require_identity_text(*transaction_id, TransactionIdTag::name);
    const auto& store = journal.journal_;

    // Every field, with lineage sorted, and the resources (those listed and
    // each observed resource's id) sorted with whether each was listed.
    struct Sorted {
        LedgerVector<std::string_view> derived;
        LedgerVector<std::string_view> resources;
        LedgerVector<std::uint8_t> listed;
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
        if (observation.episode) check_episode(memory_, *observation.episode);
        if (observation.observed_resources.size() > max_resources) fail("experience_too_many_resources");
        LedgerVector<std::string_view> observed(memory_.allocator<std::string_view>());
        observed.reserve(observation.observed_resources.size());
        for (const auto& resource : observation.observed_resources) {
            detail::require_identity_text(resource.resource_id, "resource");
            if (!valid_descriptor(resource.descriptor)) fail("experience_resource_descriptor_invalid");
            if (resource.storage_locator && !detail::is_strict_utf8(*resource.storage_locator))
                fail("experience_resource_locator_invalid");
            observed.push_back(resource.resource_id);
        }
        // The source keys a resource by its id within its memory
        // (mosaic_external_memory.py@5901a5a:111): no id twice.
        std::sort(observed.begin(), observed.end());
        if (std::adjacent_find(observed.begin(), observed.end()) != observed.end())
            fail("experience_resource_duplicate");
        // The resource view holds both lists; each entry keeps whether the
        // producer listed it, so both lists stay recoverable as given.
        LedgerVector<std::string_view> all(memory_.allocator<std::string_view>());
        LedgerVector<std::uint8_t> listed(memory_.allocator<std::uint8_t>());
        all.reserve(resources.size() + observed.size());
        listed.reserve(resources.size() + observed.size());
        std::size_t left = 0;
        std::size_t right = 0;
        while (left < resources.size() || right < observed.size()) {
            if (right == observed.size() || (left < resources.size() && resources[left] <= observed[right])) {
                if (right < observed.size() && resources[left] == observed[right]) ++right;
                all.push_back(resources[left++]);
                listed.push_back(1);
            } else {
                all.push_back(observed[right++]);
                listed.push_back(0);
            }
        }
        if (all.size() > max_resources) fail("experience_too_many_resources");
        sorted.push_back(Sorted{std::move(derived), std::move(all), std::move(listed)});
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
    sections_.reserve(observations.size());
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
        // The typed section, when the observation has an episode or observed
        // resources: laid out in place (`build_section`, which also plans the
        // received bytes) and planned as one blob read through its source,
        // which stays where `sections_` built it.
        std::optional<BlobPlan> section;
        if (observation.episode || !observation.observed_resources.empty()) {
            auto& source = sections_.emplace_back(memory_);
            build_section(memory_, observation, source, own, owned_);
            source.input.emplace(source.size, BlobReader(source));
            section = plan_blob(memory_, *source.input, own, owned_);
        }
        auto payload = encode_envelope(memory_, observation, fields.derived, fields.resources, fields.listed, raw,
                                       structured, roots ? &*roots : nullptr, contexts ? &*contexts : nullptr,
                                       section ? &*section : nullptr);
        const auto kind = fields.derived.empty() ? original_experience_kind : derived_experience_kind;
        IndexEntries index(memory_);
        add_automatic(index, memory_,
                      IndexFields{observation.source, observation.source_revision,
                                  observation.previous_revision_address, fields.derived, observation.name_space,
                                  fields.resources, raw.digest, transaction_id});
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

    // Each cue binding: its target is a memory the journal holds or one this
    // append gives; its cues, single tokens, distinct and increasing.
    // Each distinct target the journal must hold is looked up once.
    LedgerVector<std::string_view> given(memory_.allocator<std::string_view>());
    given.reserve(addresses_.size());
    for (const auto& address : addresses_) given.push_back(address.value());
    std::sort(given.begin(), given.end());
    LedgerVector<std::string_view> held_targets(memory_.allocator<std::string_view>());
    for (const auto& binding : bindings) {
        if (!is_experience_address(binding.target)) fail("experience_cue_binding_target_unknown");
        if (!std::binary_search(given.begin(), given.end(), binding.target)) held_targets.push_back(binding.target);
    }
    std::sort(held_targets.begin(), held_targets.end());
    held_targets.erase(std::unique(held_targets.begin(), held_targets.end()), held_targets.end());
    for (const auto target_text : held_targets) {
        const ExperienceAddress target(memory_, target_text);
        if (!store.resolve(target)) fail("experience_cue_binding_target_unknown");
        const auto held = store.replay(target);
        const auto kind = held.view().kind;
        if (kind != original_experience_kind && kind != derived_experience_kind)
            fail("experience_cue_binding_target_unknown");
    }
    bindings_.reserve(bindings.size());
    for (std::size_t at = 0; at < bindings.size(); ++at) {
        const auto& binding = bindings[at];
        detail::require_identity_text(binding.source, ProducerIdTag::name);
        detail::require_identity_text(binding.source_revision, journal::SourceRevisionTag::name);
        // Each cue kept whole in its normalized form (held in `owned_`, whose
        // moved buffers stay put); cues equal once normalized are one.
        if (binding.cues.size() > max_bound_cues) fail("experience_too_many_cues");
        LedgerVector<std::string_view> cues(memory_.allocator<std::string_view>());
        cues.reserve(binding.cues.size());
        for (const auto cue : binding.cues) {
            owned_.push_back(normalized_cue(memory_, cue));
            const auto kept = text_of(owned_.back());
            if (kept.empty() || !detail::is_identity_text(kept)) fail("experience_cue_invalid");
            cues.push_back(kept);
        }
        std::sort(cues.begin(), cues.end());
        cues.erase(std::unique(cues.begin(), cues.end()), cues.end());
        if (cues.empty()) fail("experience_cue_binding_empty");
        if (cues.size() > max_bound_cues) fail("experience_too_many_cues");
        IndexEntries index(memory_);
        for (const auto cue : cues) index.add_cue(cue, cue_binding_address_bytes);
        (void)index.finish();
        const auto address = binding_address_of(binding.target, binding.source, binding.source_revision, cues);
        bindings_.push_back(Binding{at, address, std::move(cues), LedgerBytes(memory_.allocator<std::byte>()),
                                    index.take_entries(), index.take_bytes(), false, std::nullopt});
    }
    // Equal bindings have equal addresses (the address digests all they
    // bind): only the first is appended, and none the journal holds.
    LedgerVector<std::size_t> by_address(memory_.allocator<std::size_t>());
    by_address.reserve(bindings_.size());
    for (std::size_t at = 0; at < bindings_.size(); ++at) by_address.push_back(at);
    std::sort(by_address.begin(), by_address.end(), [this](std::size_t left, std::size_t right) {
        return bindings_[left].address != bindings_[right].address ? bindings_[left].address < bindings_[right].address
                                                                   : left < right;
    });
    for (std::size_t rank = 0; rank < by_address.size(); ++rank) {
        auto& binding = bindings_[by_address[rank]];
        if (rank != 0 && bindings_[by_address[rank - 1]].address == binding.address) {
            binding.skip = true;
            continue;
        }
        const ExperienceAddress address(memory_, view_of(binding.address));
        if (store.resolve(address)) {
            (void)CueBindingRecord::decode(store.replay(address), memory_, store);
            binding.skip = true;
        }
    }
}

// SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:398-436
journal::RecordDraft ExperienceAppend::binding_draft(const Binding& binding) const {
    const auto& input = binding_inputs_[binding.input];
    journal::RecordDraft draft;
    draft.kind = cue_binding_kind;
    draft.address = view_of(binding.address);
    draft.source = input.source;
    draft.source_revision = input.source_revision;
    draft.operation_id = operation_id_;
    draft.transaction_id = transaction_id_;
    draft.index = binding.index;
    draft.payload = binding.payload;
    return draft;
}

// SWEGCA: user@2026-09-22:61
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
// SWEGCA: user@2026-09-22:61
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
    } else if (pending_ == 3) {
        // As for heads: another writer's record at a binding's address is
        // the same binding and is decoded to check it.
        for (auto at = pending_from_; at < next_binding_; ++at) {
            auto& binding = bindings_[at];
            if (!binding.staged) continue;
            const ExperienceAddress address(memory_, view_of(binding.address));
            const auto position = store.resolve(address);
            if (!position) {
                next_binding_ = pending_from_;
                break;
            }
            if (position->record_digest != *binding.staged)
                (void)CueBindingRecord::decode(store.replay(address), memory_, store);
            binding.staged.reset();
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
        auto staged = store.stage_experience_records(journal::ExperienceStageKey{},
                                                     drafts, state, views);
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
    if (!drafts.empty()) {
        auto staged = store.stage_experience_records(journal::ExperienceStageKey{},
                                                     drafts, state, views);
        const auto positions = staged.positions();  // in draft order
        for (std::size_t at = 0; at < drafted.size(); ++at)
            heads_[drafted[at]].staged = positions[at].record_digest;
        pending_ = 2;
        pending_from_ = next_head_;
        next_head_ = head_at;
        return staged;
    }
    next_head_ = head_at;

    // The cue bindings, once every memory is published: each names where
    // its target was published (a memory this append gave is published by
    // now; every head generation was confirmed above).
    auto binding_at = next_binding_;
    for (; binding_at < bindings_.size(); ++binding_at) {
        auto& binding = bindings_[binding_at];
        if (binding.skip) continue;
        const ExperienceAddress address(memory_, view_of(binding.address));
        if (store.resolve(address)) {  // bound meanwhile: the same binding (its address digests it)
            (void)CueBindingRecord::decode(store.replay(address), memory_, store);
            continue;
        }
        const auto target = binding_inputs_[binding.input].target;
        const auto target_at = store.resolve(ExperienceAddress(memory_, target));
        if (!target_at) fail("experience_cue_binding_target_unpublished");
        binding.payload = encode_binding(memory_, target, *target_at, binding.cues);
        const auto draft = binding_draft(binding);
        if (!budget.add(draft)) {
            if (drafts.empty()) fail("experience_record_too_large");
            break;
        }
        drafts.push_back(draft);
        drafted.push_back(binding_at);
    }
    if (drafts.empty()) {
        next_binding_ = binding_at;
        done_ = true;
        return std::nullopt;
    }
    auto staged = store.stage_experience_records(journal::ExperienceStageKey{},
                                                 drafts, state, views);
    const auto positions = staged.positions();  // in draft order
    for (std::size_t at = 0; at < drafted.size(); ++at)
        bindings_[drafted[at]].staged = positions[at].record_digest;
    pending_ = 3;
    pending_from_ = next_binding_;
    next_binding_ = binding_at;
    return staged;
}

// SWEGCA: user@2026-09-22:61
ExperienceAppend ExperienceJournal::stage(std::span<const Observation> observations, std::string_view operation_id,
                                          std::optional<std::string_view> transaction_id) const {
    return ExperienceAppend(*this, memory_, observations, {}, operation_id, transaction_id);
}

// SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:398-436
ExperienceAppend ExperienceJournal::stage(std::span<const Observation> observations,
                                          std::span<const CueBinding> bindings, std::string_view operation_id,
                                          std::optional<std::string_view> transaction_id) const {
    return ExperienceAppend(*this, memory_, observations, bindings, operation_id, transaction_id);
}

// SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:137-155
ExperienceRecord ExperienceJournal::replay(const ExperienceAddress& address) const {
    return ExperienceRecord::decode(journal_.replay(address), memory_, journal_);
}

// Each view's key becomes its entry value exactly as `add_automatic` wrote
// it: a cue token as its cue entry, a text by its SHA-256, a digest or an
// address as itself.
// Weak: the user's cold-path address/semantic-key index serving hot
// lookups; the per-view key rules are C++'s.
// SWEGCA: paper/swegca/ARCHITECTURE_SPEC.md@5901a5a:113
void ExperienceJournal::for_each_in_view(ExperienceView view, std::string_view key,
                                         journal::IndexVisitor visit) const {
    const auto kind = static_cast<char>(view);
    switch (view) {
    case ExperienceView::cue:
        if (!is_normalized_cue(memory_, key)) fail("experience_view_key_invalid");
        with_cue_lookup(key, [&](char entry_kind, std::string_view value) {
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

// SWEGCA: paper/swegca/ARCHITECTURE_SPEC.md@5901a5a:141-150
StateGeneration ExperienceJournal::state_generation() const {
    return journal_.state_generation();
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

    // The query text's tokens reach memories and single-word bound cues; a
    // whole bound phrase is reached by that phrase as a cue (the Déjà vu
    // input cues of the four-stage split, not built yet).
    CueTokens tokens(memory, query.text);
    LedgerVector<std::string_view> cues(tokens.tokens().begin(), tokens.tokens().end(),
                                        memory.allocator<std::string_view>());
    std::sort(cues.begin(), cues.end());
    cues.erase(std::unique(cues.begin(), cues.end()), cues.end());

    // A hit on a memory's own entry carries its position; a hit on a cue
    // binding beside it names the memory by the first
    // `experience_address_bytes` of its address, and the memory's position
    // is looked up once for a memory only bindings retrieved.
    struct Hit {
        std::size_t key = 0;  // offset of the memory's address in `keys`
        std::size_t cue = 0;  // which query cue retrieved it
        bool bound = false;   // through a cue binding
        journal::RecordPosition position;
    };
    LedgerBytes keys(memory.allocator<std::byte>());
    LedgerVector<Hit> hits(memory.allocator<Hit>());
    for (std::size_t cue = 0; cue < cues.size(); ++cue) {
        with_cue_lookup(cues[cue], [&](char kind, std::string_view value) {
            const auto collect_hit = [&](std::string_view address,
                                         const journal::RecordPosition& position) {
                if (position.sequence > universe.record_count) return true;  // after U
                const bool bound = address.size() == cue_binding_address_bytes &&
                                   address.substr(address_bytes, cue_binding_infix.size()) == cue_binding_infix;
                if (!bound && address.size() != address_bytes) fail("experience_select_index_inconsistent");
                if (hits.size() >= policy_.max_retrieved) fail("experience_select_over_policy");
                const auto at = keys.size();
                const auto* bytes = reinterpret_cast<const std::byte*>(address.data());
                keys.insert(keys.end(), bytes, bytes + address_bytes);
                hits.push_back(Hit{at, cue, bound, position});
                return true;
            };
            journal.for_each_index_match(kind, value, collect_hit);
        });
    }
    const auto key_of = [&keys](const Hit& hit) {
        return std::string_view(reinterpret_cast<const char*>(keys.data()) + hit.key, address_bytes);
    };
    std::sort(hits.begin(), hits.end(), [&](const Hit& left, const Hit& right) {
        const auto left_key = key_of(left);
        const auto right_key = key_of(right);
        return left_key != right_key ? left_key < right_key : left.cue < right.cue;
    });

    LedgerVector<CandidateJudgment> judgments(memory.allocator<CandidateJudgment>());
    LedgerVector<SelectedExperience> selected(memory.allocator<SelectedExperience>());
    for (std::size_t first = 0; first < hits.size();) {
        std::size_t last = first + 1;
        while (last < hits.size() && key_of(hits[last]) == key_of(hits[first])) ++last;
        const auto address = key_of(hits[first]);
        // Distinct cues (a cue may reach a memory and bindings beside it),
        // and the memory's own position, the same on every direct hit.
        std::uint32_t matched = 0;
        std::optional<journal::RecordPosition> own;
        for (std::size_t same = first; same < last; ++same) {
            if (same == first || hits[same].cue != hits[same - 1].cue) ++matched;
            if (hits[same].bound) continue;
            const auto& position = hits[same].position;
            if (own && (own->sequence != position.sequence || own->record_digest != position.record_digest))
                fail("experience_select_index_inconsistent");
            own = position;
        }
        if (!own) {
            own = journal.resolve(ExperienceAddress(memory, address));
            if (!own || own->sequence > universe.record_count) fail("experience_select_index_inconsistent");
        }
        const auto& position = *own;
        const SelectionCandidate candidate{address, position, matched};
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
