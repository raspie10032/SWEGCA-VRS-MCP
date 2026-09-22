#pragma once

#include "hot_index.hpp"

#include <memory>
#include <mutex>
#include <string>
#include <string_view>

namespace swegca::vrs {

// An immutable main-owned pair. The VRS generation is identified by its
// digest; its numerical state is retained by Main until pair replacement.
class FullCurrentMemoryVrsSnapshot {
public:
    // SWEGCA: src/swegca_vrs2/engine/mosaic_memory_activation.py@7536139:113-135
    FullCurrentMemoryVrsSnapshot(std::shared_ptr<const PublishedHotIndex> memory,
                                 std::string vrs_snapshot_id);

    // SWEGCA: src/swegca_vrs2/engine/mosaic_memory_activation.py@7536139:117-119
    [[nodiscard]] const PublishedHotIndex& memory() const { return *memory_; }
    // SWEGCA: src/swegca_vrs2/engine/mosaic_memory_activation.py@7536139:117-119
    [[nodiscard]] const std::string& vrs_snapshot_id() const { return vrs_snapshot_id_; }
    // SWEGCA: src/swegca_vrs2/engine/mosaic_memory_activation.py@7536139:119-135
    [[nodiscard]] const std::string& snapshot_id() const { return snapshot_id_; }

private:
    std::shared_ptr<const PublishedHotIndex> memory_;
    std::string vrs_snapshot_id_;
    std::string snapshot_id_;
};

class AtomicFullCurrentMemoryVrsOwner {
public:
    // SWEGCA: src/swegca_vrs2/engine/mosaic_memory_activation.py@7536139:138-143
    explicit AtomicFullCurrentMemoryVrsOwner(
        std::shared_ptr<const FullCurrentMemoryVrsSnapshot> initial);

    // SWEGCA: src/swegca_vrs2/engine/mosaic_memory_activation.py@7536139:145-147
    [[nodiscard]] std::shared_ptr<const FullCurrentMemoryVrsSnapshot> snapshot() const;

    // Main calls replace only after its journal commit succeeds. A reader
    // observes one complete pair before or after this CAS, never half of each.
    // SWEGCA: src/swegca_vrs2/engine/mosaic_memory_activation.py@7536139:149-158
    [[nodiscard]] std::string replace(
        std::string_view expected_snapshot_id,
        std::shared_ptr<const FullCurrentMemoryVrsSnapshot> replacement);

private:
    mutable std::mutex mutex_;
    std::shared_ptr<const FullCurrentMemoryVrsSnapshot> current_;
};

}  // namespace swegca::vrs
