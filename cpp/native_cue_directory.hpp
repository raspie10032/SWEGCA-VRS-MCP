#pragma once

#include "owner_lock.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace swegca::vrs {

struct CueDirectoryPublication {
    std::string journal_generation;
    std::int64_t published_rows;
    std::string pair_snapshot_id;
};

// Main's derived cue -> complete original-ID posting directory. One cue's
// exact UTF-8 bytes are checked after hash lookup; posting nodes retain the
// first original journal sequence, so an older pair cannot see later rows.
// SWEGCA: src/swegca_vrs2/cue_shards.py@c06092a:1-22
// SWEGCA: src/swegca_vrs2/engine/mosaic_memory_activation.py@7536139:246-348
class NativeCueDirectory {
public:
    NativeCueDirectory(std::filesystem::path directory,
                       std::string journal_generation,
                       OwnerLock* owner_lock = nullptr);

    // Cues come from the author's raw HotIndex posting tuple. Repeated cue
    // values for one original are inserted once, as set-union append does.
    // SWEGCA: src/swegca_vrs2/store.py@7536139:154-167
    // SWEGCA: src/swegca_vrs2/cue_shards.py@c06092a:260-360
    void put(std::span<const std::string> cues, std::string_view episode_id,
             std::int64_t journal_sequence);

    // SWEGCA: src/swegca_vrs2/engine/mosaic_memory_activation.py@7536139:246-267
    [[nodiscard]] std::uint64_t posting_count(
        std::string_view cue, std::int64_t published_row_limit) const;

    // SWEGCA: src/swegca_vrs2/engine/mosaic_memory_activation.py@7536139:301-348
    [[nodiscard]] std::vector<std::string> episode_ids_for_cue(
        std::string_view cue, std::int64_t published_row_limit) const;

    // A descending k-way merge of source sequences counts the exact distinct
    // union while retaining only one posting cursor per cue in RAM.
    // SWEGCA: src/swegca_vrs2/engine/mosaic_memory_activation.py@7536139:246-267
    [[nodiscard]] std::uint64_t exact_union_count(
        std::span<const std::string> cues,
        std::int64_t published_row_limit) const;

    // SWEGCA: src/swegca_vrs2/cue_shards.py@c06092a:260-360
    void publish(std::int64_t journal_rows, std::string_view pair_snapshot_id);

    // SWEGCA: src/swegca_vrs2/cue_shards.py@c06092a:1-22
    [[nodiscard]] std::optional<CueDirectoryPublication> publication() const;

    // Rebuild accepts only a new unpublished derived directory.
    // SWEGCA: src/swegca_vrs2/cue_shards.py@c06092a:59-91
    [[nodiscard]] bool fresh() const;

    // SWEGCA: src/swegca_vrs2/cue_shards.py@c06092a:1-22
    [[nodiscard]] bool published_reader() const { return owner_lock_ == nullptr; }

    // SWEGCA: src/swegca_vrs2/cue_shards.py@c06092a:1-22
    [[nodiscard]] const std::string& journal_generation() const {
        return journal_generation_;
    }

    // SWEGCA: src/swegca_vrs2/cue_shards.py@c06092a:59-91
    [[nodiscard]] const std::filesystem::path& directory() const {
        return directory_;
    }

private:
    using Key = std::array<unsigned char, 32>;
    struct PostingNode {
        std::uint64_t previous;
        std::int64_t sequence;
        std::uint64_t cumulative_count;
        std::string episode_id;
    };
    struct CueHead {
        std::uint8_t prefix;
        std::uint64_t head;
        std::uint64_t count;
        std::int64_t sequence;
    };

    [[nodiscard]] static Key key_of(std::string_view cue);
    [[nodiscard]] std::filesystem::path table_path(unsigned power,
                                                    std::uint8_t prefix) const;
    [[nodiscard]] std::filesystem::path cue_path(std::uint8_t prefix) const;
    [[nodiscard]] std::filesystem::path posting_path(std::uint8_t prefix) const;
    [[nodiscard]] std::optional<CueHead> head_for(
        std::string_view cue, std::int64_t published_row_limit) const;
    [[nodiscard]] PostingNode node_at(std::uint8_t prefix,
                                      std::uint64_t offset) const;

    std::filesystem::path directory_;
    std::string journal_generation_;
    OwnerLock* owner_lock_;
    mutable std::array<std::mutex, 256> prefix_mutex_;
    std::atomic<bool> failed_{false};
    mutable std::mutex publication_mutex_;
    std::optional<CueDirectoryPublication> publication_;
};

}  // namespace swegca::vrs
