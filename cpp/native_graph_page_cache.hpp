#pragma once

#include "native_graph_page_file.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <utility>
#include <variant>

namespace swegca::vrs {

// Fixed-allocation, 16-shard cache for immutable numerical pages. It changes
// no SWEGCA address or value rule: misses use the same checksummed page reader,
// and a slot is identified by the retained file object, physical offset and
// logical page ID. Direct mapping keeps resident allocation independent of
// Main size and avoids an unbounded metadata map.
// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_kernel.py@7536139:34-76
class NativeGraphPageCache {
public:
    // byte_budget covers this object and all fixed page slots as measured by
    // sizeof. The outer Main budget must retain allocator/RSS headroom.
    // SWEGCA: user@2026-09-22:89-92
    explicit NativeGraphPageCache(std::uint64_t byte_budget);
    NativeGraphPageCache(const NativeGraphPageCache&) = delete;
    NativeGraphPageCache& operator=(const NativeGraphPageCache&) = delete;

    // SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_kernel.py@7536139:34-76
    [[nodiscard]] GraphNumericNodeRecord node_record(
        std::shared_ptr<const NativeGraphPageFile> file,
        std::uint64_t offset, std::uint32_t page_id,
        std::uint32_t record);
    // SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_kernel.py@7536139:34-76
    [[nodiscard]] GraphNumericEdgeRecord edge_record(
        std::shared_ptr<const NativeGraphPageFile> file,
        std::uint64_t offset, std::uint32_t page_id,
        std::uint32_t record);

    // SWEGCA: user@2026-09-22:89-92
    [[nodiscard]] std::uint64_t resident_bytes() const {
        return resident_bytes_;
    }
    // SWEGCA: user@2026-09-22:89-92
    [[nodiscard]] std::uint64_t slot_count() const {
        return slot_count_;
    }

private:
    enum class Kind : std::uint8_t { empty, node, edge };

    struct Slot {
        Kind kind = Kind::empty;
        std::uint32_t page_id = 0;
        std::uint64_t offset = 0;
        std::shared_ptr<const NativeGraphPageFile> file;
        std::variant<std::monostate, NativeGraphNodePage,
                     NativeGraphEdgePage> page;
    };

    struct Shard {
        std::mutex mutex;
        std::unique_ptr<Slot[]> slots;
        std::size_t count = 0;
    };

    static constexpr std::size_t shard_limit = 16;

    [[nodiscard]] std::pair<Shard*, std::size_t> locate(
        Kind kind, const NativeGraphPageFile* file,
        std::uint64_t offset, std::uint32_t page_id);

    std::array<Shard, shard_limit> shards_;
    std::size_t active_shards_ = 0;
    std::uint64_t resident_bytes_ = 0;
    std::uint64_t slot_count_ = 0;
};

}  // namespace swegca::vrs
