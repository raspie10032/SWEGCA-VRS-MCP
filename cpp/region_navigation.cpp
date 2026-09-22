#include "region_navigation.hpp"

#include <algorithm>
#include <set>
#include <stdexcept>
#include <utility>

namespace swegca::vrs {
namespace {

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_local_navigation.py@c06092a:44-46
bool same_bridge(const SharedExperienceBridge& left,
                 const SharedExperienceBridge& right) {
    return left.pair_snapshot_id == right.pair_snapshot_id &&
        left.topology_id == right.topology_id &&
        left.episode_id == right.episode_id &&
        left.revision == right.revision &&
        left.source_addresses == right.source_addresses &&
        left.outcomes == right.outcomes &&
        left.memberships == right.memberships &&
        left.grants_authority == right.grants_authority;
}

struct NavigationSeeds {
    std::shared_ptr<const ConnectivityRegions> topology;
    std::vector<std::uint32_t> nodes;
};

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_local_navigation.py@c06092a:37-54
NavigationSeeds seeds(const FullCurrentMemoryVrsSnapshot& pair,
                      const EventVrsInputView& inputs,
                      const GraphNodeDirectory& nodes,
                      const GraphRegionDirectory& regions,
                      const SharedExperienceBridge& bridge,
                      std::uint32_t origin, std::uint32_t destination) {
    if (origin == destination || bridge.grants_authority)
        throw std::runtime_error("non-authoritative region transition required");
    const auto current = graph_bridge_for_episode(bridge.episode_id, pair, inputs,
                                                   nodes, regions);
    if (!current || !same_bridge(*current, bridge))
        throw std::runtime_error("shared original or navigation generation changed");
    bool has_origin = false, has_destination = false;
    for (const auto& [region, weight] : bridge.memberships) {
        (void)weight;
        has_origin |= region == origin;
        has_destination |= region == destination;
    }
    if (!has_origin || !has_destination)
        throw std::runtime_error("shared original does not join requested regions");
    const auto component = regions.component_for(nodes.address(bridge.episode_id));
    if (!component) throw std::runtime_error("graph original has no component");
    const auto topology = regions.topology_for(*component);
    if (!topology || topology->topology_id() != bridge.topology_id ||
        !topology->converged())
        throw std::runtime_error("current converged hot region required");
    const auto episode = pair.memory().episode_header(bridge.episode_id);
    std::set<std::uint32_t> found;
    for (const auto& cue : episode.cues) {
        const auto name = "cue:" + cue;
        if (!nodes.contains(name)) continue;
        const auto address = nodes.address(name);
        if (regions.component_for(address) != component) continue;
        const auto local = regions.local_address(*component, address);
        if (local >= topology->terms().size() || topology->terms()[local] != address)
            throw std::runtime_error("region cue address changed");
        if (topology->core_labels()[local] == destination) found.insert(local);
    }
    return NavigationSeeds{topology, {found.begin(), found.end()}};
}

}  // namespace

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_local_navigation.py@c06092a:57-59
RegionNavigationCursor start_graph_region_navigation(
    const FullCurrentMemoryVrsSnapshot& pair, const EventVrsInputView& inputs,
    const GraphNodeDirectory& nodes, const GraphRegionDirectory& regions,
    const SharedExperienceBridge& bridge, std::uint32_t origin_region,
    std::uint32_t destination_region) {
    auto seeded = seeds(pair, inputs, nodes, regions, bridge,
                        origin_region, destination_region);
    return RegionNavigationCursor{bridge, origin_region, destination_region,
                                  std::move(seeded.nodes)};
}

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_local_navigation.py@c06092a:62-107
RegionNavigationPage next_graph_region_cues(
    const FullCurrentMemoryVrsSnapshot& pair, const EventVrsInputView& inputs,
    const GraphNodeDirectory& nodes, const GraphRegionDirectory& regions,
    const RegionNavigationCursor& cursor, std::uint64_t work_budget) {
    if (work_budget == 0)
        throw std::runtime_error("explicit positive integer navigation work budget required");
    const auto seeded = seeds(pair, inputs, nodes, regions, cursor.bridge,
                              cursor.origin_region, cursor.destination_region);
    const auto& topology = *seeded.topology;
    const auto& offsets = topology.region_offsets();
    const auto destination = cursor.destination_region;
    if (destination + std::uint64_t{1} >= offsets.size())
        throw std::runtime_error("navigation destination changed");
    const auto begin = offsets[destination];
    const auto term_count = offsets[destination + 1] - begin;
    if (cursor.seed_nodes != seeded.nodes ||
        cursor.seed_offset > seeded.nodes.size() ||
        cursor.region_offset > term_count || cursor.emitted_terms > term_count ||
        (cursor.region_offset != 0 && cursor.seed_offset != seeded.nodes.size()))
        throw std::runtime_error("navigation cursor content changed");
    const auto skipped = cursor.region_offset == 0 ? std::uint64_t{0} :
        static_cast<std::uint64_t>(std::upper_bound(
            seeded.nodes.begin(), seeded.nodes.end(),
            topology.region_nodes()[begin + cursor.region_offset - 1]) - seeded.nodes.begin());
    if (skipped > cursor.seed_offset + cursor.region_offset ||
        cursor.emitted_terms != cursor.seed_offset + cursor.region_offset - skipped)
        throw std::runtime_error("navigation cursor progress count changed");
    auto seed_offset = cursor.seed_offset;
    auto offset = cursor.region_offset;
    auto emitted = cursor.emitted_terms;
    std::vector<std::uint32_t> page_nodes;
    std::uint64_t visited = 0;
    while (visited < work_budget && emitted < term_count) {
        std::uint32_t local;
        if (seed_offset < seeded.nodes.size()) {
            local = seeded.nodes[seed_offset++];
        } else if (offset < term_count) {
            local = topology.region_nodes()[begin + offset++];
            if (std::binary_search(seeded.nodes.begin(), seeded.nodes.end(), local)) {
                ++visited;
                continue;
            }
        } else {
            break;
        }
        ++visited;
        page_nodes.push_back(local);
        ++emitted;
    }
    std::vector<std::string> cues;
    for (const auto local : page_nodes) {
        const auto address = topology.terms()[local];
        const auto name = nodes.name(address);
        if (!nodes.contains(name) || nodes.address(name) != address)
            throw std::runtime_error("region node directory changed");
        if (name.rfind("cue:", 0) == 0) cues.push_back(name.substr(4));
    }
    const bool done = emitted == term_count;
    std::optional<RegionNavigationCursor> next;
    if (!done)
        next = RegionNavigationCursor{cursor.bridge, cursor.origin_region,
            cursor.destination_region, cursor.seed_nodes, seed_offset, offset, emitted};
    return RegionNavigationPage{cursor, std::move(next), std::move(page_nodes),
        std::move(cues), visited, term_count - emitted, done, false, false};
}

}  // namespace swegca::vrs
