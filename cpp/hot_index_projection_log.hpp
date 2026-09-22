#pragma once

#include "hot_index_projection.hpp"
#include "owner_lock.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>

namespace swegca::vrs {

// Physical location of one rebuildable metadata frame. The native journal
// remains the sole original experience; this address never names a body.
// SWEGCA: src/swegca_vrs2/exact_replay.py@c06092a:190-223
struct HotProjectionAddress {
    std::string journal_generation;
    std::uint8_t prefix;
    std::uint64_t byte_offset;
    std::uint32_t byte_length;
    std::int64_t journal_sequence;
};

// A 256-way physical metadata log. Independent prefixes can be written by
// separate background workers. Publication and ID->offset lookup belong to
// the derived HotIndex directory, not to this append-only log.
// SWEGCA: src/swegca_vrs2/exact_replay.py@c06092a:190-223
class HotIndexProjectionLog {
public:
    HotIndexProjectionLog(std::filesystem::path directory,
                          std::string journal_generation,
                          OwnerLock* owner_lock = nullptr);

    // SWEGCA: src/swegca_vrs2/exact_replay.py@c06092a:496-609
    [[nodiscard]] HotProjectionAddress append(const HotIndexProjectionRow& row);

    // The caller supplies the published Main row limit from its pinned pair.
    // SWEGCA: src/swegca_vrs2/exact_replay.py@c06092a:691-713
    [[nodiscard]] HotIndexProjectionRow read_at(
        const HotProjectionAddress& address, std::string_view expected_episode_id,
        std::int64_t published_row_limit) const;

    // SWEGCA: src/swegca_vrs2/exact_replay.py@c06092a:190-223
    [[nodiscard]] const std::string& journal_generation() const {
        return journal_generation_;
    }

private:
    [[nodiscard]] std::filesystem::path file_for(std::uint8_t prefix) const;

    std::filesystem::path directory_;
    std::string journal_generation_;
    OwnerLock* owner_lock_;
    mutable std::array<std::mutex, 256> prefix_mutex_;
    std::atomic<bool> failed_{false};
};

}  // namespace swegca::vrs
