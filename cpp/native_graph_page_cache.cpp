#include "native_graph_page_cache.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace swegca::vrs {
namespace {

// Physical cache routing only; it never changes the source's address order.
// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_kernel.py@7536139:34-76
std::uint64_t mix_key(std::uint8_t kind,
                      const NativeGraphPageFile* file,
                      std::uint64_t offset,
                      std::uint32_t page_id) {
    auto value = static_cast<std::uint64_t>(
        reinterpret_cast<std::uintptr_t>(file));
    value ^= offset + 0x9e3779b97f4a7c15ULL + (value << 6) + (value >> 2);
    value ^= std::uint64_t(page_id) * 0xbf58476d1ce4e5b9ULL;
    value ^= std::uint64_t(kind) * 0x94d049bb133111ebULL;
    value ^= value >> 30;
    value *= 0xbf58476d1ce4e5b9ULL;
    value ^= value >> 27;
    value *= 0x94d049bb133111ebULL;
    return value ^ (value >> 31);
}

}  // namespace

// SWEGCA: user@2026-09-22:89-92
NativeGraphPageCache::NativeGraphPageCache(std::uint64_t byte_budget) {
    if (byte_budget < sizeof(NativeGraphPageCache))
        throw std::runtime_error("graph_page_cache_budget_too_small");
    const auto possible =
        (byte_budget - sizeof(NativeGraphPageCache)) / sizeof(Slot);
    if (possible > std::numeric_limits<std::size_t>::max())
        throw std::runtime_error("graph_page_cache_budget_too_large");
    slot_count_ = possible;
    active_shards_ = static_cast<std::size_t>(
        std::min<std::uint64_t>(slot_count_, shard_limit));
    if (active_shards_ != 0) {
        const auto base = static_cast<std::size_t>(
            slot_count_ / active_shards_);
        const auto extra = static_cast<std::size_t>(
            slot_count_ % active_shards_);
        for (std::size_t at = 0; at < active_shards_; ++at) {
            shards_[at].count = base + (at < extra ? 1 : 0);
            shards_[at].slots =
                std::make_unique<Slot[]>(shards_[at].count);
        }
    }
    resident_bytes_ = sizeof(NativeGraphPageCache) +
        slot_count_ * sizeof(Slot);
    if (resident_bytes_ > byte_budget)
        throw std::runtime_error("graph_page_cache_budget_changed");
}

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_kernel.py@7536139:34-76
std::pair<NativeGraphPageCache::Shard*, std::size_t>
NativeGraphPageCache::locate(
    Kind kind, const NativeGraphPageFile* file,
    std::uint64_t offset, std::uint32_t page_id) {
    if (active_shards_ == 0) return {nullptr, 0};
    const auto key = mix_key(static_cast<std::uint8_t>(kind), file,
                             offset, page_id);
    auto& shard = shards_[key % active_shards_];
    return {&shard, static_cast<std::size_t>(
        (key / active_shards_) % shard.count)};
}

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_kernel.py@7536139:34-76
GraphNumericNodeRecord NativeGraphPageCache::node_record(
    std::shared_ptr<const NativeGraphPageFile> file,
    std::uint64_t offset, std::uint32_t page_id,
    std::uint32_t record) {
    if (!file || record >= NativeGraphPageMap::records_per_page)
        throw std::runtime_error("graph_page_cache_node_request_invalid");
    auto [shard, slot_index] = locate(
        Kind::node, file.get(), offset, page_id);
    if (shard) {
        std::lock_guard guard(shard->mutex);
        const auto& slot = shard->slots[slot_index];
        if (slot.kind == Kind::node && slot.file.get() == file.get() &&
            slot.offset == offset && slot.page_id == page_id) {
            const auto& page = std::get<NativeGraphNodePage>(slot.page);
            if (record >= page.valid_records)
                throw std::runtime_error("graph_numeric_node_page_changed");
            return page.records[record];
        }
    }
    auto loaded = file->read_node(offset, page_id);
    if (record >= loaded.valid_records)
        throw std::runtime_error("graph_numeric_node_page_changed");
    const auto result = loaded.records[record];
    if (shard) {
        std::lock_guard guard(shard->mutex);
        auto& slot = shard->slots[slot_index];
        slot.kind = Kind::empty;
        slot.file.reset();
        slot.page = std::move(loaded);
        slot.page_id = page_id;
        slot.offset = offset;
        slot.file = std::move(file);
        slot.kind = Kind::node;
    }
    return result;
}

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_kernel.py@7536139:34-76
GraphNumericEdgeRecord NativeGraphPageCache::edge_record(
    std::shared_ptr<const NativeGraphPageFile> file,
    std::uint64_t offset, std::uint32_t page_id,
    std::uint32_t record) {
    if (!file || record >= NativeGraphPageMap::records_per_page)
        throw std::runtime_error("graph_page_cache_edge_request_invalid");
    auto [shard, slot_index] = locate(
        Kind::edge, file.get(), offset, page_id);
    if (shard) {
        std::lock_guard guard(shard->mutex);
        const auto& slot = shard->slots[slot_index];
        if (slot.kind == Kind::edge && slot.file.get() == file.get() &&
            slot.offset == offset && slot.page_id == page_id) {
            const auto& page = std::get<NativeGraphEdgePage>(slot.page);
            if (record >= page.valid_records)
                throw std::runtime_error("graph_numeric_edge_page_changed");
            return page.records[record];
        }
    }
    auto loaded = file->read_edge(offset, page_id);
    if (record >= loaded.valid_records)
        throw std::runtime_error("graph_numeric_edge_page_changed");
    const auto result = loaded.records[record];
    if (shard) {
        std::lock_guard guard(shard->mutex);
        auto& slot = shard->slots[slot_index];
        slot.kind = Kind::empty;
        slot.file.reset();
        slot.page = std::move(loaded);
        slot.page_id = page_id;
        slot.offset = offset;
        slot.file = std::move(file);
        slot.kind = Kind::edge;
    }
    return result;
}

}  // namespace swegca::vrs
