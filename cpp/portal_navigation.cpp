#include "portal_navigation.hpp"

#include <algorithm>
#include <cstddef>
#include <set>
#include <stdexcept>
#include <utility>

namespace swegca::vrs {

// SWEGCA: src/tinylm_slicer/mosaic_vrs_portal_activation.py@3bddcb7:81-122
PortalNavigationRecall recall_after_deja_vu_navigation(
    const FullCurrentMemoryVrsSnapshot& pair, const DejaVuSignal& signal,
    const EventVrsInputView& inputs, const GraphNodeDirectory& nodes,
    const GraphRegionDirectory& regions,
    const CoactivationAssociations& associations, const PortalPolicy& policy,
    std::int64_t observed_at_ns, std::uint64_t navigation_term_budget,
    const std::vector<PortalRevocation>& revocations) {
    if (navigation_term_budget == 0)
        throw std::runtime_error("explicit positive navigation term work budget required");
    if (signal.snapshot_id != pair.memory().snapshot_id() ||
        pair.vrs_snapshot_id() != inputs.snapshot_id())
        throw std::runtime_error("anonymous signal belongs to another generation");
    nodes.require_source(inputs);
    regions.require_source(inputs);
    regions.require_memory_source(pair.memory());
    std::vector<std::uint32_t> components;
    std::set<std::uint32_t> seen;
    for (const auto& cue : signal.matched_cues) {
        const auto name = "cue:" + cue;
        if (!nodes.contains(name)) continue;
        const auto component = regions.component_for(nodes.address(name));
        if (component && seen.insert(*component).second)
            components.push_back(*component);
    }
    std::vector<RegionPreactivation> preactivations;
    std::vector<RegionOrigin> origins;
    std::vector<PortalNavigationFailure> failures;
    for (const auto component : components) {
        try {
            auto preactivation = preactivate_graph_regions(pair, signal, component,
                                                            inputs, nodes, regions);
            for (const auto& [region, weight] : preactivation.regions)
                origins.push_back(RegionOrigin{component, preactivation.topology_id,
                                               region, weight});
            preactivations.push_back(std::move(preactivation));
        } catch (const std::runtime_error& error) {
            failures.push_back(PortalNavigationFailure{component, std::nullopt,
                                                       error.what()});
        } catch (const std::out_of_range& error) {
            failures.push_back(PortalNavigationFailure{component, std::nullopt,
                                                       error.what()});
        }
    }
    // The author compares all region masses in one normalized topology. The
    // product stores disconnected components separately, so compare their
    // already normalized masses globally and use component only as the stable
    // adaptation tie break.
    // SWEGCA: src/tinylm_slicer/mosaic_vrs_portal_activation.py@3bddcb7:33-47
    std::sort(origins.begin(), origins.end(), [](const auto& left,
                                                  const auto& right) {
        if (left.weight != right.weight) return left.weight > right.weight;
        if (left.component != right.component)
            return left.component < right.component;
        return left.region < right.region;
    });
    std::vector<PortalPlan> plans;
    std::optional<PortalCandidate> selected;
    std::optional<RegionNavigationPage> page;
    std::vector<std::string> navigation;
    std::vector<RegionOrigin> deferred;
    for (std::size_t position = 0; position < origins.size(); ++position) {
        const auto& origin = origins[position];
        try {
            auto source = associations.query(pair, inputs, nodes, regions,
                                             origin.component, origin.region);
            if (source.topology_id != origin.topology_id)
                throw std::runtime_error("portal query changed the pinned generation");
            auto plan = plan_portals(source, policy, observed_at_ns, revocations);
            plans.push_back(std::move(plan));
            if (!plans.back().selected) continue;
            const auto& candidate = *plans.back().selected;
            auto cursor = start_graph_region_navigation(pair, inputs, nodes, regions,
                candidate.association.bridge, origin.region,
                candidate.key.destination_region);
            auto first = next_graph_region_cues(pair, inputs, nodes, regions,
                                                cursor, navigation_term_budget);
            navigation = first.cues;
            page = std::move(first);
            selected = candidate;
            deferred.assign(origins.begin() + static_cast<std::ptrdiff_t>(position + 1),
                            origins.end());
            break;
        } catch (const std::runtime_error& error) {
            failures.push_back(PortalNavigationFailure{
                origin.component, origin.region, error.what()});
        } catch (const std::out_of_range& error) {
            failures.push_back(PortalNavigationFailure{
                origin.component, origin.region, error.what()});
        }
    }
    auto recalled = recall_memory(pair.memory(), signal, navigation);
    return PortalNavigationRecall{signal, std::move(preactivations), std::move(plans),
        std::move(selected), std::move(deferred), std::move(navigation),
        std::move(failures), std::move(page), std::move(recalled),
        false, false, false};
}

}  // namespace swegca::vrs
