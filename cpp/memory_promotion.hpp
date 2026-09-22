#pragma once

#include <string>

namespace swegca::vrs {

// A threshold result is detached semantic-evidence eligibility. It does not
// authorize an action, durable write, or deletion of the underlying original.
// SWEGCA: src/swegca_vrs2/engine/mosaic_memory_promotion.py@7536139:7-45
struct VRSExperiencePromotionDecision {
    const std::string snapshot_id;
    const std::string connection_id;
    const double previous_strength;
    const double current_strength;
    const std::string action;
    const bool promoted;
    const bool semantic_evidence_allowed;
    const bool underlying_experience_preserved;
    const bool action_authorized;
    const bool persistent_write_authorized;

    // SWEGCA: src/swegca_vrs2/engine/mosaic_memory_promotion.py@7536139:9-45
    VRSExperiencePromotionDecision(
        std::string snapshot_id, std::string connection_id,
        double previous_strength, double current_strength, std::string action,
        bool promoted, bool semantic_evidence_allowed,
        bool underlying_experience_preserved = true,
        bool action_authorized = false, bool persistent_write_authorized = false);
};

// SWEGCA: src/swegca_vrs2/engine/mosaic_memory_promotion.py@7536139:48-83
[[nodiscard]] VRSExperiencePromotionDecision assess_vrs_experience_promotion(
    std::string snapshot_id, std::string connection_id,
    double previous_strength, double current_strength);

}  // namespace swegca::vrs
