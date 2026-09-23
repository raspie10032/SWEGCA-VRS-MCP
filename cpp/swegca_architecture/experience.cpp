#include "swegca_architecture/experience.hpp"

#include "swegca_architecture/sha256.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
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
constexpr std::uint16_t envelope_version = 2;
// magic, version, observed step, uncertainty and contradiction bits, span
// flag, offset and length, namespace flag, lineage count, resource count,
// raw and structured lengths (a present namespace adds its text).
constexpr std::size_t envelope_fixed_bytes = 4 + 2 + 8 + 8 + 8 + 1 + 8 + 8 + 1 + 4 + 4 + 4 + 4;
constexpr std::size_t max_derived_from = 1024;
constexpr std::size_t max_resources = 1024;
constexpr std::size_t max_semantic_cues = 4096;
constexpr std::size_t digest_hex_bytes = 64;
constexpr std::size_t address_bytes = experience_address_prefix.size() + digest_hex_bytes;
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
bool is_single_token(const MemoryLedger::Account& memory, std::string_view text) {
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
    explicit IndexEntries(const MemoryLedger::Account& memory)
        : bytes_(memory.allocator<std::byte>()),
          spans_(memory.allocator<std::pair<std::size_t, std::size_t>>()),
          entries_(memory.allocator<std::string_view>()) {}

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
void add_automatic(IndexEntries& index, const MemoryLedger::Account& memory, const IndexFields& fields) {
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
    constexpr auto transaction = static_cast<char>(ExperienceView::transaction);
    const auto skip = [](std::span<const std::string_view> entries, std::size_t& at) {
        while (at < entries.size() && entries[at].front() == transaction) ++at;
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

// An entry no field derives: only an authored cue can be one.
// SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:411-431
bool is_authored_cue_entry(const MemoryLedger::Account& memory, std::string_view entry) {
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
std::string_view view_of(const std::array<char, address_bytes>& address) noexcept {
    return std::string_view(address.data(), address.size());
}

// A record text as an optional: empty on disk means absent.
// SWEGCA: user@2026-09-22:60-61
std::optional<std::string_view> present(std::string_view text) noexcept {
    return text.empty() ? std::nullopt : std::optional<std::string_view>(text);
}

// The experience payload: the observation's step, uncertainty,
// contradiction, source span, namespace, sorted lineage, sorted resources,
// raw and structured bytes.
// SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:23-60
LedgerBytes encode_envelope(const MemoryLedger::Account& memory, const Observation& observation,
                            std::span<const std::string_view> derived_from,
                            std::span<const std::string_view> resources) {
    std::uint64_t total = envelope_fixed_bytes;
    if (observation.name_space) total += 4 + observation.name_space->size();
    for (const auto address : derived_from) total += 4 + address.size();
    for (const auto resource : resources) total += 4 + resource.size();
    total += observation.raw.size();
    total += observation.structured.size();
    if (total > journal::max_payload_bytes) fail("experience_too_large");
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
    writer.u8(observation.name_space ? 1 : 0);
    if (observation.name_space) writer.text(*observation.name_space, detail::identity_text_max_bytes);
    writer.u32(static_cast<std::uint32_t>(derived_from.size()));
    for (const auto address : derived_from) writer.text(address, detail::identity_text_max_bytes);
    writer.u32(static_cast<std::uint32_t>(resources.size()));
    for (const auto resource : resources) writer.text(resource, detail::identity_text_max_bytes);
    writer.bytes(observation.raw, journal::max_payload_bytes);
    writer.bytes(observation.structured, journal::max_payload_bytes);
    if (out.size() != total) fail("experience_envelope_size_mismatch");
    return out;
}

// Reads `count` identity texts, strictly increasing, each accepted by `valid`.
// SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:23-60
template <class Valid>
LedgerVector<std::string_view> read_sorted_texts(ByteReader& reader, std::uint32_t count,
                                                 const MemoryLedger::Account& memory, Valid valid) {
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

}  // namespace

// SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:413-418
CueTokens::CueTokens(const MemoryLedger::Account& memory, std::string_view source)
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
ExperienceRecord::ExperienceRecord(journal::PublishedRecord record, LedgerVector<std::string_view> derived,
                                   LedgerVector<std::string_view> resources,
                                   LedgerVector<std::string_view> index)
    : record_(std::move(record)), derived_from_(std::move(derived)), resources_(std::move(resources)),
      index_(std::move(index)) {}

// SWEGCA: user@2026-09-22:72-79
ExperienceRecord::ExperienceRecord(ExperienceRecord&& other) noexcept
    : record_(std::move(other.record_)), observed_at_(other.observed_at_),
      uncertainty_(other.uncertainty_), contradiction_(other.contradiction_),
      span_(other.span_), derived_from_(std::move(other.derived_from_)),
      name_space_(std::exchange(other.name_space_, std::nullopt)),
      resources_(std::move(other.resources_)), index_(std::move(other.index_)),
      raw_(std::exchange(other.raw_, {})), raw_digest_(other.raw_digest_),
      structured_(std::exchange(other.structured_, {})) {}

// Checks the kind, the envelope byte for byte, that the address is the
// digest of the record's identity (so it was issued by this module), and
// that the index entries are exactly the automatic ones plus authored cues.
// SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:23-60
ExperienceRecord ExperienceRecord::decode(journal::PublishedRecord published,
                                          const MemoryLedger::Account& memory) {
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
    const auto raw = reader.bytes_view(journal::max_payload_bytes);
    const auto structured = reader.bytes_view(journal::max_payload_bytes);
    if (reader.remaining() != 0) fail("experience_envelope_trailing_bytes");

    const auto address = address_of(view.kind, view.source, view.source_revision,
                                     present(view.previous_revision_address), present(view.outcome),
                                     view.payload_digest);
    if (view.address != view_of(address)) fail("experience_address_mismatch");

    // The record's entries against the automatic ones: every automatic one
    // present, and every other one an authored cue.
    const auto raw_digest = Sha256::of(raw);
    LedgerVector<std::string_view> index(memory.allocator<std::string_view>());
    index.reserve(view.index_count);
    journal::for_each_index_entry(view, [&index](std::string_view entry) { index.push_back(entry); });
    IndexEntries automatic(memory);
    add_automatic(automatic, memory,
                  IndexFields{view.source, view.source_revision, present(view.previous_revision_address),
                              derived, name_space, resources, raw_digest, present(view.transaction_id)});
    const auto expected = automatic.finish();
    std::size_t next = 0;
    for (const auto entry : index) {
        if (next < expected.size() && expected[next] == entry) {
            ++next;
            continue;
        }
        if (next < expected.size() && expected[next] < entry) fail("experience_index_incomplete");
        if (!is_authored_cue_entry(memory, entry)) fail("experience_index_invalid");
    }
    if (next != expected.size()) fail("experience_index_incomplete");

    ExperienceRecord out(std::move(published), std::move(derived), std::move(resources), std::move(index));
    out.observed_at_ = observed_at;
    out.uncertainty_ = uncertainty;
    out.contradiction_ = contradiction;
    if (has_span == 1) out.span_ = span;
    out.name_space_ = name_space;
    out.raw_ = raw;
    out.raw_digest_ = raw_digest;
    out.structured_ = structured;
    return out;
}

// Every observation is checked and encoded before anything is staged; a
// failure stages nothing. An observation already in the journal (or earlier
// in `observations`) gets the existing address and no second record, and
// must carry the same index entries apart from the transaction.
// SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@cefdc3f:282-283
ExperienceAppend ExperienceJournal::stage(std::span<const Observation> observations,
                                          std::string_view operation_id,
                                          std::optional<std::string_view> transaction_id,
                                          const StateGeneration& state,
                                          std::span<const journal::ViewGeneration> views) const {
    detail::require_identity_text(operation_id, journal::OperationIdTag::name);
    if (transaction_id) detail::require_identity_text(*transaction_id, TransactionIdTag::name);
    struct Prepared {
        std::uint16_t kind = 0;
        std::array<char, address_bytes> address{};
        LedgerBytes payload;
        LedgerVector<std::string_view> derived_from;
        IndexEntries index;
        bool fresh = false;  // appended by this generation
    };
    LedgerVector<Prepared> prepared(memory_.allocator<Prepared>());
    prepared.reserve(observations.size());
    ExperienceAppend out{std::nullopt, LedgerVector<ExperienceAddress>(memory_.allocator<ExperienceAddress>())};
    out.addresses.reserve(observations.size());

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

        auto payload = encode_envelope(memory_, observation, derived, resources);
        const auto kind = derived.empty() ? original_experience_kind : derived_experience_kind;
        const auto raw_digest = Sha256::of(observation.raw);
        IndexEntries index(memory_);
        add_automatic(index, memory_,
                      IndexFields{observation.source, observation.source_revision,
                                  observation.previous_revision_address, derived, observation.name_space,
                                  resources, raw_digest, transaction_id});
        for (const auto cue : observation.semantic_cues) index.add_cue(cue);
        (void)index.finish();

        const auto address = address_of(kind, observation.source, observation.source_revision,
                                        observation.previous_revision_address, observation.outcome,
                                        Sha256::of(payload));
        prepared.push_back(Prepared{kind, address, std::move(payload), std::move(derived), std::move(index), false});
        out.addresses.emplace_back(memory_, view_of(address));
    }

    // Lineage must already be published experience. Of equal addresses in
    // this call only the first is a candidate, and it is appended only when
    // the journal does not hold it yet: O(n log n), one lookup per address.
    const auto require_experience = [this](std::string_view address) {
        const ExperienceAddress lineage(memory_, address);
        if (!journal_.resolve(lineage)) fail("experience_lineage_unknown");
        (void)ExperienceRecord::decode(journal_.replay(lineage), memory_);
    };
    LedgerVector<std::size_t> order(memory_.allocator<std::size_t>());
    order.reserve(observations.size());
    for (std::size_t at = 0; at < observations.size(); ++at) order.push_back(at);
    std::sort(order.begin(), order.end(), [&prepared](std::size_t left, std::size_t right) {
        return prepared[left].address != prepared[right].address
                   ? prepared[left].address < prepared[right].address
                   : left < right;
    });
    for (std::size_t rank = 0; rank < order.size(); ++rank) {
        const auto at = order[rank];
        auto& item = prepared[at];
        const auto& observation = observations[at];
        if (observation.previous_revision_address) require_experience(*observation.previous_revision_address);
        for (const auto address : item.derived_from) require_experience(address);
        if (rank != 0 && prepared[order[rank - 1]].address == item.address) {
            if (!same_observed_index(prepared[order[rank - 1]].index.entries(), item.index.entries()))
                fail("experience_index_conflict");
            continue;
        }
        if (journal_.resolve(out.addresses[at])) {
            const auto existing = ExperienceRecord::decode(journal_.replay(out.addresses[at]), memory_);
            if (!same_observed_index(existing.index_entries(), item.index.entries()))
                fail("experience_index_conflict");
            continue;
        }
        item.fresh = true;
    }

    LedgerVector<journal::RecordDraft> drafts(memory_.allocator<journal::RecordDraft>());
    drafts.reserve(observations.size());
    for (std::size_t at = 0; at < observations.size(); ++at) {
        const auto& item = prepared[at];
        if (!item.fresh) continue;
        const auto& observation = observations[at];
        journal::RecordDraft draft;
        draft.kind = item.kind;
        draft.address = view_of(item.address);
        draft.source = observation.source;
        draft.source_revision = observation.source_revision;
        draft.previous_revision_address = observation.previous_revision_address;
        draft.outcome = observation.outcome;
        draft.operation_id = operation_id;
        draft.transaction_id = transaction_id;
        draft.index = item.index.entries();
        draft.payload = item.payload;
        drafts.push_back(draft);
    }
    if (!drafts.empty()) out.staged.emplace(journal_.stage(drafts, state, views));
    return out;
}

// SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:137-155
ExperienceRecord ExperienceJournal::replay(const ExperienceAddress& address) const {
    return ExperienceRecord::decode(journal_.replay(address), memory_);
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
    hash_field(hash, "swegca.selection_receipt.v2");
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
                                                  record.raw().size(), record.raw_digest(),
                                                  position.record_digest, judgment.verification_state,
                                                  judgment.revision});
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
