#pragma once

#include "swegca_vrs/journal_store.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

// Main's append-only receipt payload. It binds one exact journal manifest,
// one state publication record and one durable VRS strength root. This codec
// grants no authority: only Main may verify the predecessor chain, open the
// named root, reconstruct state and strength, and select a committed filename.
// The receipt file and its pending/committed names live outside the journal.
namespace swegca::vrs {

inline constexpr std::size_t main_commit_marker_bytes =
    4 + 2 + 8 + digest256_width + 3 * 8 + digest256_width +
    digest256_width + 3 * 8 + digest256_width + digest256_width + digest256_width;

struct MainCommitMarkerFields final {
    std::uint64_t commit_sequence = 0;
    DigestBytes previous_marker_digest{};  // zero only for genesis
    journal::JournalRoot journal_root;
    journal::StateHeadReference state_head;
    DigestBytes strength_root_digest{};
};

// Exact SWMC/v1 bytes, ending in SHA-256 of every preceding byte. The
// checksum detects a torn or modified file before any field can be selected.
// Lineage: native mechanism — a Main receipt binds the replacement pair before rename.
// SWEGCA: src/tinylm_slicer/mosaic_paper_resident_assimilation.py@3bddcb7:491-535
[[nodiscard]] std::array<std::byte, main_commit_marker_bytes> encode_main_commit_marker(
    const MainCommitMarkerFields& fields);

// Requires the one encoding and structurally valid fields. It does not
// validate the committed filename, predecessor receipt, journal or strength.
// Lineage: native mechanism — a marker is data until Main verifies its sources.
// SWEGCA: src/tinylm_slicer/mosaic_paper_resident_assimilation.py@3bddcb7:491-535
[[nodiscard]] MainCommitMarkerFields decode_main_commit_marker(
    std::span<const std::byte> payload);

// The digest named by the next marker's predecessor field. Accepts only a
// canonical, fully checked marker payload.
// Lineage: native mechanism — append-only predecessor identity for Main receipts.
// SWEGCA: src/tinylm_slicer/mosaic_paper_resident_assimilation.py@3bddcb7:491-535
[[nodiscard]] DigestBytes main_commit_marker_digest(std::span<const std::byte> payload);

// Requires the marker's journal locator and state head to equal one published
// JournalStore snapshot. This is only a binding check: Main still verifies
// the selected marker chain, exact state record, state bytes and strength.
// Lineage: native mechanism — the receipt binds one replacement pair.
// SWEGCA: src/tinylm_slicer/mosaic_paper_resident_assimilation.py@3bddcb7:491-535
void validate_main_marker_journal_binding(const MainCommitMarkerFields& marker,
                                          const journal::JournalStore& store);

}  // namespace swegca::vrs
