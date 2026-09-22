#include "memory_vrs_pair.hpp"

#include "digest.hpp"
#include "json.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace swegca::vrs {
namespace {

// SWEGCA: src/swegca_vrs2/engine/mosaic_memory_activation.py@7536139:121-135
std::string pair_digest(const PublishedHotIndex& memory, std::string_view vrs_snapshot_id) {
    if (vrs_snapshot_id.size() != 64 ||
        !std::all_of(vrs_snapshot_id.begin(), vrs_snapshot_id.end(), [](char character) {
            return (character >= '0' && character <= '9') ||
                   (character >= 'a' && character <= 'f');
        }))
        throw std::runtime_error("VRS snapshot ID must be a SHA-256 digest");
    Json::Object payload;
    payload.emplace("schema_version", Json(std::string("rozephine-full-current-memory-vrs-snapshot-v1")));
    payload.emplace("memory_snapshot_id", Json(memory.snapshot_id()));
    payload.emplace("vrs_snapshot_id", Json(std::string(vrs_snapshot_id)));
    return sha256_hex(Json(std::move(payload)).canonical());
}

}  // namespace

// SWEGCA: src/swegca_vrs2/engine/mosaic_memory_activation.py@7536139:113-135
FullCurrentMemoryVrsSnapshot::FullCurrentMemoryVrsSnapshot(
    std::shared_ptr<const PublishedHotIndex> memory, std::string vrs_snapshot_id)
    : memory_(std::move(memory)), vrs_snapshot_id_(std::move(vrs_snapshot_id)) {
    if (!memory_) throw std::runtime_error("full-current memory must satisfy the hot-memory contract");
    snapshot_id_ = pair_digest(*memory_, vrs_snapshot_id_);
}

// SWEGCA: src/swegca_vrs2/engine/mosaic_memory_activation.py@7536139:138-143
AtomicFullCurrentMemoryVrsOwner::AtomicFullCurrentMemoryVrsOwner(
    std::shared_ptr<const FullCurrentMemoryVrsSnapshot> initial)
    : current_(std::move(initial)) {
    if (!current_) throw std::runtime_error("full-current memory pair is missing");
}

// SWEGCA: src/swegca_vrs2/engine/mosaic_memory_activation.py@7536139:145-147
std::shared_ptr<const FullCurrentMemoryVrsSnapshot>
AtomicFullCurrentMemoryVrsOwner::snapshot() const {
    std::lock_guard lock(mutex_);
    return current_;
}

// SWEGCA: src/swegca_vrs2/engine/mosaic_memory_activation.py@7536139:149-158
std::string AtomicFullCurrentMemoryVrsOwner::replace(
    std::string_view expected_snapshot_id,
    std::shared_ptr<const FullCurrentMemoryVrsSnapshot> replacement) {
    if (!replacement) throw std::runtime_error("full-current memory pair is missing");
    std::lock_guard lock(mutex_);
    if (current_->snapshot_id() != expected_snapshot_id)
        throw std::runtime_error("full-current snapshot changed before replacement");
    auto result = replacement->snapshot_id();
    current_ = std::move(replacement);
    return result;
}

}  // namespace swegca::vrs
