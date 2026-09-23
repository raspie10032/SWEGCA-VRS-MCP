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
constexpr std::uint16_t envelope_version = 1;
// magic, version, observed step, uncertainty and contradiction bits, span
// flag, offset and length, lineage count, raw and structured lengths.
constexpr std::size_t envelope_fixed_bytes = 4 + 2 + 8 + 8 + 8 + 1 + 8 + 8 + 4 + 4 + 4;
constexpr std::size_t max_derived_from = 1024;
constexpr std::size_t address_hex_bytes = 64;
constexpr std::size_t address_bytes = experience_address_prefix.size() + address_hex_bytes;

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
// the listed marks, punctuation and symbol blocks.
// SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:413-418
bool is_cue_letter(char32_t value) noexcept {
    static constexpr std::array<std::pair<char32_t, char32_t>, 16> separators{{
        {0x00d7, 0x00d7},    {0x00f7, 0x00f7},    {0x0300, 0x036f},   {0x2000, 0x2bff},
        {0x2e00, 0x2e7f},    {0x3000, 0x303f},    {0xfe10, 0xfe1f},   {0xfe30, 0xfe6f},
        {0xff00, 0xff20},    {0xff3b, 0xff40},    {0xff5b, 0xff65},   {0xfff0, 0xffff},
        {0x1f000, 0x1faff},  {0xe0000, 0xe007f},  {0xf0000, 0x10ffff}, {0xd800, 0xdfff},
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

// The record identity an address is the digest of: kind, source, revision,
// lineage, outcome, every cue and the payload digest. `cues` visits the cues
// in their (increasing) order.
// SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:31-35
template <class Cues>
std::array<char, address_bytes> address_of(std::uint16_t kind, std::string_view source,
                                           std::string_view source_revision,
                                           const std::optional<std::string_view>& previous,
                                           const std::optional<std::string_view>& outcome,
                                           std::uint64_t cue_count, Cues cues,
                                           const DigestBytes& payload_digest) {
    Sha256 hash;
    hash_field(hash, "swegca.experience_address.v1");
    hash_u64(hash, kind);
    hash_field(hash, source);
    hash_field(hash, source_revision);
    hash_optional(hash, previous);
    hash_optional(hash, outcome);
    hash_u64(hash, cue_count);
    cues([&hash](std::string_view cue) { hash_field(hash, cue); });
    hash.update(payload_digest);
    const auto digest = hash.finish();
    static constexpr char digits[] = "0123456789abcdef";
    std::array<char, address_bytes> out{};
    std::copy(experience_address_prefix.begin(), experience_address_prefix.end(), out.begin());
    auto* hex = out.data() + experience_address_prefix.size();
    for (std::size_t at = 0; at < digest.size(); ++at) {
        const auto byte = std::to_integer<unsigned>(digest[at]);
        hex[at * 2] = digits[byte >> 4];
        hex[at * 2 + 1] = digits[byte & 0x0f];
    }
    return out;
}

// True when `cue` can be a cue of an experience record: a cue text whose
// key with an experience address (all of one size) is an identity text.
// SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:411-431
bool fits_experience_cue(std::string_view cue) noexcept {
    return journal::is_cue_text(cue) &&
           cue.size() + 1 + address_bytes <= detail::identity_text_max_bytes;
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
// contradiction, source span, sorted lineage, raw and structured bytes.
// SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:23-60
LedgerBytes encode_envelope(const MemoryLedger::Account& memory, const Observation& observation,
                            std::span<const std::string_view> derived_from) {
    std::uint64_t total = envelope_fixed_bytes;
    for (const auto address : derived_from) total += 4 + address.size();
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
    writer.u32(static_cast<std::uint32_t>(derived_from.size()));
    for (const auto address : derived_from) writer.text(address, detail::identity_text_max_bytes);
    writer.bytes(observation.raw, journal::max_payload_bytes);
    writer.bytes(observation.structured, journal::max_payload_bytes);
    if (out.size() != total) fail("experience_envelope_size_mismatch");
    return out;
}

// Adds `cue` when it can be a cue of an experience record; a source text
// that cannot (a separator byte, or a key too long) is not a cue, never an
// error, and its tokens still are.
// SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:411-431
void add_automatic_cue(LedgerVector<std::string_view>& cues, std::string_view cue) {
    if (fits_experience_cue(cue)) cues.push_back(cue);
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
ExperienceRecord::ExperienceRecord(journal::PublishedRecord record,
                                   LedgerVector<std::string_view> derived)
    : record_(std::move(record)), derived_from_(std::move(derived)) {}

// SWEGCA: user@2026-09-22:72-79
ExperienceRecord::ExperienceRecord(ExperienceRecord&& other) noexcept
    : record_(std::move(other.record_)), observed_at_(other.observed_at_),
      uncertainty_(other.uncertainty_), contradiction_(other.contradiction_),
      span_(other.span_), derived_from_(std::move(other.derived_from_)),
      raw_(std::exchange(other.raw_, {})), structured_(std::exchange(other.structured_, {})) {}

// Checks the kind, the envelope byte for byte, and that the address is the
// digest of the record's identity (so it was issued by this module).
// SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:23-60
ExperienceRecord ExperienceRecord::decode(journal::PublishedRecord published,
                                          const MemoryLedger::Account& memory) {
    const auto& view = published.view();
    if (view.kind != original_experience_kind && view.kind != derived_experience_kind)
        fail("experience_kind_invalid");
    // An experience documents; it carries no authority and judges no claim.
    if (view.authority || !view.claim.empty()) fail("experience_record_invalid");
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
    const auto derived_count = reader.u32();
    if (derived_count > max_derived_from || (derived_count != 0) != (view.kind == derived_experience_kind))
        fail("experience_envelope_invalid");
    LedgerVector<std::string_view> derived(memory.allocator<std::string_view>());
    derived.reserve(derived_count);
    for (std::uint32_t at = 0; at < derived_count; ++at) {
        const auto address = reader.text_view(detail::identity_text_max_bytes);
        if (!detail::is_identity_text(address) || (at != 0 && !(derived.back() < address)))
            fail("experience_envelope_invalid");
        derived.push_back(address);
    }
    const auto raw = reader.bytes_view(journal::max_payload_bytes);
    const auto structured = reader.bytes_view(journal::max_payload_bytes);
    if (reader.remaining() != 0) fail("experience_envelope_trailing_bytes");

    const auto address = address_of(
        view.kind, view.source, view.source_revision, present(view.previous_revision_address),
        present(view.outcome), view.cue_count,
        [&view](auto&& visit) { journal::for_each_cue(view, visit); }, view.payload_digest);
    if (view.address != view_of(address)) fail("experience_address_mismatch");

    ExperienceRecord out(std::move(published), std::move(derived));
    out.observed_at_ = observed_at;
    out.uncertainty_ = uncertainty;
    out.contradiction_ = contradiction;
    if (has_span == 1) out.span_ = span;
    out.raw_ = raw;
    out.structured_ = structured;
    return out;
}

// Every observation is checked and encoded before anything is staged; a
// failure stages nothing. An observation already in the journal (or earlier
// in `observations`) gets the existing address and no second record.
// SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@cefdc3f:282-283
ExperienceAppend ExperienceJournal::stage(std::span<const Observation> observations,
                                          std::string_view operation_id,
                                          const StateGeneration& state,
                                          std::span<const journal::ViewGeneration> views) const {
    detail::require_identity_text(operation_id, journal::OperationIdTag::name);
    struct Prepared {
        std::uint16_t kind = 0;
        std::array<char, address_bytes> address{};
        LedgerBytes payload;
        CueTokens source_tokens;
        CueTokens revision_tokens;
        LedgerVector<std::string_view> derived_from;
        LedgerVector<std::string_view> cues;
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
        if (observation.derived_from.size() > max_derived_from) fail("experience_lineage_too_long");
        if (observation.semantic_cues.size() > journal::max_record_cues / 2)
            fail("experience_too_many_cues");

        LedgerVector<std::string_view> derived(memory_.allocator<std::string_view>());
        derived.assign(observation.derived_from.begin(), observation.derived_from.end());
        std::sort(derived.begin(), derived.end());
        for (std::size_t at = 0; at < derived.size(); ++at) {
            detail::require_identity_text(derived[at], ExperienceAddressTag::name);
            if (at != 0 && derived[at - 1] == derived[at]) fail("experience_lineage_duplicate");
        }
        auto payload = encode_envelope(memory_, observation, derived);
        const auto kind = derived.empty() ? original_experience_kind : derived_experience_kind;

        // Cues: the source and its revision whole and as tokens, then the
        // Rozephine-authored cues, sorted and unique (the address is part
        // of every key, and all experience addresses have one size).
        CueTokens source_tokens(memory_, observation.source);
        CueTokens revision_tokens(memory_, observation.source_revision);
        LedgerVector<std::string_view> cues(memory_.allocator<std::string_view>());
        cues.reserve(2 + source_tokens.tokens().size() + revision_tokens.tokens().size() +
                     observation.semantic_cues.size());
        add_automatic_cue(cues, observation.source);
        add_automatic_cue(cues, observation.source_revision);
        for (const auto token : source_tokens.tokens()) add_automatic_cue(cues, token);
        for (const auto token : revision_tokens.tokens()) add_automatic_cue(cues, token);
        for (const auto cue : observation.semantic_cues) {
            if (!fits_experience_cue(cue)) fail("experience_cue_invalid");
            cues.push_back(cue);
        }
        std::sort(cues.begin(), cues.end());
        cues.erase(std::unique(cues.begin(), cues.end()), cues.end());
        if (cues.size() > journal::max_record_cues) fail("experience_too_many_cues");

        const auto address = address_of(
            kind, observation.source, observation.source_revision,
            observation.previous_revision_address, observation.outcome, cues.size(),
            [&cues](auto&& visit) {
                for (const auto cue : cues) visit(cue);
            },
            Sha256::of(payload));
        prepared.push_back(Prepared{kind, address, std::move(payload), std::move(source_tokens),
                                    std::move(revision_tokens), std::move(derived), std::move(cues),
                                    false});
        out.addresses.emplace_back(memory_, view_of(address));
    }

    // Lineage must already be published. Of equal addresses in this call
    // only the first is a candidate, and it is appended only when the
    // journal does not hold it yet: O(n log n), one lookup per address.
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
        if (observation.previous_revision_address &&
            !journal_.resolve(ExperienceAddress(memory_, *observation.previous_revision_address)))
            fail("experience_lineage_unknown");
        for (const auto address : item.derived_from)
            if (!journal_.resolve(ExperienceAddress(memory_, address))) fail("experience_lineage_unknown");
        const bool repeat = rank != 0 && prepared[order[rank - 1]].address == item.address;
        item.fresh = !repeat && !journal_.resolve(out.addresses[at]);
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
        draft.cues = item.cues;
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

// Covers the query, context, universe, method, rationale, every judgment
// and every selected experience, in order.
// SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:274-290
Digest256 SelectionReceipt<NoAuthority>::compute_digest() const {
    Sha256 hash;
    hash_field(hash, "swegca.selection_receipt.v1");
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
        hash.update(item.record_digest);
        hash_field(hash, item.verification_state.value());
        hash_field(hash, item.revision.value());
    }
    for (const bool flag : {NoAuthority::external_action_authorized, NoAuthority::memory_write_authorized,
                            NoAuthority::world_write_authorized, NoAuthority::training_write_authorized,
                            NoAuthority::p3_promotion_authorized})
        hash_u64(hash, flag ? 1 : 0);
    return Digest256(hash.finish());
}

// U is read first; retrieval keeps only entries of records in U (the cue
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
        if (!fits_experience_cue(cue)) continue;  // no experience record can carry it
        journal.for_each_cue_match(cue, [&](std::string_view address,
                                            const journal::RecordPosition& position) {
            if (position.sequence > universe.record_count) return true;  // after U
            if (hits.size() >= policy_.max_retrieved) fail("experience_select_over_policy");
            const auto at = keys.size();
            const auto* bytes = reinterpret_cast<const std::byte*>(address.data());
            keys.insert(keys.end(), bytes, bytes + address.size());
            hits.push_back(Hit{at, address.size(), position});
            return true;
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
                fail("experience_select_cue_view_inconsistent");
        const SelectionCandidate candidate{address, position, static_cast<std::uint32_t>(last - first)};
        const auto verdict = judge(candidate, query);
        if (verdict.address != address) fail("experience_judgment_address_changed");
        LedgerVector<EvidenceText> evidence(memory.allocator<EvidenceText>());
        evidence.reserve(verdict.rejection_evidence.size());
        for (const auto item : verdict.rejection_evidence) evidence.emplace_back(memory, item);
        CandidateJudgment judgment{ExperienceAddress(memory, address),
                                   position,
                                   candidate.matched_cues,
                                   verdict.selected,
                                   unit_fraction(verdict.relevance, "experience_relevance_invalid"),
                                   unit_fraction(verdict.contradiction, "experience_contradiction_invalid"),
                                   VerificationState(memory, verdict.verification_state),
                                   RevisionText(memory, verdict.revision),
                                   Rationale(memory, verdict.rationale),
                                   std::move(evidence)};
        if (judgment.selected) {
            const auto record = experience_.replay(judgment.address);
            if (record.position().sequence != position.sequence ||
                record.record().record_digest != position.record_digest)
                fail("experience_select_position_changed");
            selected.push_back(SelectedExperience{ExperienceAddress(memory, address), position,
                                                  record.raw().size(), position.record_digest,
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
