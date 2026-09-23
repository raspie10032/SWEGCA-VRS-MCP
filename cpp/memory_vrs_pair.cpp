#include "memory_vrs_pair.hpp"

#include "digest.hpp"
#include "json.hpp"
#include "main_read_generation.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace swegca::vrs {

// SWEGCA: src/swegca_vrs2/engine/mosaic_memory_activation.py@7536139:121-135
std::string full_current_pair_snapshot_id(
    std::string_view memory_snapshot_id, std::string_view vrs_snapshot_id) {
    if (vrs_snapshot_id.size() != 64 ||
        !std::all_of(vrs_snapshot_id.begin(), vrs_snapshot_id.end(), [](char character) {
            return (character >= '0' && character <= '9') ||
                   (character >= 'a' && character <= 'f');
        }))
        throw std::runtime_error("VRS snapshot ID must be a SHA-256 digest");
    Json::Object payload;
    payload.emplace("schema_version", Json(std::string("rozephine-full-current-memory-vrs-snapshot-v1")));
    payload.emplace("memory_snapshot_id", Json(std::string(memory_snapshot_id)));
    payload.emplace("vrs_snapshot_id", Json(std::string(vrs_snapshot_id)));
    return sha256_hex(Json(std::move(payload)).canonical());
}

// SWEGCA: src/swegca_vrs2/engine/mosaic_memory_activation.py@7536139:113-135
FullCurrentMemoryVrsSnapshot::FullCurrentMemoryVrsSnapshot(
    std::shared_ptr<const PublishedHotIndex> memory, std::string vrs_snapshot_id)
    : memory_(std::move(memory)), vrs_snapshot_id_(std::move(vrs_snapshot_id)) {
    if (!memory_) throw std::runtime_error("full-current memory must satisfy the hot-memory contract");
    snapshot_id_ = full_current_pair_snapshot_id(memory_->snapshot_id(),
                                                  vrs_snapshot_id_);
}

// SWEGCA: src/swegca_vrs2/engine/mosaic_memory_activation.py@7536139:138-143
AtomicFullCurrentMemoryVrsOwner::AtomicFullCurrentMemoryVrsOwner(
    std::shared_ptr<const MainReadGeneration> initial)
    : current_(std::move(initial)) {
    if (!current_) throw std::runtime_error("full-current memory pair is missing");
}

// SWEGCA: src/swegca_vrs2/engine/mosaic_memory_activation.py@7536139:145-147
std::shared_ptr<const MainReadGeneration>
AtomicFullCurrentMemoryVrsOwner::snapshot() const {
    std::lock_guard lock(mutex_);
    return current_;
}

// SWEGCA: src/swegca_vrs2/engine/mosaic_memory_activation.py@7536139:149-158
// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_region_publication.py@0dc716a:37-53
std::string AtomicFullCurrentMemoryVrsOwner::replace(
    std::shared_ptr<const MainReadGeneration> expected,
    std::shared_ptr<const MainReadGeneration> replacement) {
    if (!expected || !replacement)
        throw std::runtime_error("full-current memory generation is missing");
    std::lock_guard lock(mutex_);
    if (current_ != expected)
        throw std::runtime_error("full-current generation changed before replacement");
    auto result = replacement->pair().snapshot_id();
    current_ = std::move(replacement);
    return result;
}

}  // namespace swegca::vrs
