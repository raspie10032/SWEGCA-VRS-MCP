#include "swegca_vrs/state_publication_codec.hpp"

#include "swegca_vrs/core_sha256.hpp"

#include <algorithm>
#include <cstdint>
#include <stdexcept>

namespace swegca::vrs {
namespace {

constexpr std::array<std::byte, 4> publication_magic{
    std::byte{'S'}, std::byte{'W'}, std::byte{'S'}, std::byte{'H'}};
constexpr std::uint16_t publication_version = 1;
constexpr std::byte no_predecessor{0};
constexpr std::byte genesis_tag{0};

// Lineage: native mechanism — one failure for an invalid native publication record.
// SWEGCA: src/tinylm_slicer/mosaic_paper_resident_assimilation.py@3bddcb7:491-535
[[noreturn]] void invalid() { throw std::invalid_argument("state_publication_invalid"); }

}  // namespace

// Lineage: native mechanism — this fixed payload encodes a Main-owned genesis selection.
// SWEGCA: src/tinylm_slicer/mosaic_paper_resident_assimilation.py@3bddcb7:491-535
std::array<std::byte, genesis_state_publication_payload_bytes>
encode_genesis_state_publication(const DigestBytes& content_digest) {
    if (content_digest == DigestBytes{}) invalid();
    std::array<std::byte, genesis_state_publication_payload_bytes> out{};
    auto* at = std::copy(publication_magic.begin(), publication_magic.end(), out.data());
    *at++ = static_cast<std::byte>(publication_version & 0xff);
    *at++ = static_cast<std::byte>(publication_version >> 8);
    at = std::copy(content_digest.begin(), content_digest.end(), at);
    *at++ = no_predecessor;
    *at = genesis_tag;
    return out;
}

// Lineage: native mechanism — reject a body that cannot be a genesis selection.
// SWEGCA: src/tinylm_slicer/mosaic_paper_resident_assimilation.py@3bddcb7:491-535
DigestBytes decode_genesis_state_publication(std::span<const std::byte> payload) {
    if (payload.size() != genesis_state_publication_payload_bytes ||
        !std::equal(publication_magic.begin(), publication_magic.end(), payload.begin()) ||
        payload[4] != static_cast<std::byte>(publication_version & 0xff) ||
        payload[5] != static_cast<std::byte>(publication_version >> 8) ||
        payload[genesis_state_publication_payload_bytes - 2] != no_predecessor ||
        payload[genesis_state_publication_payload_bytes - 1] != genesis_tag)
        invalid();
    DigestBytes content_digest{};
    std::copy_n(payload.begin() + 6, digest256_width, content_digest.begin());
    if (content_digest == DigestBytes{}) invalid();
    return content_digest;
}

// Lineage: native mechanism — the kind-7 address binds its full immutable payload.
// SWEGCA: src/tinylm_slicer/mosaic_paper_resident_assimilation.py@3bddcb7:491-535
std::array<char, state_publication_address_bytes>
genesis_state_publication_address(const DigestBytes& content_digest) {
    const auto payload = encode_genesis_state_publication(content_digest);
    const auto digest = Sha256::of(payload);
    constexpr std::string_view hex = "0123456789abcdef";
    std::array<char, state_publication_address_bytes> out{};
    auto at = std::copy(state_publication_address_prefix.begin(),
                        state_publication_address_prefix.end(), out.begin());
    for (const auto byte : digest) {
        const auto value = std::to_integer<unsigned>(byte);
        *at++ = hex[value >> 4];
        *at++ = hex[value & 0x0f];
    }
    return out;
}

}  // namespace swegca::vrs
