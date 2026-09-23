#include "native_graph_region_directory.hpp"

#include "memory_vrs_pair.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace swegca::vrs {
namespace {

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_kernel.py@7536139:44-45
bool sha256_id(std::string_view identifier) {
    return identifier.size() == 64 &&
        std::all_of(identifier.begin(), identifier.end(), [](char digit) {
            return (digit >= '0' && digit <= '9') ||
                   (digit >= 'a' && digit <= 'f');
        });
}

// Physical cache routing only; no SWEGCA score or address enters the key.
// SWEGCA: src/swegca_vrs2/store.py@c06092a:599-608
std::uint64_t mix_key(const NativeRegionTopologyCatalog* catalog,
                      std::uint64_t offset, std::uint32_t component) {
    auto value = static_cast<std::uint64_t>(
        reinterpret_cast<std::uintptr_t>(catalog));
    value ^= offset + 0x9e3779b97f4a7c15ULL + (value << 6) + (value >> 2);
    value ^= std::uint64_t(component) * 0xbf58476d1ce4e5b9ULL;
    value ^= value >> 30;
    value *= 0xbf58476d1ce4e5b9ULL;
    value ^= value >> 27;
    value *= 0x94d049bb133111ebULL;
    return value ^ (value >> 31);
}

}  // namespace

// SWEGCA: user@2026-09-22:89-92
NativeRegionTopologyCache::NativeRegionTopologyCache(
    std::uint64_t byte_budget) {
    if (byte_budget < sizeof(NativeRegionTopologyCache))
        throw std::runtime_error("region_topology_cache_budget_too_small");
    const auto possible =
        (byte_budget - sizeof(NativeRegionTopologyCache)) / sizeof(Slot);
    if (possible > std::numeric_limits<std::size_t>::max())
        throw std::runtime_error("region_topology_cache_budget_too_large");
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
    allocated_slot_bytes_ = sizeof(NativeRegionTopologyCache) +
        slot_count_ * sizeof(Slot);
    if (allocated_slot_bytes_ > byte_budget)
        throw std::runtime_error("region_topology_cache_budget_changed");
}

// SWEGCA: src/swegca_vrs2/store.py@c06092a:599-608
std::pair<NativeRegionTopologyCache::Shard*, std::size_t>
NativeRegionTopologyCache::locate(
    const NativeRegionTopologyCatalog* catalog,
    std::uint64_t offset, std::uint32_t component) {
    if (active_shards_ == 0) return {nullptr, 0};
    const auto key = mix_key(catalog, offset, component);
    auto& shard = shards_[key % active_shards_];
    return {&shard, static_cast<std::size_t>(
        (key / active_shards_) % shard.count)};
}

// SWEGCA: src/swegca_vrs2/store.py@c06092a:599-608
std::shared_ptr<const NativeRegionTopologyFile>
NativeRegionTopologyCache::topology(
    std::shared_ptr<const NativeRegionTopologyCatalog> catalog,
    std::uint64_t offset, std::uint32_t component,
    std::string_view graph_snapshot_id) {
    if (!catalog || offset == 0)
        throw std::runtime_error("region_topology_cache_request_invalid");
    auto [shard, slot_index] = locate(catalog.get(), offset, component);
    if (shard) {
        std::lock_guard guard(shard->mutex);
        const auto& slot = shard->slots[slot_index];
        if (slot.occupied && slot.catalog.get() == catalog.get() &&
            slot.offset == offset && slot.component == component) {
            if (slot.topology->vrs_snapshot_id() != graph_snapshot_id)
                throw std::runtime_error("region_topology_cache_source_changed");
            return slot.topology;
        }
    }
    auto loaded = catalog->open_topology(offset, component);
    if (loaded->vrs_snapshot_id() != graph_snapshot_id ||
        !loaded->converged() || loaded->term_count() == 0 ||
        loaded->term(0) != component)
        throw std::runtime_error("region_topology_cache_source_changed");
    if (shard) {
        std::lock_guard guard(shard->mutex);
        auto& slot = shard->slots[slot_index];
        slot.occupied = false;
        slot.topology.reset();
        slot.catalog.reset();
        slot.offset = offset;
        slot.component = component;
        slot.catalog = std::move(catalog);
        slot.topology = loaded;
        slot.occupied = true;
    }
    return loaded;
}

// SWEGCA: src/swegca_vrs2/store.py@c06092a:599-608
std::shared_ptr<const NativeGraphRegionDirectory>
NativeGraphRegionDirectory::open(
    NativeRegionManifestCursor publication,
    std::shared_ptr<const NativeRegionBindingPageFile> binding_file,
    std::shared_ptr<NativeRegionBindingPageCache> binding_cache,
    std::shared_ptr<const NativeRegionTopologyCatalog> catalog,
    std::shared_ptr<NativeRegionTopologyCache> topology_cache) {
    auto result = std::shared_ptr<NativeGraphRegionDirectory>(
        new NativeGraphRegionDirectory(
            std::move(publication), std::move(binding_file),
            std::move(binding_cache), std::move(catalog),
            std::move(topology_cache)));
    result->validate_complete_directory();
    return result;
}

