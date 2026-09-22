#pragma once

#include "exact_journal_directory.hpp"
#include "hot_index_projection_log.hpp"
#include "hot_index.hpp"
#include "native_cue_directory.hpp"
#include "native_journal.hpp"

#include <cstdint>

namespace swegca::vrs {

struct NativeHotIndexRebuildCount {
    std::uint64_t journal_rows;
    std::uint64_t distinct_originals;
    std::uint64_t duplicate_observations;
    std::uint64_t non_observation_rows;
    std::uint64_t cue_postings;
    std::uint64_t proposition_postings;
    std::uint64_t successor_postings;
    HotIndexSeed memory;
};

// A second journal-ordered pass routes cue, proposition and supersedes keys
// to 16 independent workers. Every key keeps author journal order, while its
// values come from the verified projection of the first original.
// SWEGCA: src/swegca_vrs2/store.py@7536139:145-175
// SWEGCA: src/swegca_vrs2/cue_shards.py@c06092a:260-360
// SWEGCA: user@2026-09-22:89-92
[[nodiscard]] NativeHotIndexRebuildCount rebuild_native_hot_index_directories(
    const NativeJournal& journal, const ExactJournalDirectory& addresses,
    const HotIndexProjectionLog& headers, NativeCueDirectory& cues,
    NativeCueDirectory& propositions, NativeCueDirectory& successors);

}  // namespace swegca::vrs
