#include "region_preactivation.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <stdexcept>

namespace swegca::vrs {

// SWEGCA: src/tinylm_slicer/mosaic_vrs_portal_activation.py@3bddcb7:29-47
RegionPreactivation preactivate_graph_region_masses(
    const FullCurrentMemoryVrsSnapshot& pair, const DejaVuSignal& signal,
    std::uint32_t component, const EventVrsInputView& inputs,
    const GraphNodeDirectory& nodes, const GraphRegionDirectory& directory) {
    if (signal.snapshot_id != pair.memory().snapshot_id() ||
        pair.vrs_snapshot_id() != inputs.snapshot_id())
        throw std::runtime_error("anonymous signal belongs to another memory snapshot");
    nodes.require_source(inputs);
    directory.require_source(inputs);
    directory.require_memory_source(pair.memory());
    const auto topology = directory.topology_for(component);
    if (!topology || topology->vrs_snapshot_id() != inputs.snapshot_id() ||
        !topology->converged())
        throw std::runtime_error("current converged region topology required");
    std::set<std::uint32_t> local_terms;
    for (const auto& cue : signal.matched_cues) {
        const auto name = "cue:" + cue;
        if (!nodes.contains(name)) continue;
        const auto address = nodes.address(name);
        if (directory.component_for(address) != component) continue;
        const auto local = directory.local_address(component, address);
        if (local >= topology->term_count() || topology->term(local) != address)
            throw std::runtime_error("region cue address changed");
        local_terms.insert(local);
    }
    std::map<std::uint32_t, double> masses;
    for (const auto local : local_terms)
        for (const auto& [group, weight] : topology->memberships_for_term(local)) {
            const auto [found, inserted] = masses.try_emplace(group, 0.0);
            (void)inserted;
            found->second += weight;
        }
    std::vector<std::pair<std::uint32_t, double>> raw;
    for (const auto& [group, weight] : masses)
        raw.emplace_back(group, weight);
    std::sort(raw.begin(), raw.end(), [](const auto& left, const auto& right) {
        if (left.second != right.second) return left.second > right.second;
        return left.first < right.first;
    });
    return RegionPreactivation{pair.snapshot_id(), topology->topology_id(), component, signal,
        std::move(raw), 0.0, {}, static_cast<std::uint32_t>(local_terms.size()),
        false, false};
}

}  // namespace swegca::vrs
