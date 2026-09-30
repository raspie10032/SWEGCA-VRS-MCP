#include "world/ordinary_source_router.hpp"

#include "world/compressed_memory.hpp"

#include <limits>
#include <stdexcept>

namespace swegca::world {

OrdinarySourceRouter::OrdinarySourceRouter(
    std::vector<std::shared_ptr<const HotMemoryIndex>> sources,
    const std::size_t maximum_entries,
    const std::size_t maximum_estimated_bytes)
    : sources_(std::move(sources)), maximum_entries_(maximum_entries),
      maximum_estimated_bytes_(maximum_estimated_bytes) {
    if (!maximum_entries_ || !maximum_estimated_bytes_)
        throw std::invalid_argument("positive address memo capacity required");
    for (const auto& source : sources_) {
        if (!source) throw std::invalid_argument("ordinary source required");
        if (std::dynamic_pointer_cast<const MemoryActivationIndex>(source) ||
            std::dynamic_pointer_cast<const CompressedMemoryActivationIndex>(source))
            known_.push_back(source);
        else
            unknown_.push_back(source);
    }
}

std::vector<std::shared_ptr<const HotMemoryIndex>>
OrdinarySourceRouter::known_sources_for(const std::string_view episode_id) const {
    {
        std::lock_guard lock(mutex_);
        const auto found = routes_.find(episode_id);
        if (found != routes_.end()) {
            order_.splice(order_.end(), order_, found->second.lru);
            return found->second.routes;
        }
    }

    std::vector<std::shared_ptr<const HotMemoryIndex>> routed;
    for (const auto& source : known_)
        if (source->contains_episode(episode_id)) routed.push_back(source);

    const auto fixed = sizeof(Entry) + sizeof(std::string) + 256U;
    const auto route_bytes = routed.size() * sizeof(routed.front());
    if (episode_id.size() <= std::numeric_limits<std::size_t>::max() - fixed - route_bytes) {
        const auto charge = fixed + route_bytes + episode_id.size();
        if (charge <= maximum_estimated_bytes_) {
            std::lock_guard lock(mutex_);
            if (const auto concurrent = routes_.find(episode_id); concurrent != routes_.end()) {
                order_.splice(order_.end(), order_, concurrent->second.lru);
                return concurrent->second.routes;
            }
            while (!order_.empty() &&
                   (routes_.size() >= maximum_entries_ ||
                    estimated_bytes_ > maximum_estimated_bytes_ - charge)) {
                const auto oldest = order_.front();
                const auto found = routes_.find(oldest);
                estimated_bytes_ -= found->second.charge;
                routes_.erase(found);
                order_.pop_front();
            }
            order_.emplace_back(episode_id);
            auto at = std::prev(order_.end());
            routes_.emplace(*at, Entry{routed, charge, at});
            estimated_bytes_ += charge;
        }
    }
    return routed;
}

const MemoryEpisode& OrdinarySourceRouter::episode(const std::string_view episode_id) const {
    const MemoryEpisode* result{};
    auto accept = [&](const std::shared_ptr<const HotMemoryIndex>& source) {
        try {
            const auto& candidate = source->episode(episode_id);
            if (result) throw std::invalid_argument("ordinary hot-memory episode identity overlaps");
            result = &candidate;
        } catch (const std::out_of_range&) {
        }
    };
    for (const auto& source : known_sources_for(episode_id)) accept(source);
    for (const auto& source : unknown_) accept(source);
    if (!result) throw std::out_of_range("ordinary hot-memory episode unavailable");
    return *result;
}

std::size_t OrdinarySourceRouter::estimated_bytes() const {
    std::lock_guard lock(mutex_);
    return estimated_bytes_;
}

const std::vector<std::shared_ptr<const HotMemoryIndex>>&
OrdinarySourceRouter::sources() const noexcept { return sources_; }

}  // namespace swegca::world
