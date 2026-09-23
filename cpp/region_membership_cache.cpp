#include "region_membership_cache.hpp"

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace swegca::vrs {

// SWEGCA: src/tinylm_slicer/mosaic_vrs_membership_cache.py@3bddcb7:31-35
bool GraphMembershipCache::Key::operator<(const Key& other) const {
    return std::tie(episode_id, revision, cues) <
        std::tie(other.episode_id, other.revision, other.cues);
}

GraphMembershipCache::GraphMembershipCache(
    std::shared_ptr<const FullCurrentMemoryVrsSnapshot> pair,
    std::shared_ptr<const ValidatedEventVrsInputs> inputs,
    std::shared_ptr<const GraphNodeDirectory> nodes,
    std::shared_ptr<const GraphRegionDirectory> regions,
    std::uint64_t maximum_entries,
    std::uint64_t maximum_estimated_bytes)
    // SWEGCA: src/tinylm_slicer/mosaic_vrs_membership_cache.py@3bddcb7:14-25
    : pair_(std::move(pair)), inputs_(std::move(inputs)),
      nodes_(std::move(nodes)),
      regions_(std::move(regions)), maximum_entries_(maximum_entries),
      maximum_estimated_bytes_(maximum_estimated_bytes) {
    if (!pair_ || !inputs_ || !nodes_ || !regions_ || maximum_entries_ == 0 ||
        maximum_estimated_bytes_ == 0)
        throw std::runtime_error("membership_cache_configuration_invalid");
    const auto& source = inputs_->require_validated_immutable();
    graph_snapshot_id_ = source.snapshot_id();
    if (pair_->vrs_snapshot_id() != graph_snapshot_id_)
        throw std::runtime_error("membership_cache_configuration_invalid");
    nodes_->require_source(source);
    regions_->require_source(source);
    regions_->require_memory_source(pair_->memory());
}

// SWEGCA: src/tinylm_slicer/mosaic_vrs_membership_cache.py@3bddcb7:27-30
void GraphMembershipCache::require(
    const FullCurrentMemoryVrsSnapshot& pair,
    const EventVrsInputView& inputs,
    const GraphNodeDirectory& nodes,
    const GraphRegionDirectory& regions) const {
    if (&pair != pair_.get() || pair.vrs_snapshot_id() != graph_snapshot_id_ ||
        inputs.snapshot_id() != graph_snapshot_id_ ||
        &inputs != &inputs_->require_validated_immutable() ||
        &nodes != nodes_.get() || &regions != regions_.get())
        throw std::runtime_error("membership_cache_generation_mismatch");
}

// Conservative derived-row estimate from the author's cache. This remains an
// estimate; the Main-wide RSS owner must account for allocator overhead.
// SWEGCA: src/tinylm_slicer/mosaic_vrs_membership_cache.py@3bddcb7:43-55
std::uint64_t GraphMembershipCache::estimated_charge(const Row& row) {
    std::uint64_t result = 0;
    const auto add = [&result](std::uint64_t amount) {
        if (result > std::numeric_limits<std::uint64_t>::max() - amount)
            throw std::runtime_error("membership_cache_charge_overflow");
        result += amount;
    };
    add(256);
    add(sizeof(Row));
    add(sizeof(Key));
    // std::map owns a second Key. Include both copies' dynamic storage rather
    // than hiding it in the fixed bookkeeping allowance.
    for (int copy = 0; copy < 2; ++copy) {
        add(row.key.episode_id.capacity());
        add(row.key.revision.capacity());
        if (row.key.cues.capacity() >
            std::numeric_limits<std::uint64_t>::max() / sizeof(std::string))
            throw std::runtime_error("membership_cache_charge_overflow");
        add(row.key.cues.capacity() * sizeof(std::string));
        for (const auto& cue : row.key.cues) add(cue.capacity());
    }
    add(row.value.topology_id.capacity());
    if (row.value.memberships.capacity() >
        std::numeric_limits<std::uint64_t>::max() /
            sizeof(std::pair<std::uint32_t, double>))
        throw std::runtime_error("membership_cache_charge_overflow");
    add(row.value.memberships.capacity() *
        sizeof(std::pair<std::uint32_t, double>));
    return result;
}

// SWEGCA: src/tinylm_slicer/mosaic_vrs_membership_cache.py@3bddcb7:31-58
CachedGraphMemberships GraphMembershipCache::derive(
    const HotIndexEpisodeHeader& episode,
    const FullCurrentMemoryVrsSnapshot& pair) const {
    const auto& source = inputs_->require_validated_immutable();
    require(pair, source, *nodes_, *regions_);
    const auto address = nodes_->address(episode.episode_id);
    const auto component = regions_->component_for(address);
    if (!component)
        throw std::runtime_error("membership_cache_component_missing");
    const auto topology = regions_->topology_for(*component);
    if (!topology || topology->vrs_snapshot_id() != graph_snapshot_id_)
        throw std::runtime_error("membership_cache_topology_changed");
    return CachedGraphMemberships{
        topology->topology_id(),
        graph_episode_memberships(episode.episode_id, pair, source,
                                  *nodes_, *regions_)};
}

// Derivation remains under the same mutex as lookup and counters, exactly as
// the source OrderedDict path. This preserves one miss and one insertion for
// each absent key instead of inventing a race-dependent second result.
// SWEGCA: src/tinylm_slicer/mosaic_vrs_membership_cache.py@3bddcb7:31-58
CachedGraphMemberships GraphMembershipCache::memberships(
    const HotIndexEpisodeHeader& episode,
    const FullCurrentMemoryVrsSnapshot& pair) {
    if (episode.episode_id.empty() || episode.revision.empty())
        throw std::runtime_error("membership_cache_episode_invalid");
    const auto& source = inputs_->require_validated_immutable();
    require(pair, source, *nodes_, *regions_);
    Key key{episode.episode_id, episode.revision, episode.cues};
    std::lock_guard guard(mutex_);
    const auto found = index_.find(key);
    if (found != index_.end()) {
        ++hits_;
        rows_.splice(rows_.end(), rows_, found->second);
        found->second = std::prev(rows_.end());
        return found->second->value;
    }
    ++misses_;
    auto value = derive(episode, pair);
    Row candidate{std::move(key), value, 0};
    candidate.charge = estimated_charge(candidate);
    if (candidate.charge > maximum_estimated_bytes_) {
        ++oversized_;
        return value;
    }
    while (!rows_.empty() &&
           (rows_.size() >= maximum_entries_ ||
            candidate.charge > maximum_estimated_bytes_ - estimated_bytes_)) {
        estimated_bytes_ -= rows_.front().charge;
        index_.erase(rows_.front().key);
        rows_.pop_front();
        ++evictions_;
    }
    estimated_bytes_ += candidate.charge;
    rows_.push_back(std::move(candidate));
    auto inserted = std::prev(rows_.end());
    index_.emplace(inserted->key, inserted);
    return inserted->value;
}

// SWEGCA: src/tinylm_slicer/mosaic_vrs_membership_cache.py@3bddcb7:60-66
std::optional<SharedExperienceBridge> GraphMembershipCache::bridge(
    const HotIndexEpisodeHeader& episode,
    const FullCurrentMemoryVrsSnapshot& pair) {
    auto value = memberships(episode, pair);
    if (value.memberships.size() < 2) return std::nullopt;
    return SharedExperienceBridge{
        pair.snapshot_id(), std::move(value.topology_id), episode.episode_id,
        episode.revision, episode.source_addresses,
        episode.historical_outcomes, std::move(value.memberships), false};
}

// SWEGCA: src/tinylm_slicer/mosaic_vrs_membership_cache.py@3bddcb7:68-74
std::uint64_t GraphMembershipCache::estimated_bytes() const {
    std::lock_guard guard(mutex_);
    return estimated_bytes_;
}

// SWEGCA: src/tinylm_slicer/mosaic_vrs_membership_cache.py@3bddcb7:68-74
std::uint64_t GraphMembershipCache::entry_count() const {
    std::lock_guard guard(mutex_);
    return rows_.size();
}

// SWEGCA: src/tinylm_slicer/mosaic_vrs_membership_cache.py@3bddcb7:68-74
GraphMembershipCacheStatus GraphMembershipCache::status() const {
    std::lock_guard guard(mutex_);
    return GraphMembershipCacheStatus{
        rows_.size(), estimated_bytes_, maximum_entries_,
        maximum_estimated_bytes_, hits_, misses_, evictions_, oversized_,
        pair_->snapshot_id(), graph_snapshot_id_, false};
}

}  // namespace swegca::vrs
