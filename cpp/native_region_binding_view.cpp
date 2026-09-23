#include "native_region_binding_view.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace swegca::vrs {
namespace {

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_kernel.py@7536139:44-45
bool digest_id(std::string_view value) {
    return value.size() == 64 &&
        std::all_of(value.begin(), value.end(), [](char digit) {
            return (digit >= '0' && digit <= '9') ||
                   (digit >= 'a' && digit <= 'f');
        });
}

// Physical routing only; this does not rank or omit a source node.
// SWEGCA: src/swegca_vrs2/store.py@c06092a:297-310
std::uint64_t mix_key(const NativeRegionBindingPageFile* file,
                      std::uint64_t offset, std::uint32_t page_id) {
    auto value = static_cast<std::uint64_t>(
        reinterpret_cast<std::uintptr_t>(file));
    value ^= offset + 0x9e3779b97f4a7c15ULL + (value << 6) + (value >> 2);
    value ^= std::uint64_t(page_id) * 0xbf58476d1ce4e5b9ULL;
    value ^= value >> 30;
    value *= 0xbf58476d1ce4e5b9ULL;
    value ^= value >> 27;
    value *= 0x94d049bb133111ebULL;
    return value ^ (value >> 31);
}

// SWEGCA: src/swegca_vrs2/store.py@c06092a:487-510
std::uint64_t required_pages(std::uint64_t node_count) {
    return node_count == 0 ? 0 :
        (node_count - 1) / NativeGraphPageMap::records_per_page + 1;
}

}  // namespace

// SWEGCA: user@2026-09-22:89-92
NativeRegionBindingPageCache::NativeRegionBindingPageCache(
    std::uint64_t byte_budget) {
    if (byte_budget < sizeof(NativeRegionBindingPageCache))
        throw std::runtime_error("region_binding_cache_budget_too_small");
    const auto possible =
        (byte_budget - sizeof(NativeRegionBindingPageCache)) / sizeof(Slot);
    if (possible > std::numeric_limits<std::size_t>::max())
        throw std::runtime_error("region_binding_cache_budget_too_large");
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
    resident_bytes_ = sizeof(NativeRegionBindingPageCache) +
        slot_count_ * sizeof(Slot);
    if (resident_bytes_ > byte_budget)
        throw std::runtime_error("region_binding_cache_budget_changed");
}

// SWEGCA: src/swegca_vrs2/store.py@c06092a:297-310
std::pair<NativeRegionBindingPageCache::Shard*, std::size_t>
NativeRegionBindingPageCache::locate(
    const NativeRegionBindingPageFile* file,
    std::uint64_t offset, std::uint32_t page_id) {
    if (active_shards_ == 0) return {nullptr, 0};
    const auto key = mix_key(file, offset, page_id);
    auto& shard = shards_[key % active_shards_];
    return {&shard, static_cast<std::size_t>(
        (key / active_shards_) % shard.count)};
}

// SWEGCA: src/swegca_vrs2/store.py@c06092a:297-310
RegionBindingRecord NativeRegionBindingPageCache::record(
    std::shared_ptr<const NativeRegionBindingPageFile> file,
    std::uint64_t offset, std::uint32_t page_id,
    std::uint32_t record) {
    if (!file || record >= NativeGraphPageMap::records_per_page)
        throw std::runtime_error("region_binding_cache_request_invalid");
    auto [shard, slot_index] = locate(file.get(), offset, page_id);
    if (shard) {
        std::lock_guard guard(shard->mutex);
        const auto& slot = shard->slots[slot_index];
        if (slot.occupied && slot.file.get() == file.get() &&
            slot.offset == offset && slot.page_id == page_id) {
            if (record >= slot.page.valid_records)
                throw std::runtime_error("region_binding_page_changed");
            return slot.page.records[record];
        }
    }
    auto loaded = file->read(offset, page_id);
    if (record >= loaded.valid_records)
        throw std::runtime_error("region_binding_page_changed");
    const auto result = loaded.records[record];
    if (shard) {
        std::lock_guard guard(shard->mutex);
        auto& slot = shard->slots[slot_index];
        slot.occupied = false;
        slot.file.reset();
        slot.page = std::move(loaded);
        slot.page_id = page_id;
        slot.offset = offset;
        slot.file = std::move(file);
        slot.occupied = true;
    }
    return result;
}