// SWEGCA: src/swegca_vrs2/store.py@c06092a:599-608
NativeGraphRegionDirectory::NativeGraphRegionDirectory(
    NativeRegionManifestCursor publication,
    std::shared_ptr<const NativeRegionBindingPageFile> binding_file,
    std::shared_ptr<NativeRegionBindingPageCache> binding_cache,
    std::shared_ptr<const NativeRegionTopologyCatalog> catalog,
    std::shared_ptr<NativeRegionTopologyCache> topology_cache)
    : publication_(std::move(publication)),
      catalog_(std::move(catalog)),
      topology_cache_(std::move(topology_cache)) {
    if (!binding_file || !binding_cache || !catalog_ || !topology_cache_ ||
        publication_.manifest_rows == 0 ||
        publication_.published_source_rows <= 0 ||
        !sha256_id(publication_.bindings.graph_snapshot_id) ||
        !sha256_id(publication_.memory_snapshot_id) ||
        !sha256_id(publication_.pair_snapshot_id) ||
        publication_.bindings.journal_generation !=
            binding_file->journal_generation() ||
        publication_.bindings.journal_generation !=
            catalog_->journal_generation() ||
        publication_.pair_snapshot_id != full_current_pair_snapshot_id(
            publication_.memory_snapshot_id,
            publication_.bindings.graph_snapshot_id))
        throw std::runtime_error("native_region_directory_publication_invalid");
    bindings_ = std::make_shared<const NativeRegionBindingView>(
        publication_.bindings, std::move(binding_file),
        std::move(binding_cache));
}

// SWEGCA: src/swegca_vrs2/store.py@c06092a:599-608
void NativeGraphRegionDirectory::validate_complete_directory() const {
    for (std::uint64_t node = 0;
         node < publication_.bindings.node_count; ++node) {
        const auto binding = bindings_->binding(
            static_cast<std::uint32_t>(node));
        if (!binding) continue;
        const auto topology = topology_for(binding->component);
        if (!topology || binding->local >= topology->term_count() ||
            topology->term(binding->local) != node)
            throw std::runtime_error("native_region_directory_binding_invalid");
        if (binding->component != node) continue;
        for (std::uint64_t local = 0;
             local < topology->term_count(); ++local) {
            const auto address = topology->term(
                static_cast<std::uint32_t>(local));
            if (address >= publication_.bindings.node_count)
                throw std::runtime_error(
                    "native_region_directory_topology_invalid");
            const auto reverse = bindings_->binding(address);
            if (!reverse || reverse->component != node ||
                reverse->local != local)
                throw std::runtime_error(
                    "native_region_directory_topology_invalid");
        }
    }
}

// SWEGCA: src/swegca_vrs2/store.py@7536139:282-291
void NativeGraphRegionDirectory::require_source(
    const EventVrsInputView& source) const {
    bindings_->require_source(source);
}

// SWEGCA: src/swegca_vrs2/store.py@7536139:282-291
std::optional<std::uint32_t> NativeGraphRegionDirectory::component_for(
    std::uint32_t node) const {
    const auto binding = bindings_->binding(node);
    return binding ? std::optional<std::uint32_t>(binding->component)
                   : std::nullopt;
}

// SWEGCA: src/swegca_vrs2/store.py@7536139:299-302
std::shared_ptr<const RegionTopologyView>
NativeGraphRegionDirectory::topology_for(std::uint32_t component) const {
    if (component >= publication_.bindings.node_count) return nullptr;
    const auto root = bindings_->binding(component);
    if (!root) return nullptr;
    if (root->component != component || root->local != 0 ||
        root->topology_offset == 0)
        throw std::runtime_error("native_region_directory_root_invalid");
    return topology_cache_->topology(
        catalog_, root->topology_offset, component,
        publication_.bindings.graph_snapshot_id);
}

// SWEGCA: src/swegca_vrs2/store.py@7536139:299-302
std::uint32_t NativeGraphRegionDirectory::local_address(
    std::uint32_t component, std::uint32_t node) const {
    const auto binding = bindings_->binding(node);
    if (!binding || binding->component != component)
        throw std::runtime_error("native_region_directory_component_changed");
    return binding->local;
}

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_connectivity_regions.py@7536139:235-248
void NativeGraphRegionDirectory::require_memory_source(
    const PublishedHotIndex& memory) const {
    if (memory.snapshot_id() != publication_.memory_snapshot_id)
        throw std::runtime_error("native_region_directory_memory_changed");
}

}  // namespace swegca::vrs
