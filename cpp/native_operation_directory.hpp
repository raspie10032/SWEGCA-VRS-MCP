#pragma once

#include "main_operations.hpp"
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

struct OperationDirectoryPublication {
    std::string journal_generation;
    std::int64_t published_rows;
    std::string pair_snapshot_id;
};

// A derived request-ID directory. The journal alone owns operation history;
// this tiered exact-key index makes its idempotency certificates addressable
// without retaining every request in the 4 GB resident budget.
// SWEGCA: src/swegca_vrs2/store.py@7536139:378-399
// SWEGCA: src/swegca_vrs2/exact_replay.py@c06092a:311-390
class NativeOperationDirectory final : public MainOperationRead {
public:
    // SWEGCA: src/swegca_vrs2/exact_replay.py@c06092a:190-223
    NativeOperationDirectory(std::filesystem::path directory,
                             std::string journal_generation,
                             std::int64_t published_row_limit,
                             OwnerLock* owner_lock = nullptr);

    // The caller has already committed this exact row in the native journal.
    // A duplicate request may only retain its first identical certificate.
    // SWEGCA: src/swegca_vrs2/store.py@7536139:378-399
    void put(std::string_view request_id, const MainOperation& operation,
             std::int64_t journal_sequence);

    // SWEGCA: src/swegca_vrs2/store.py@7536139:378-383
    [[nodiscard]] std::optional<MainOperation> find_operation(
        std::string_view request_id) const override;

    // SWEGCA: src/swegca_vrs2/exact_replay.py@c06092a:583-609
    void publish(std::int64_t journal_rows, std::string_view pair_snapshot_id);
    [[nodiscard]] std::optional<OperationDirectoryPublication>
    publication() const;
    [[nodiscard]] bool fresh() const;
    // SWEGCA: src/swegca_vrs2/exact_replay.py@c06092a:190-223
    [[nodiscard]] bool published_reader() const { return owner_lock_ == nullptr; }
    // SWEGCA: src/swegca_vrs2/exact_replay.py@c06092a:190-223
    [[nodiscard]] const std::string& journal_generation() const {
        return journal_generation_;
    }
    // SWEGCA: src/swegca_vrs2/exact_replay.py@c06092a:190-223
    [[nodiscard]] const std::filesystem::path& directory() const {
        return directory_;
    }

private:
    using Key = std::array<unsigned char, 32>;
    [[nodiscard]] static Key key_of(std::string_view request_id);
    [[nodiscard]] std::filesystem::path table_path(const Key& key,
                                                   unsigned power) const;
    [[nodiscard]] std::filesystem::path text_path(const Key& key) const;

    std::filesystem::path directory_;
    std::string journal_generation_;
    std::int64_t published_row_limit_;
    OwnerLock* owner_lock_;
    mutable std::array<std::mutex, 256> prefix_mutex_;
    std::atomic<bool> failed_{false};
    mutable std::mutex publication_mutex_;
    std::optional<OperationDirectoryPublication> publication_;
};

}  // namespace swegca::vrs
