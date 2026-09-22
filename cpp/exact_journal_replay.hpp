#pragma once

#include "exact_journal_directory.hpp"
#include "original_journal_replay.hpp"

#include <cstdint>
#include <string_view>

namespace swegca::vrs {

// The selected Replay obtains one original address from the generation-bound
// VRS directory, then opens that original from the native journal. The pair's
// published sequence limit prevents a later write from leaking to this read.
// SWEGCA: src/swegca_vrs2/exact_replay.py@c06092a:691-786
// SWEGCA: user@2026-09-22:24-25
[[nodiscard]] MemoryEpisode replay_exact_journal_original(
    const NativeJournal& journal, const ExactJournalDirectory& addresses,
    std::string_view episode_id, std::int64_t published_row_limit);

// SWEGCA: src/swegca_vrs2/exact_replay.py@c06092a:691-786
// SWEGCA: user@2026-09-22:24-25
[[nodiscard]] MemoryEpisode replay_exact_journal_original(
    const NativeJournalReadView& journal, const ExactJournalDirectory& addresses,
    std::string_view episode_id, std::int64_t published_row_limit);

}  // namespace swegca::vrs
