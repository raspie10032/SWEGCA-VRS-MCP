#pragma once

#include "memory_episode.hpp"
#include "native_journal.hpp"

#include <cstdint>
#include <string_view>

namespace swegca::vrs {

// A derived exact address into the original native VRS journal. The frame is
// opened only after Recall has chosen this original for Replay.
// SWEGCA: user@2026-09-22:24-25
struct OriginalJournalAddress {
    JournalFrameAddress frame;
    std::int64_t sequence;
};

// The native journal remains the sole original body. Validate its recorded
// observation fingerprint and requested episode identity before construction.
// SWEGCA: src/swegca_vrs2/store.py@7536139:340-349
// SWEGCA: user@2026-09-22:24-25
[[nodiscard]] MemoryEpisode replay_original_from_journal(
    const NativeJournal& journal, const OriginalJournalAddress& address,
    std::string_view expected_episode_id);

}  // namespace swegca::vrs
