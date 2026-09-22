#pragma once

#include "memory_receipt.hpp"
#include "portal_navigation.hpp"
#include "session_first_read.hpp"

namespace swegca::vrs {

// navigation.recall is the full memory_selection address set. The activation
// receipt contains only originals actually opened by Replay and Re-evidence.
// SWEGCA: src/swegca_vrs2/store.py@c06092a:1717-1780
struct FullFourStageRead {
    PortalNavigationRecall navigation;
    MemoryActivationReceipt activation;

    // SWEGCA: src/swegca_vrs2/store.py@c06092a:1766-1780
    [[nodiscard]] const RecallResult& memory_selection() const {
        return navigation.recall;
    }
};

// The full Recall is already complete. Open one chosen original, judge the
// current explicit proposition, then open only its relevant opposing originals
// if that first Re-evidence found a conflict. No history result grants truth.
// SWEGCA: src/swegca_vrs2/store.py@c06092a:1719-1768
[[nodiscard]] FullFourStageRead finish_selected_four_stage_read(
    PortalNavigationRecall navigation, const PinnedReadLayer& layer);

}  // namespace swegca::vrs
