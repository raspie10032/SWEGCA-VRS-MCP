#include "coactivation_associations.hpp"

#include "unicode.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <set>
#include <stdexcept>
#include <string_view>
#include <tuple>

namespace swegca::vrs {
namespace {

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_coactivation_navigation.py@c06092a:77-82
bool nonblank(std::string_view value) {
    const auto points = decode_utf8(value);
    return std::any_of(points.begin(), points.end(), [](std::uint32_t point) {
        return !python_space(point);
    });
}

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_coactivation_navigation.py@c06092a:95-97
bool known_outcome(std::string_view outcome) {
    constexpr std::string_view accepted[] = {
        "success", "failure", "negative", "uncertain", "conflict", "pending"};
    return std::find(std::begin(accepted), std::end(accepted), outcome) != std::end(accepted);
}

struct CurrentOriginal {
    std::optional<HotIndexEpisodeHeader> header;
    std::optional<SharedExperienceBridge> bridge;
};

}  // namespace

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_coactivation_navigation.py@c06092a:77-116
bool CoactivationAssociations::add(std::shared_ptr<const CoactivationEvent> event) {
    if (failed_) throw std::runtime_error("coactivation index must be rebuilt after failure");
    if (!event || event->grants_authority || !nonblank(event->request_id) ||
        event->observed_at_ns < 0)
        throw std::runtime_error("non-authoritative typed main observation required");
    const auto prior = events_.find(event->request_id);
    if (prior != events_.end()) {
        if (*prior->second != *event)
            throw std::runtime_error("request identity reused for a different historical event");
        return false;
    }
    std::map<PostingKey, std::vector<CoactivationWitness>> prepared;
    std::set<std::string> seen;
    for (std::size_t at = 0; at < event->experiences.size(); ++at) {
        const auto& row = event->experiences[at];
        if (!seen.insert(row.episode_id).second)
            throw std::runtime_error("unique typed original experiences required");
        if (std::any_of(row.outcomes.begin(), row.outcomes.end(),
                        [](const auto& outcome) { return !known_outcome(outcome); }))
            throw std::runtime_error("unknown original outcome");
        std::set<std::uint32_t> groups;
        for (const auto& [region, weight] : row.memberships) {
            if (!groups.insert(region).second || !std::isfinite(weight) || weight <= 0)
                throw std::runtime_error("valid unique positive membership required");
            if (!row.topology_id ||
                (event->topology_id && *event->topology_id != *row.topology_id))
                throw std::runtime_error("memberships require a recorded topology");
            prepared[{*row.topology_id, region}].push_back(CoactivationWitness{event, at});
        }
    }
    try {
        events_.emplace(event->request_id, event);
        for (auto& [key, witnesses] : prepared)
            postings_[key].emplace(event->request_id, std::move(witnesses));
    } catch (...) {
        failed_ = true;
        throw;
    }
    return true;
}

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_coactivation_navigation.py@c06092a:118-183
CoactivationAssociationsReceipt CoactivationAssociations::query(
    const FullCurrentMemoryVrsSnapshot& pair, const EventVrsInputView& inputs,
    const GraphNodeDirectory& nodes, const GraphRegionDirectory& regions,
    std::uint32_t component, std::uint32_t origin_region) const {
    if (failed_) throw std::runtime_error("coactivation index must be rebuilt after failure");
    if (pair.vrs_snapshot_id() != inputs.snapshot_id())
        throw std::runtime_error("association topology belongs to another VRS generation");
    nodes.require_source(inputs);
    regions.require_source(inputs);
    regions.require_memory_source(pair.memory());
    const auto topology = regions.topology_for(component);
    if (!topology || topology->vrs_snapshot_id() != inputs.snapshot_id() ||
        !topology->converged())
        throw std::runtime_error("converged topology required");
    if (origin_region >= topology->region_count() ||
        topology->region_size(origin_region) == 0)
        throw std::runtime_error("existing integer origin region required");
    const auto postings = postings_.find({topology->topology_id(), origin_region});
    using AssociationKey = std::tuple<std::uint32_t, std::string, std::string>;
    std::map<AssociationKey, std::vector<CoactivationWitness>> accepted;
    std::map<AssociationKey, SharedExperienceBridge> bridges;
    std::vector<RejectedCoactivation> rejected;
    std::map<std::string, CurrentOriginal> current;
    std::size_t examined = 0;
    if (postings != postings_.end()) {
        for (const auto& [request, witnesses] : postings->second) {
            (void)request;
            for (const auto& witness : witnesses) {
                ++examined;
                const auto& event = *witness.event;
                const auto& row = witness.experience();
                std::string reason;
                if (event.pair_snapshot_id != pair.snapshot_id() ||
                    event.memory_snapshot_id != pair.memory().snapshot_id() ||
                    event.vrs_snapshot_id != pair.vrs_snapshot_id()) {
                    reason = "historical_snapshot_not_current";
                } else {
                    auto found = current.find(row.episode_id);
                    if (found == current.end()) {
                        CurrentOriginal item;
                        try {
                            item.header = pair.memory().episode_header(row.episode_id);
                            item.bridge = graph_bridge_for_episode(row.episode_id, pair,
                                inputs, nodes, regions);
                        } catch (const std::out_of_range&) {
                            item.header.reset();
                            item.bridge.reset();
                        }
                        found = current.emplace(row.episode_id, std::move(item)).first;
                    }
                    const auto& item = found->second;
                    if (!item.header) {
                        reason = "original_address_unavailable";
                    } else if (item.header->revision != row.revision ||
                               item.header->source_addresses != row.source_addresses ||
                               item.header->historical_outcomes != row.outcomes ||
                               !row.topology_id ||
                               *row.topology_id != topology->topology_id()) {
                        reason = "original_lineage_changed";
                    } else if (!item.bridge) {
                        reason = "not_a_shared_original_experience";
                    } else if (item.bridge->memberships != row.memberships) {
                        reason = "recorded_memberships_changed";
                    } else if (std::none_of(item.bridge->memberships.begin(),
                                            item.bridge->memberships.end(),
                                            [origin_region](const auto& membership) {
                                                return membership.first == origin_region;
                                            })) {
                        reason = "origin_not_in_shared_experience";
                    } else {
                        for (const auto& [destination, weight] : item.bridge->memberships) {
                            (void)weight;
                            if (destination == origin_region) continue;
                            AssociationKey key{destination, item.bridge->episode_id,
                                               item.bridge->revision};
                            accepted[key].push_back(witness);
                            bridges.insert_or_assign(key, *item.bridge);
                        }
                    }
                }
                if (!reason.empty())
                    rejected.push_back(RejectedCoactivation{
                        event.request_id, row.episode_id, std::move(reason)});
            }
        }
    }
    std::vector<ObservedSharedAssociation> associations;
    associations.reserve(accepted.size());
    for (auto& [key, witnesses] : accepted) {
        std::sort(witnesses.begin(), witnesses.end(), [](const auto& left, const auto& right) {
            return left.event->request_id < right.event->request_id;
        });
        associations.push_back(ObservedSharedAssociation{
            origin_region, std::get<0>(key), bridges.at(key), std::move(witnesses),
            std::nullopt, std::nullopt, false});
    }
    std::sort(associations.begin(), associations.end(), [](const auto& left, const auto& right) {
        if (left.activation_request_count() != right.activation_request_count())
            return left.activation_request_count() > right.activation_request_count();
        if (left.destination_region != right.destination_region)
            return left.destination_region < right.destination_region;
        if (left.bridge.episode_id != right.bridge.episode_id)
            return left.bridge.episode_id < right.bridge.episode_id;
        return left.bridge.revision < right.bridge.revision;
    });
    std::sort(rejected.begin(), rejected.end(), [](const auto& left, const auto& right) {
        return std::tie(left.request_id, left.episode_id, left.reason) <
               std::tie(right.request_id, right.episode_id, right.reason);
    });
    return CoactivationAssociationsReceipt{
        pair.snapshot_id(), topology->topology_id(), origin_region,
        std::move(associations), std::move(rejected), events_.size(),
        postings == postings_.end() ? 0 : postings->second.size(), examined,
        false, false};
}

}  // namespace swegca::vrs
