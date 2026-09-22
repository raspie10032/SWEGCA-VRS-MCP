#include "original_journal_replay.hpp"

#include "digest.hpp"
#include "observation.hpp"

#include <stdexcept>
#include <string>
#include <utility>

namespace swegca::vrs {
namespace {

// SWEGCA: src/swegca_vrs2/store.py@7536139:340-349
// SWEGCA: user@2026-09-22:24-25
MemoryEpisode reconstruct_original(JournalRow stored,
                                   std::string_view expected_episode_id) {
    const auto normalized = observation(Json::parse(stored.body));
    if (normalized.at("request_id").string() != stored.request_id ||
        sha256_hex(normalized.canonical()) != stored.fingerprint)
        throw std::runtime_error("stored_observation_integrity_failed");
    auto episode = episode_from_observation(normalized);
    if (episode.episode_id != expected_episode_id)
        throw std::runtime_error("original_experience_address_mismatch");
    return episode;
}

}  // namespace

// SWEGCA: src/swegca_vrs2/store.py@7536139:340-349
// SWEGCA: user@2026-09-22:24-25
MemoryEpisode replay_original_from_journal(
    const NativeJournal& journal, const OriginalJournalAddress& address,
    std::string_view expected_episode_id) {
    return reconstruct_original(journal.row_at(address.frame, address.sequence),
                                expected_episode_id);
}

// SWEGCA: src/swegca_vrs2/store.py@7536139:340-349
// SWEGCA: user@2026-09-22:24-25
MemoryEpisode replay_original_from_journal(
    const NativeJournalReadView& journal, const OriginalJournalAddress& address,
    std::string_view expected_episode_id) {
    return reconstruct_original(journal.row_at(address.frame, address.sequence),
                                expected_episode_id);
}

}  // namespace swegca::vrs
