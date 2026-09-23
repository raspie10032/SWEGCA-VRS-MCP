#pragma once

#include "swegca_vrs/core_digest.hpp"

#include <array>
#include <cstddef>
#include <span>
#include <string_view>

// The kind-7 publication record's genesis form. This codec only describes
// bytes; JournalStore's Main-only stage key and Main's selected durable marker
// decide whether a record is published as authority. Later transition bodies
// remain unsupported until their predecessor and receipt checks are defined.
// Lineage: native mechanism — the author's commit receipt selects a Main-owned generation; this fixed byte format is new.
// SWEGCA: src/tinylm_slicer/mosaic_paper_resident_assimilation.py@3bddcb7:491-535
namespace swegca::vrs {

inline constexpr std::string_view state_publication_address_prefix = "state-publication:";
inline constexpr std::size_t state_publication_address_bytes =
    state_publication_address_prefix.size() + 2 * digest256_width;
inline constexpr std::size_t genesis_state_publication_payload_bytes =
    4 + 2 + digest256_width + 1 + 1;

// Exact SWSH/v1 bytes: magic, version, real initial content digest,
// has_predecessor=0, body_tag=0. The zero digest is the lower journal's
// uninitialized sentinel and cannot name a published initial state.
// Lineage: native mechanism — the source receipt's immutable selection is encoded as a kind-7 record.
// SWEGCA: src/tinylm_slicer/mosaic_paper_resident_assimilation.py@3bddcb7:491-535
[[nodiscard]] std::array<std::byte, genesis_state_publication_payload_bytes>
encode_genesis_state_publication(const DigestBytes& content_digest);

// Accepts only the exact genesis form above. Unknown transition tags and a
// predecessor on genesis fail closed before Main can select the record.
// Lineage: native mechanism — a candidate without a supported transition cannot become Main's selected state.
// SWEGCA: src/tinylm_slicer/mosaic_paper_resident_assimilation.py@3bddcb7:491-535
[[nodiscard]] DigestBytes decode_genesis_state_publication(
    std::span<const std::byte> payload);

// Kind-7's address binds every byte of the payload, including its tag.
// The returned fixed array is not NUL-terminated; use its full span.
// Lineage: native mechanism — a content address keeps the native commit body immutable.
// SWEGCA: src/tinylm_slicer/mosaic_paper_resident_assimilation.py@3bddcb7:491-535
[[nodiscard]] std::array<char, state_publication_address_bytes>
genesis_state_publication_address(const DigestBytes& content_digest);

}  // namespace swegca::vrs
