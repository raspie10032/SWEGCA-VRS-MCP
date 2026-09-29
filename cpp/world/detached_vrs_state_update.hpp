#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace swegca::world {

inline constexpr std::string_view detached_vrs_update_source_sha256 =
    "e43aafe08e0a3b6e0ba6b8fa10150768d563645f5767749836552d1d8ecef39a";
inline constexpr double legacy_vrs_stable_reinforcement_factor = 1.01;
inline constexpr double legacy_vrs_unstable_weakening_factor = 0.995;

enum class CurrentEvidenceVerdict : std::uint8_t {
    support,
    refute,
    insufficient,
    conflict,
};

struct VrsReplayObservation final {
    std::optional<std::int64_t> edge_id;
    std::optional<double> vrs_strength;
    std::optional<std::int64_t> canonical_group_id;
    std::optional<double> deweighted_vrs_strength;
};

struct VrsReplayedEpisode final {
    std::string episode_id;
    std::vector<VrsReplayObservation> observations;
};

struct VrsReEvidenceJudgment final {
    std::string episode_id;
    std::string proposition;
    CurrentEvidenceVerdict verdict{CurrentEvidenceVerdict::insufficient};
};

class VrsExperiencePromotionDecision final {
public:
    VrsExperiencePromotionDecision(
        std::string snapshot_id, std::string connection_id,
        double previous_strength, double current_strength, std::string action,
        bool promoted, bool semantic_evidence_allowed,
        bool underlying_experience_preserved = true,
        bool action_authorized = false,
        bool persistent_write_authorized = false);

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

    friend bool operator==(const VrsExperiencePromotionDecision&,
                           const VrsExperiencePromotionDecision&) = default;
};

struct VrsSourceJudgment final {
    std::string episode_id;
    std::string proposition;
    CurrentEvidenceVerdict verdict{CurrentEvidenceVerdict::insufficient};
    friend bool operator==(const VrsSourceJudgment&, const VrsSourceJudgment&) = default;
};

class VrsConnectionStateUpdate final {
public:
    VrsConnectionStateUpdate(
        std::string episode_id, std::string connection_id,
        std::string proposition, CurrentEvidenceVerdict verdict,
        double previous_strength, double current_strength,
        std::string update_action, VrsExperiencePromotionDecision promotion,
        bool underlying_experience_preserved = true,
        std::vector<VrsSourceJudgment> source_judgments = {});

    const std::string episode_id;
    const std::string connection_id;
    const std::string proposition;
    const CurrentEvidenceVerdict verdict;
    const double previous_strength;
    const double current_strength;
    const std::string update_action;
    const VrsExperiencePromotionDecision promotion;
    const bool underlying_experience_preserved;
    const std::vector<VrsSourceJudgment> source_judgments;
};

class DetachedVrsStateUpdateReceipt final {
public:
    DetachedVrsStateUpdateReceipt(
        std::string snapshot_id, std::vector<VrsConnectionStateUpdate> updates,
        std::array<std::string, 5> stage_order = {
            "deja_vu", "recall", "replay", "re_evidence", "vrs_state_update"},
        bool detached_state_only = true,
        bool persistent_state_mutated = false, bool action_authorized = false,
        bool persistent_write_authorized = false,
        bool semantic_promotion_authorized = false);

    const std::string snapshot_id;
    const std::vector<VrsConnectionStateUpdate> updates;
    const std::array<std::string, 5> stage_order;
    const bool detached_state_only;
    const bool persistent_state_mutated;
    const bool action_authorized;
    const bool persistent_write_authorized;
    const bool semantic_promotion_authorized;
};

[[nodiscard]] VrsExperiencePromotionDecision assess_vrs_experience_promotion(
    std::string snapshot_id, std::string connection_id,
    double previous_strength, double current_strength);

// Compatibility port of the pinned detached floating-strength generation.
// The verdict is an input produced by Re-evidence. This function cannot judge
// evidence, mutate persistent VRS state, or supersede integer ternary counts.
[[nodiscard]] DetachedVrsStateUpdateReceipt plan_detached_vrs_state_update(
    std::string snapshot_id,
    const std::vector<VrsReplayedEpisode>& replayed,
    const std::vector<VrsReEvidenceJudgment>& judgments,
    const std::vector<std::string>& conflicting_propositions);

}  // namespace swegca::world
