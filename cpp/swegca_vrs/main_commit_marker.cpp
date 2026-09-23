#include "swegca_vrs/main_commit_marker.hpp"

#include "swegca_vrs/core_sha256.hpp"

#include <algorithm>
#include <stdexcept>

namespace swegca::vrs {
namespace {

constexpr std::array<std::byte, 4> magic{
    std::byte{'S'}, std::byte{'W'}, std::byte{'M'}, std::byte{'C'}};
constexpr std::uint16_t version = 1;
constexpr std::size_t checksum_offset = main_commit_marker_bytes - digest256_width;

// Lineage: native mechanism — invalid receipt bytes cannot select Main state.
// SWEGCA: src/tinylm_slicer/mosaic_paper_resident_assimilation.py@3bddcb7:491-535
[[noreturn]] void invalid() { throw std::invalid_argument("main_commit_marker_invalid"); }

// SWEGCA: user@2026-09-22:60-61
void put_u64(std::byte*& out, std::uint64_t value) noexcept {
    for (std::size_t at = 0; at < 8; ++at)
        *out++ = static_cast<std::byte>((value >> (8 * at)) & 0xff);
}

// SWEGCA: user@2026-09-22:60-61
std::uint64_t get_u64(const std::byte*& at) noexcept {
    std::uint64_t value = 0;
    for (std::size_t shift = 0; shift < 64; shift += 8)
        value |= static_cast<std::uint64_t>(std::to_integer<unsigned>(*at++)) << shift;
    return value;
}

// Lineage: native mechanism — a receipt can name only complete, nonzero roots.
// SWEGCA: src/tinylm_slicer/mosaic_paper_resident_assimilation.py@3bddcb7:491-535
void check(const MainCommitMarkerFields& fields) {
    const auto& location = fields.journal_root.location;
    if (fields.commit_sequence == 0 ||
        (fields.commit_sequence == 1) !=
            (fields.previous_marker_digest == DigestBytes{}) ||
        location.log_ordinal == 0 || location.length == 0 ||
        fields.journal_root.manifest_digest == DigestBytes{} ||
        fields.state_head.content_digest == DigestBytes{} ||
        !fields.state_head.publication ||
        fields.strength_root_digest == DigestBytes{})
        invalid();
    const auto& position = *fields.state_head.publication;
    if (position.segment_ordinal == 0 || position.byte_offset == 0 ||
        position.sequence == 0 || position.record_digest == DigestBytes{})
        invalid();
}

}  // namespace

// Lineage: native mechanism — one fixed receipt binds the exact selected roots.
// SWEGCA: src/tinylm_slicer/mosaic_paper_resident_assimilation.py@3bddcb7:491-535
std::array<std::byte, main_commit_marker_bytes> encode_main_commit_marker(
    const MainCommitMarkerFields& fields) {
    check(fields);
    std::array<std::byte, main_commit_marker_bytes> out{};
    auto* at = std::copy(magic.begin(), magic.end(), out.data());
    *at++ = static_cast<std::byte>(version & 0xff);
    *at++ = static_cast<std::byte>(version >> 8);
    put_u64(at, fields.commit_sequence);
    at = std::copy(fields.previous_marker_digest.begin(),
                   fields.previous_marker_digest.end(), at);
    const auto& location = fields.journal_root.location;
    put_u64(at, location.log_ordinal);
    put_u64(at, location.offset);
    put_u64(at, location.length);
    at = std::copy(fields.journal_root.manifest_digest.begin(),
                   fields.journal_root.manifest_digest.end(), at);
    at = std::copy(fields.state_head.content_digest.begin(),
                   fields.state_head.content_digest.end(), at);
    const auto& position = *fields.state_head.publication;
    put_u64(at, position.segment_ordinal);
    put_u64(at, position.byte_offset);
    put_u64(at, position.sequence);
    at = std::copy(position.record_digest.begin(), position.record_digest.end(), at);
    at = std::copy(fields.strength_root_digest.begin(),
                   fields.strength_root_digest.end(), at);
    if (at != out.data() + checksum_offset) invalid();
    const auto checksum = Sha256::of(
        std::span<const std::byte>(out.data(), checksum_offset));
    std::copy(checksum.begin(), checksum.end(), at);
    return out;
}

// Lineage: native mechanism — malformed bytes fail before Main can inspect a named root.
// SWEGCA: src/tinylm_slicer/mosaic_paper_resident_assimilation.py@3bddcb7:491-535
MainCommitMarkerFields decode_main_commit_marker(std::span<const std::byte> payload) {
    if (payload.size() != main_commit_marker_bytes) invalid();
    const auto checksum = Sha256::of(payload.first(checksum_offset));
    if (!std::equal(magic.begin(), magic.end(), payload.begin()) ||
        payload[4] != static_cast<std::byte>(version & 0xff) ||
        payload[5] != static_cast<std::byte>(version >> 8) ||
        !std::equal(checksum.begin(), checksum.end(),
                    payload.begin() + checksum_offset))
        invalid();
    const auto* at = payload.data() + 6;
    MainCommitMarkerFields out;
    out.commit_sequence = get_u64(at);
    std::copy_n(at, digest256_width, out.previous_marker_digest.begin());
    at += digest256_width;
    out.journal_root.location.log_ordinal = get_u64(at);
    out.journal_root.location.offset = get_u64(at);
    out.journal_root.location.length = get_u64(at);
    std::copy_n(at, digest256_width, out.journal_root.manifest_digest.begin());
    at += digest256_width;
    std::copy_n(at, digest256_width, out.state_head.content_digest.begin());
    at += digest256_width;
    journal::RecordPosition position;
    position.segment_ordinal = get_u64(at);
    position.byte_offset = get_u64(at);
    position.sequence = get_u64(at);
    std::copy_n(at, digest256_width, position.record_digest.begin());
    at += digest256_width;
    out.state_head.publication = position;
    std::copy_n(at, digest256_width, out.strength_root_digest.begin());
    at += digest256_width;
    if (at != payload.data() + checksum_offset) invalid();
    check(out);
    return out;
}

// Lineage: native mechanism — a successor receipt binds exact predecessor bytes.
// SWEGCA: src/tinylm_slicer/mosaic_paper_resident_assimilation.py@3bddcb7:491-535
DigestBytes main_commit_marker_digest(std::span<const std::byte> payload) {
    (void)decode_main_commit_marker(payload);
    return Sha256::of(payload);
}

// Lineage: native mechanism — Main's receipt must name the actual published journal HEAD.
// SWEGCA: src/tinylm_slicer/mosaic_paper_resident_assimilation.py@3bddcb7:491-535
void validate_main_marker_journal_binding(const MainCommitMarkerFields& marker,
                                          const journal::JournalStore& store) {
    check(marker);
    const auto current = store.publication_coordinates();
    const auto& named = marker.journal_root.location;
    const auto& actual = current.root.location;
    if (named.log_ordinal != actual.log_ordinal || named.offset != actual.offset ||
        named.length != actual.length ||
        marker.journal_root.manifest_digest != current.root.manifest_digest ||
        marker.state_head != current.state_head)
        throw std::runtime_error("main_commit_marker_journal_mismatch");
}

}  // namespace swegca::vrs
