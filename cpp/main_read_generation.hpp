#pragma once

#include "session_first_read.hpp"

#include <memory>
#include <vector>

namespace swegca::vrs {

// Main retains the author's pair together with the exact immutable Graph
// generation named by its VRS ID. The caller must keep a shared snapshot
// alive while using layer(), which exposes references into this owner.
// SWEGCA: src/swegca_vrs2/store.py@7536139:371-405
// SWEGCA: user@2026-09-22:13-21
class MainReadGeneration {
public:
    // SWEGCA: src/swegca_vrs2/store.py@7536139:371-405
    MainReadGeneration(
        std::shared_ptr<const FullCurrentMemoryVrsSnapshot> pair,
        std::shared_ptr<const ValidatedEventVrsInputs> inputs,
        std::shared_ptr<const GraphNodeDirectory> nodes,
        std::shared_ptr<const GraphRegionDirectory> regions,
        std::shared_ptr<const CoactivationAssociations> associations,
        PortalPolicy policy, std::vector<PortalRevocation> revocations,
        std::shared_ptr<const NativeJournalReadView> journal,
        std::shared_ptr<const ExactJournalDirectory> original_addresses,
        std::int64_t published_row_limit);

    // SWEGCA: src/swegca_vrs2/engine/mosaic_memory_activation.py@7536139:113-135
    [[nodiscard]] const FullCurrentMemoryVrsSnapshot& pair() const { return *pair_; }
    [[nodiscard]] PinnedReadLayer layer() const;

private:
    std::shared_ptr<const FullCurrentMemoryVrsSnapshot> pair_;
    std::shared_ptr<const ValidatedEventVrsInputs> inputs_;
    std::shared_ptr<const GraphNodeDirectory> nodes_;
    std::shared_ptr<const GraphRegionDirectory> regions_;
    std::shared_ptr<const CoactivationAssociations> associations_;
    PortalPolicy policy_;
    std::vector<PortalRevocation> revocations_;
    std::shared_ptr<const NativeJournalReadView> journal_;
    std::shared_ptr<const ExactJournalDirectory> original_addresses_;
    std::int64_t published_row_limit_;
};

}  // namespace swegca::vrs
