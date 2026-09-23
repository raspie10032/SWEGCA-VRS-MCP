#pragma once

#include "main_observation_batch.hpp"
#include "native_journal.hpp"
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
#include <utility>

namespace swegca::vrs {

struct GraphNodePublication {
    std::string journal_generation;
    std::string graph_snapshot_id;
    std::string pair_snapshot_id;
    std::int64_t published_rows;
    std::uint64_t node_count;
};

// The author's record/cue node order, represented by append-only name and
// address records plus a tiered exact name index. No name map grows with the
// complete Graph in process memory. An old reader filters later addresses by
// its pinned node count, even when the owner appends to the same files.
// SWEGCA: src/swegca_vrs2/store.py@c06092a:202-265
// SWEGCA: src/swegca_vrs2/exact_replay.py@c06092a:311-390
class NativeGraphNodeDirectory final : public GraphNodeDirectory {
public:
    // Readers require a matching published certificate. A writer holds the
    // native Main owner lock and may prepare later nodes before publication.
    // SWEGCA: src/swegca_vrs2/store.py@c06092a:202-265
    NativeGraphNodeDirectory(std::filesystem::path directory,
                             std::string journal_generation,
                             std::string graph_snapshot_id,
                             std::uint64_t node_count,
                             std::int64_t published_rows,
                             OwnerLock* owner_lock = nullptr);

    // Main may apply node names only for committed source Graph row
    // transitions. Repeating the same frame after a partial write is accepted only when
    // every existing address and exact name agree.
    // SWEGCA: src/swegca_vrs2/store.py@7536139:384-399
    void append_committed(const NativeJournal& journal,
                          const JournalAppendResult& committed,
                          const MainObservationBatchPlan& batch,
                          const ValidatedEventVrsInputs& parent,
                          std::string_view published_parent_pair);

    // Commit only after the original journal and all Graph numeric state are
    // durable. Main still replaces its complete read generation separately.
    // SWEGCA: src/swegca_vrs2/store.py@c06092a:1429-1452
    void publish(const NativeJournal& journal,
                 const ValidatedEventVrsInputs& successor,
                 std::string_view memory_snapshot_id,
                 std::string_view pair_snapshot_id,
                 std::int64_t journal_rows);

    // SWEGCA: src/swegca_vrs2/store.py@c06092a:202-265
    void require_source(const EventVrsInputView& source) const override;
    // SWEGCA: src/swegca_vrs2/store.py@c06092a:216-244
    [[nodiscard]] std::uint64_t node_count() const override { return node_count_; }
    // SWEGCA: src/swegca_vrs2/store.py@c06092a:224-240
    [[nodiscard]] bool contains(std::string_view name) const override;
    // SWEGCA: src/swegca_vrs2/store.py@c06092a:232-240
    [[nodiscard]] std::uint32_t address(std::string_view name) const override;
    // SWEGCA: src/swegca_vrs2/store.py@c06092a:242-244
    [[nodiscard]] std::string name(std::uint32_t address) const override;

    // SWEGCA: src/swegca_vrs2/store.py@c06092a:1429-1452
    [[nodiscard]] std::optional<GraphNodePublication> publication() const;
    // SWEGCA: src/swegca_vrs2/store.py@c06092a:1429-1452
    [[nodiscard]] bool published_reader() const { return owner_lock_ == nullptr; }
    // SWEGCA: src/swegca_vrs2/store.py@c06092a:202-265
    [[nodiscard]] const std::string& journal_generation() const {
        return journal_generation_;
    }
    // SWEGCA: src/swegca_vrs2/store.py@c06092a:202-265
    [[nodiscard]] const std::filesystem::path& directory() const {
        return directory_;
    }

private:
    using Key = std::array<unsigned char, 32>;
    void append(std::span<const std::pair<std::string, std::uint32_t>> nodes);
    [[nodiscard]] std::filesystem::path table_path(
        unsigned power, unsigned char prefix) const;
    [[nodiscard]] std::optional<std::pair<std::uint32_t, std::uint64_t>>
    find_name(std::string_view name, std::uint64_t limit) const;
    [[nodiscard]] std::filesystem::path insert_name(
        std::string_view name, std::uint32_t address,
        std::uint64_t name_offset);
    [[nodiscard]] std::uint64_t reverse_count() const;
    [[nodiscard]] std::uint64_t reverse_offset(std::uint32_t address) const;
    [[nodiscard]] std::string read_name(std::uint64_t offset) const;

    std::filesystem::path directory_;
    std::string journal_generation_;
    std::string graph_snapshot_id_;
    std::uint64_t node_count_;
    std::int64_t published_rows_;
    OwnerLock* owner_lock_;
    mutable std::mutex append_mutex_;
    mutable std::array<std::mutex, 256> prefix_mutex_;
    mutable std::mutex publication_mutex_;
    std::optional<GraphNodePublication> publication_;
    std::atomic<bool> failed_{false};
};

}  // namespace swegca::vrs
