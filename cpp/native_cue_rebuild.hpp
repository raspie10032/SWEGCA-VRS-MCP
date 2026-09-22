#pragma once

#include "exact_journal_directory.hpp"
#include "hot_index_projection_log.hpp"
#include "native_cue_directory.hpp"
#include "native_journal.hpp"

#include <cstdint>

namespace swegca::vrs {

struct NativeCueRebuildCount {
    std::uint64_t journal_rows;
    std::uint64_t distinct_originals;
    std::uint64_t duplicate_observations;
    std::uint64_t distinct_postings;
};

// A second journal-ordered pass routes each cue to one of 16 independent
// workers. One cue's node sequence stays in author journal order, while its
// raw posting value comes from the verified projection of the first original.
// SWEGCA: src/swegca_vrs2/store.py@7536139:145-175
// SWEGCA: src/swegca_vrs2/cue_shards.py@c06092a:260-360
// SWEGCA: user@2026-09-22:89-92
[[nodiscard]] NativeCueRebuildCount rebuild_native_cue_directory(
    const NativeJournal& journal, const ExactJournalDirectory& addresses,
    const HotIndexProjectionLog& headers, NativeCueDirectory& cues);

}  // namespace swegca::vrs
