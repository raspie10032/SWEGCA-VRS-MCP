#pragma once

#include "graph_append.hpp"
#include "json.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace swegca::vrs {

// One immutable, generation-bound topology for an affected graph component.
// It keeps the complete original edge arrays and overlapping memberships.
class ConnectivityRegions {
public:
    // SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_connectivity_regions.py@7536139:159-199
    [[nodiscard]] static ConnectivityRegions build(
        std::shared_ptr<const AffectedGraphComponent> source,
        std::string vrs_snapshot_id,
        std::uint32_t maximum_sweeps = 100,
        std::uint32_t maximum_levels = 32);

    // SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_connectivity_regions.py@7536139:201-203
    void require_source(const AffectedGraphComponent& source,
                        const std::string& vrs_snapshot_id) const;
    // SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_connectivity_regions.py@7536139:205-211
    [[nodiscard]] std::vector<std::pair<std::uint32_t, double>> memberships_for_term(
        std::uint32_t local_node) const;
    // SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_connectivity_regions.py@7536139:275-281
    [[nodiscard]] Json receipt() const;

    // SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_connectivity_regions.py@7536139:138-157
    [[nodiscard]] const std::string& vrs_snapshot_id() const { return vrs_snapshot_id_; }
    // SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_connectivity_regions.py@7536139:138-157
    [[nodiscard]] const std::string& topology_id() const { return topology_id_; }
    // SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_connectivity_regions.py@7536139:138-157
    [[nodiscard]] bool converged() const { return converged_; }
    // SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_connectivity_regions.py@7536139:138-157
    [[nodiscard]] const std::vector<std::uint32_t>& terms() const { return terms_; }
    // SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_connectivity_regions.py@7536139:138-157
    [[nodiscard]] const std::vector<std::uint32_t>& core_labels() const { return core_labels_; }
    // SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_connectivity_regions.py@7536139:138-157
    [[nodiscard]] const std::vector<std::uint64_t>& region_offsets() const { return region_offsets_; }
    // SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_connectivity_regions.py@7536139:138-157
    [[nodiscard]] const std::vector<std::uint32_t>& region_nodes() const { return region_nodes_; }
    // SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_connectivity_regions.py@7536139:138-157
    [[nodiscard]] const std::vector<std::uint32_t>& sweeps() const { return sweeps_; }

private:
    ConnectivityRegions() = default;

    std::string vrs_snapshot_id_;
    std::string topology_id_;
    std::shared_ptr<const AffectedGraphComponent> source_;
    std::vector<std::uint32_t> terms_;
    std::vector<std::uint32_t> core_labels_;
    std::vector<std::uint32_t> edge_source_;
    std::vector<std::uint32_t> edge_target_;
    std::vector<std::int8_t> edge_sign_;
    std::vector<double> strengths_;
    std::vector<std::uint64_t> member_offsets_;
    std::vector<std::uint32_t> member_regions_;
    std::vector<double> member_weights_;
    std::vector<std::uint64_t> region_offsets_;
    std::vector<std::uint32_t> region_nodes_;
    bool converged_ = false;
    std::vector<std::uint32_t> sweeps_;
};

}  // namespace swegca::vrs
