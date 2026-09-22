#include "exact_journal_replay.hpp"

#include <stdexcept>

namespace swegca::vrs {

// SWEGCA: src/swegca_vrs2/exact_replay.py@c06092a:691-786
// SWEGCA: user@2026-09-22:24-25
MemoryEpisode replay_exact_journal_original(
    const NativeJournal& journal, const ExactJournalDirectory& addresses,
    std::string_view episode_id, std::int64_t published_row_limit) {
    if (journal.generation() != addresses.journal_generation() ||
        published_row_limit < 0 ||
        static_cast<std::uint64_t>(published_row_limit) > journal.row_count())
        throw std::runtime_error("exact_journal_generation_changed");
    const auto address = addresses.find(episode_id, published_row_limit);
    if (!address) throw std::runtime_error("exact_original_experience_missing");
    return replay_original_from_journal(journal, *address, episode_id);
}

}  // namespace swegca::vrs
