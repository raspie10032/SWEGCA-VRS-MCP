#include "memory_promotion.hpp"

#include "unicode.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace swegca::vrs {
namespace {

// SWEGCA: src/swegca_vrs2/engine/mosaic_memory_promotion.py@7536139:21-22
bool nonblank(std::string_view value) {
    const auto points = decode_utf8(value);
    return std::any_of(points.begin(), points.end(),
                       [](auto point) { return !python_space(point); });
}

// SWEGCA: src/swegca_vrs2/engine/mosaic_memory_promotion.py@7536139:4-4
constexpr double verified_experience_promotion_strength = 1.0;

// SWEGCA: src/swegca_vrs2/engine/mosaic_memory_promotion.py@7536139:32-40
std::string promotion_action(bool was_promoted, bool is_promoted) {
    if (was_promoted && is_promoted) return "retain";
    if (was_promoted) return "revoke";
    return is_promoted ? "promote" : "remain_unpromoted";
}

}  // namespace

// SWEGCA: src/swegca_vrs2/engine/mosaic_memory_promotion.py@7536139:7-52
VRSExperiencePromotionDecision::VRSExperiencePromotionDecision(
    std::string new_snapshot_id, std::string new_connection_id,
    double new_previous_strength, double new_current_strength,
    std::string new_action, bool new_promoted, bool new_semantic_evidence_allowed,
    bool new_underlying_experience_preserved, bool new_action_authorized,
    bool new_persistent_write_authorized)
    // SWEGCA: src/swegca_vrs2/engine/mosaic_memory_promotion.py@7536139:7-52
    : snapshot_id(std::move(new_snapshot_id)),
      connection_id(std::move(new_connection_id)),
      previous_strength(new_previous_strength), current_strength(new_current_strength),
      action(std::move(new_action)), promoted(new_promoted),
      semantic_evidence_allowed(new_semantic_evidence_allowed),
      underlying_experience_preserved(new_underlying_experience_preserved),
      action_authorized(new_action_authorized),
      persistent_write_authorized(new_persistent_write_authorized) {
    if (!nonblank(snapshot_id) || !nonblank(connection_id))
        throw std::runtime_error("VRS promotion requires snapshot and connection identities");
    if (!std::isfinite(previous_strength) || previous_strength < 0 ||
        !std::isfinite(current_strength) || current_strength < 0)
        throw std::runtime_error("VRS promotion strengths must be finite and nonnegative");
    const bool was_promoted = previous_strength >= verified_experience_promotion_strength;
    const bool is_promoted = current_strength >= verified_experience_promotion_strength;
    if (action != promotion_action(was_promoted, is_promoted) ||
        promoted != is_promoted || semantic_evidence_allowed != is_promoted)
        throw std::runtime_error("VRS promotion decision changed");
    if (!underlying_experience_preserved || action_authorized || persistent_write_authorized)
        throw std::runtime_error("VRS promotion changed its authority boundary");
}

// SWEGCA: src/swegca_vrs2/engine/mosaic_memory_promotion.py@7536139:55-83
VRSExperiencePromotionDecision assess_vrs_experience_promotion(
    std::string snapshot_id, std::string connection_id,
    double previous_strength, double current_strength) {
    return VRSExperiencePromotionDecision(
        std::move(snapshot_id), std::move(connection_id),
        previous_strength, current_strength,
        promotion_action(previous_strength >= verified_experience_promotion_strength,
                         current_strength >= verified_experience_promotion_strength),
        current_strength >= verified_experience_promotion_strength,
        current_strength >= verified_experience_promotion_strength);
}

}  // namespace swegca::vrs