// SWEGCA: src/swegca_vrs2/store.py@c06092a:512-577
NativeRegionBindingView::NativeRegionBindingView(
    NativeRegionBindingState state,
    std::shared_ptr<const NativeRegionBindingPageFile> file,
    std::shared_ptr<NativeRegionBindingPageCache> cache)
    : state_(std::move(state)), file_(std::move(file)),
      cache_(std::move(cache)) {
    if (!file_ || !cache_ || !digest_id(state_.graph_snapshot_id) ||
        state_.journal_generation != file_->journal_generation() ||
        state_.node_count > std::uint64_t{0x100000000ULL} ||
        state_.pages.page_count() != required_pages(state_.node_count))
        throw std::runtime_error("region_binding_state_invalid");
    validate_complete_state();
}

// SWEGCA: src/swegca_vrs2/store.py@c06092a:512-577
void NativeRegionBindingView::require_source(
    const EventVrsInputView& source) const {
    if (source.snapshot_id() != state_.graph_snapshot_id ||
        source.node_count() != state_.node_count)
        throw std::runtime_error("region_binding_source_changed");
}

// Cold-open proof: every logical page and live record belongs to the exact
// node range. This retains only one 4 KiB payload at a time.
// SWEGCA: src/swegca_vrs2/store.py@c06092a:512-577
void NativeRegionBindingView::validate_complete_state() const {
    const auto pages = required_pages(state_.node_count);
    std::uint64_t visited_pages = 0;
    state_.pages.visit([&](std::uint32_t page_id, std::uint64_t offset) {
        if (page_id != visited_pages || page_id >= pages)
            throw std::runtime_error("region_binding_page_map_invalid");
        const auto page = file_->read(offset, page_id);
        const auto first = std::uint64_t{page_id} *
                           NativeGraphPageMap::records_per_page;
        const auto expected = static_cast<std::uint32_t>(
            std::min<std::uint64_t>(NativeGraphPageMap::records_per_page,
                                    state_.node_count - first));
        if (page.valid_records != expected)
            throw std::runtime_error("region_binding_page_shape_invalid");
        for (std::uint32_t at = 0; at < page.valid_records; ++at) {
            const auto global = first + at;
            const auto& value = page.records[at];
            if (!value.present) continue;
            if (value.component > global || value.component >= state_.node_count ||
                ((value.local == 0) != (value.component == global)))
                throw std::runtime_error("region_binding_record_source_invalid");
        }
        ++visited_pages;
    });
    if (visited_pages != pages)
        throw std::runtime_error("region_binding_page_map_invalid");
}

// SWEGCA: src/swegca_vrs2/store.py@c06092a:602-608
std::optional<RegionBindingRecord> NativeRegionBindingView::binding(
    std::uint32_t node) const {
    if (node >= state_.node_count)
        throw std::out_of_range("region node outside directory");
    const auto page_id = node / NativeGraphPageMap::records_per_page;
    const auto record = node % NativeGraphPageMap::records_per_page;
    const auto offset = state_.pages.offset(page_id);
    if (!offset) throw std::runtime_error("region_binding_page_missing");
    const auto value = cache_->record(file_, *offset, page_id, record);
    if (!value.present) return std::nullopt;
    if (value.component > node || value.component >= state_.node_count ||
        ((value.local == 0) != (value.component == node)))
        throw std::runtime_error("region_binding_record_source_invalid");
    return value;
}

}  // namespace swegca::vrs
