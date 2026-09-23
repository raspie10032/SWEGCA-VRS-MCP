#pragma once

#include "connectivity_regions.hpp"
#include "owner_lock.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace swegca::vrs {

// Immutable checksummed physical representation of one complete affected
// component topology. Seven fixed-width sections preserve the author's term,
// core, overlapping membership and reverse region-node arrays. Values are
// addressed through 4 KiB payload blocks; open validates without retaining
// the full arrays in RAM.
// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_connectivity_regions.py@7536139:138-199
class NativeRegionTopologyFile final : public RegionTopologyView {
public:
    // The source topology is already converged and belongs to the committed
    // numerical generation. This writes an unpublished derived file.
    // SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_connectivity_regions.py@7536139:159-199
    [[nodiscard]] static std::shared_ptr<const NativeRegionTopologyFile> create(
        const std::filesystem::path& directory,
        std::string journal_generation,
        std::uint32_t component,
        const ConnectivityRegions& topology,
        OwnerLock& owner);

    // Cold recovery requires the manifest's exact metadata and validates all
    // block checksums plus array bounds before returning the view.
    // SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_connectivity_regions.py@7536139:138-203
    [[nodiscard]] static std::shared_ptr<const NativeRegionTopologyFile> open(
        std::filesystem::path path,
        std::string journal_generation,
        std::uint32_t component,
        std::string vrs_snapshot_id,
        std::string topology_id);

    // SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_connectivity_regions.py@7536139:138-157
    [[nodiscard]] const std::string& vrs_snapshot_id() const override {
        return vrs_snapshot_id_;
    }
    // SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_connectivity_regions.py@7536139:138-157
    [[nodiscard]] const std::string& topology_id() const override {
        return topology_id_;
    }
    // SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_connectivity_regions.py@7536139:138-157
    [[nodiscard]] bool converged() const override { return converged_; }
    // SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_connectivity_regions.py@7536139:138-157
    [[nodiscard]] std::uint64_t term_count() const override;
    // SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_connectivity_regions.py@7536139:138-157
    [[nodiscard]] std::uint32_t term(std::uint32_t local) const override;
    // SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_connectivity_regions.py@7536139:138-157
    [[nodiscard]] std::uint32_t core_label(std::uint32_t local) const override;
    // SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_region_arrays.py@7536139:29-36
    [[nodiscard]] std::uint64_t region_count() const override;
    // SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_region_arrays.py@7536139:29-36
    [[nodiscard]] std::uint64_t region_size(std::uint32_t region) const override;
    // SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_region_arrays.py@7536139:29-36
    [[nodiscard]] std::uint32_t region_node(
        std::uint32_t region, std::uint64_t offset) const override;
    // SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_connectivity_regions.py@7536139:205-211
    [[nodiscard]] std::vector<std::pair<std::uint32_t, double>>
    memberships_for_term(std::uint32_t local_node) const override;

    // SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:91-127
    [[nodiscard]] const std::filesystem::path& path() const { return path_; }
    // SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:91-127
    [[nodiscard]] const std::string& journal_generation() const {
        return journal_generation_;
    }
    // SWEGCA: src/swegca_vrs2/store.py@7536139:282-302
    [[nodiscard]] std::uint32_t component() const { return component_; }

private:
    enum class Section : std::size_t {
        terms,
        core_labels,
        member_offsets,
        member_regions,
        member_weights,
        region_offsets,
        region_nodes,
        count
    };

    NativeRegionTopologyFile() = default;

    [[nodiscard]] std::vector<std::uint64_t> read_values(
        Section section, std::uint64_t first, std::uint64_t count) const;
    void validate_file() const;

    std::filesystem::path path_;
    std::string journal_generation_;
    std::string vrs_snapshot_id_;
    std::string topology_id_;
    std::uint32_t component_ = 0;
    bool converged_ = false;
    std::array<std::uint64_t, static_cast<std::size_t>(Section::count)> counts_{};
    std::array<std::uint32_t, static_cast<std::size_t>(Section::count)> widths_{};
    std::array<std::uint32_t, static_cast<std::size_t>(Section::count)> blocks_{};
    std::array<std::uint64_t, static_cast<std::size_t>(Section::count)> starts_{};
};

}  // namespace swegca::vrs
