#include "exact_journal_rebuild.hpp"

#include "digest.hpp"
#include "observation.hpp"

#include <stdexcept>
#include <utility>

namespace swegca::vrs {

// SWEGCA: src/swegca_vrs2/store.py@7536139:340-349
// SWEGCA: src/swegca_vrs2/exact_replay.py@c06092a:496-609
ExactJournalRebuildCount rebuild_exact_journal_directory(
    const NativeJournal& journal, ExactJournalDirectory& addresses) {
    if (journal.generation() != addresses.journal_generation())
        throw std::runtime_error("exact_journal_generation_changed");
    if (!addresses.fresh())
        throw std::runtime_error("exact_journal_rebuild_requires_fresh_directory");
    ExactJournalRebuildCount count{};
    journal.visit_addressed_rows([&](JournalRow&& stored,
                                     const JournalFrameAddress& frame) {
        const auto row = observation(Json::parse(stored.body));
        if (row.at("request_id").string() != stored.request_id ||
            sha256_hex(row.canonical()) != stored.fingerprint)
            throw std::runtime_error("stored_observation_integrity_failed");
        auto episode = episode_from_observation(row);
        ++count.journal_rows;
        const auto previous = addresses.find(episode.episode_id, stored.sequence - 1);
        if (previous) {
            ++count.duplicate_observations;
            return;
        }
        addresses.put(episode.episode_id,
                      OriginalJournalAddress{frame, stored.sequence});
        ++count.distinct_originals;
    });
    if (count.journal_rows != journal.row_count())
        throw std::runtime_error("exact_journal_rebuild_row_count_changed");
    return count;
}

}  // namespace swegca::vrs
