#include "main_read_generation.hpp"

#include <stdexcept>
#include <utility>

namespace swegca::vrs {

// SWEGCA: src/swegca_vrs2/store.py@7536139:371-405
// SWEGCA: user@2026-09-22:13-21
MainReadGeneration::MainReadGeneration(
    std::shared_ptr<const FullCurrentMemoryVrsSnapshot> pair,
    std::shared_ptr<const ValidatedEventVrsInputs> inputs,
    std::shared_ptr<const GraphNodeDirectory> nodes,
    std::shared_ptr<const GraphRegionDirectory> regions,
    std::shared_ptr<const CoactivationAssociations> associations,
    PortalPolicy policy, std::vector<PortalRevocation> revocations)
    // SWEGCA: src/swegca_vrs2/store.py@7536139:371-405
    : pair_(std::move(pair)), inputs_(std::move(inputs)),
      nodes_(std::move(nodes)), regions_(std::move(regions)),
      associations_(std::move(associations)), policy_(std::move(policy)),
      revocations_(std::move(revocations)) {
    if (!pair_ || !inputs_ || !nodes_ || !regions_ || !associations_)
        throw std::runtime_error("complete Main read generation required");
    const auto& source = inputs_->require_validated_immutable();
    if (pair_->vrs_snapshot_id() != source.snapshot_id())
        throw std::runtime_error("read generation belongs to another VRS snapshot");
    nodes_->require_source(source);
    regions_->require_source(source);
    regions_->require_memory_source(pair_->memory());
}

// SWEGCA: user@2026-09-22:13-21
PinnedReadLayer MainReadGeneration::layer() const {
    return PinnedReadLayer{*pair_, inputs_->require_validated_immutable(), *nodes_,
                           *regions_, *associations_, policy_, revocations_};
}

}  // namespace swegca::vrs
