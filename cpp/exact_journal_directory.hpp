#pragma once

#include "original_journal_replay.hpp"
#include "owner_lock.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

namespace swegca::vrs {

// Main owns this derived exact-address directory. Its source of truth is the
// bound native journal generation; values name original frames, not capsules.
// Level, prefix, and odd-step probing follow the existing VRS address route.
// SWEGCA: src/swegca_vrs2/exact_replay.py@c06092a:190-223
class ExactJournalDirectory {
public:
    // A writer must hold the journal owner's lock. Readers pin the same
    // generation and pass their published row limit to avoid future records.
    // SWEGCA: src/swegca_vrs2/exact_replay.py@c06092a:190-223
    ExactJournalDirectory(std::filesystem::path directory,
                          std::string journal_generation,
                          OwnerLock* owner_lock = nullptr);

    // The caller invokes put only for an author HotIndexAppendPlan with
    // added=true, after the original row has been committed to the journal.
    // It must not publish the corresponding Main pair until put returns.
    // SWEGCA: src/swegca_vrs2/exact_replay.py@c06092a:496-609
    void put(std::string_view episode_id, const OriginalJournalAddress& address);

    // Resolve one original ID without scanning transcript, journal, or shards.
    // Old readers cannot see a just-written row beyond their pair's row limit.
    // SWEGCA: src/swegca_vrs2/exact_replay.py@c06092a:691-713
    [[nodiscard]] std::optional<OriginalJournalAddress> find(
        std::string_view episode_id, std::int64_t published_row_limit) const;

    // Rebuild starts only from a new unpublished directory.
    // SWEGCA: src/swegca_vrs2/exact_replay.py@c06092a:190-223
    [[nodiscard]] bool fresh() const;

    // SWEGCA: src/swegca_vrs2/exact_replay.py@c06092a:190-223
    [[nodiscard]] const std::string& journal_generation() const {
        return journal_generation_;
    }

private:
    using Key = std::array<unsigned char, 32>;

    // SWEGCA: src/swegca_vrs2/exact_replay.py@c06092a:72-83
    [[nodiscard]] static Key key_of(std::string_view episode_id);
    // SWEGCA: src/swegca_vrs2/exact_replay.py@c06092a:311-343
    [[nodiscard]] std::filesystem::path segment_path(const Key& key,
                                                       unsigned power) const;

    std::filesystem::path directory_;
    std::string journal_generation_;
    OwnerLock* owner_lock_;
    mutable std::array<std::mutex, 256> prefix_mutex_;
    std::atomic<bool> failed_{false};
};

}  // namespace swegca::vrs
