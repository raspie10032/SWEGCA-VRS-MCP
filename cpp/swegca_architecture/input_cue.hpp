#pragma once

#include "swegca_architecture/sha256.hpp"

namespace swegca::architecture {

// The recorded dialogue context key. Computed at session setup, never an
// extra name hash before each Recall. This is identity, not semantic evidence.
[[nodiscard]] inline DigestBytes input_session_context(const DigestBytes& owner,std::string_view session) {
    // The fixed-width owner binds provider/instance/session provenance. Equal
    // display names from independent stores do not imply shared dialogue.
    Sha256 digest;digest.update("SWEGCA input session v2");digest.update(owner);
    digest.update(session);return digest.finish();
}

// Content addressing only: an exact cue carries no support/refutation verdict.
// Framing prevents media/content boundary ambiguity; all bytes are retained.
[[nodiscard]] inline Sha256 input_cue_prefix(std::uint64_t media_bytes) {
    Sha256 digest;
    digest.update("SWEGCA original input cue v1");
    std::array<std::byte, 8> length{};
    for (unsigned i = 0; i < 8; ++i) length[i] = std::byte((media_bytes >> (8 * i)) & 255);
    digest.update(length); return digest;
}
[[nodiscard]] inline DigestBytes input_cue(std::string_view media, std::span<const std::byte> content) {
    auto digest=input_cue_prefix(media.size());
    digest.update(media); digest.update(content);
    return digest.finish();
}

}  // namespace swegca::architecture
