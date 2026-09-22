#pragma once

#include "memory_promotion.hpp"
#include "memory_receipt.hpp"

#include <array>
#include <string>
#include <tuple>
#include <vector>

namespace swegca::vrs {

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_state_update.py@7536139:10-34
struct VRSConnectionStateUpdate {
    const std::string episode_id;
    const std::string connection_id;
    const std::string proposition;
    const std::string verdict;
    const double previous_strength;
    const double current_strength;
    const std::string update_action;
    const VRSExperiencePromotionDecision promotion;
    const bool underlying_experience_preserved;
    const std::vector<std::tuple<std::string, std::string, std::string>> source_judgments;

    // SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_state_update.py@7536139:11-34
    VRSConnectionStateUpdate(
        std::string episode_id, std::string connection_id, std::string proposition,
        std::string verdict, double previous_strength, double current_strength,
        std::string update_action, VRSExperiencePromotionDecision promotion,
        bool underlying_experience_preserved = true,
        std::vector<std::tuple<std::string, std::string, std::string>> source_judgments = {});
};

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_state_update.py@7536139:37-72
struct VRSStateUpdateReceipt {
    const std::string snapshot_id;
    const std::vector<VRSConnectionStateUpdate> updates;
    const std::array<std::string, 5> stage_order;
    const bool detached_state_only;
    const bool persistent_state_mutated;
    const bool action_authorized;
    const bool persistent_write_authorized;
    const bool semantic_promotion_authorized;

    // SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_state_update.py@7536139:38-72
    VRSStateUpdateReceipt(
        std::string snapshot_id, std::vector<VRSConnectionStateUpdate> updates,
        std::array<std::string, 5> stage_order = {
            "deja_vu", "recall", "replay", "re_evidence", "vrs_state_update"},
        bool detached_state_only = true, bool persistent_state_mutated = false,
        bool action_authorized = false, bool persistent_write_authorized = false,
        bool semantic_promotion_authorized = false);
};

// A plan changes a detached strength view. Main owns any later publication.
// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_state_update.py@7536139:75-152
[[nodiscard]] VRSStateUpdateReceipt plan_vrs_state_update(
    const MemoryActivationReceipt& receipt);

}  // namespace swegca::vrs
