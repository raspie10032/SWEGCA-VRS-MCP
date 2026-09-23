#pragma once

#include "hot_index.hpp"

#include <memory>
#include <mutex>
#include <string>
#include <string_view>

namespace swegca::vrs {

class MainReadGeneration;

// Compute the author's pair certificate from two already prepared immutable
// generation IDs. Main uses this before the journal row is committed.
// SWEGCA: src/swegca_vrs2/engine/mosaic_memory_activation.py@7536139:121-135
[[nodiscard]] std::string full_current_pair_snapshot_id(
    std::string_view memory_snapshot_id, std::string_view vrs_snapshot_id);

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
        std::shared_ptr<const MainReadGeneration> initial);

    // SWEGCA: src/swegca_vrs2/engine/mosaic_memory_activation.py@7536139:145-147
    [[nodiscard]] std::shared_ptr<const MainReadGeneration> snapshot() const;

    // Main calls replace only after its journal commit succeeds. The expected
    // value is the exact pinned generation, so same-pair navigation work
    // cannot overwrite a newer publication. A reader observes the pair and
    // every bound Graph directory in one generation.
    // SWEGCA: src/swegca_vrs2/engine/mosaic_memory_activation.py@7536139:149-158
    // SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_region_publication.py@0dc716a:37-53
    [[nodiscard]] std::string replace(
        std::shared_ptr<const MainReadGeneration> expected,
        std::shared_ptr<const MainReadGeneration> replacement);

private:
    mutable std::mutex mutex_;
    std::shared_ptr<const MainReadGeneration> current_;
};

}  // namespace swegca::vrs
