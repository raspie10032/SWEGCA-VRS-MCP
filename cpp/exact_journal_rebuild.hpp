#pragma once

#include "exact_journal_directory.hpp"
#include "native_journal.hpp"

#include <cstdint>

namespace swegca::vrs {

struct ExactJournalRebuildCount {
    std::uint64_t journal_rows;
    std::uint64_t distinct_originals;
    std::uint64_t duplicate_observations;
};

// Rebuild a derived exact-address directory from one already validated active
// native journal generation. Main publishes no reader of this directory until
// the complete journal walk succeeds and its pair generation is checked.
// SWEGCA: src/swegca_vrs2/store.py@7536139:340-349
// SWEGCA: src/swegca_vrs2/exact_replay.py@c06092a:496-609
[[nodiscard]] ExactJournalRebuildCount rebuild_exact_journal_directory(
    const NativeJournal& journal, ExactJournalDirectory& addresses,
    HotIndexProjectionLog& headers);

}  // namespace swegca::vrs
