#pragma once

#include "world/memory_activation.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace swegca::world {

inline constexpr std::string_view paper_hot_causal_ablation_source_sha256 =
    "30cbbb8f09052a14235625164003ab3dfa929cb898ffed45b25a7227bf559b4c";
inline constexpr double vrs_promotion_threshold = 1.0;

enum class EpisodeRole : unsigned char { base, repair, other };
enum class CausalArm : unsigned char {
    current_vrs, frozen_vrs, no_vrs, base_promotion_only, repair_promotion_only
};
inline constexpr std::array<CausalArm, 5> causal_arms{
    CausalArm::current_vrs, CausalArm::frozen_vrs, CausalArm::no_vrs,
    CausalArm::base_promotion_only, CausalArm::repair_promotion_only};

[[nodiscard]] std::string_view causal_arm_name(CausalArm arm) noexcept;

class HotVrsStrengthIndex {
public:
    virtual ~HotVrsStrengthIndex() = default;
    [[nodiscard]] virtual std::string_view snapshot_id() const noexcept = 0;
    [[nodiscard]] virtual bool lookup_requires_io() const noexcept { return false; }
    [[nodiscard]] virtual double strength(std::string_view episode_id) const noexcept = 0;
};

class VrsPromotionProjection final : public HotVrsStrengthIndex {
public:
    VrsPromotionProjection(std::string snapshot_id,
        std::map<std::string, double, std::less<>> strengths_by_episode_id,
        bool lookup_requires_io = false);
    [[nodiscard]] std::string_view snapshot_id() const noexcept override;
    [[nodiscard]] bool lookup_requires_io() const noexcept override;
    [[nodiscard]] double strength(std::string_view episode_id) const noexcept override;

    const std::map<std::string, double, std::less<>> strengths_by_episode_id;
    const std::string projection_sha256;
private:
    std::string snapshot_id_;
    bool lookup_requires_io_{};
};

class HotEpisodeRoleIndex {
public:
    virtual ~HotEpisodeRoleIndex() = default;
    [[nodiscard]] virtual EpisodeRole role(std::string_view episode_id) const = 0;
};

class EpisodeRoleIndex final : public HotEpisodeRoleIndex {
public:
    EpisodeRoleIndex(std::map<std::string, EpisodeRole, std::less<>> overrides,
                     EpisodeRole default_role = EpisodeRole::base,
                     bool codex_or_evaluator_allowlist_used = false);
    [[nodiscard]] EpisodeRole role(std::string_view episode_id) const noexcept override;
private:
    std::map<std::string, EpisodeRole, std::less<>> overrides_;
    EpisodeRole default_role_{};
};

struct StageInterval final {
    std::uint64_t started_monotonic_ns{};
    std::uint64_t ended_monotonic_ns{};
    std::uint64_t elapsed_ns{};
    StageInterval(std::uint64_t started, std::uint64_t ended);
};

struct CausalArmResult final {
    CausalArm arm{};
    std::optional<std::string> effective_vrs_snapshot_id;
    std::vector<EpisodeRole> promotion_roles;
    MemoryActivationReceipt memory_activation;
    std::string decision;
    bool decisive{};
    bool decision_rule_invoked{};
    StageInterval re_evidence_interval;

    CausalArmResult(CausalArm arm,
        std::optional<std::string> effective_vrs_snapshot_id,
        std::vector<EpisodeRole> promotion_roles,
        MemoryActivationReceipt memory_activation, std::string decision,
        bool decisive, bool decision_rule_invoked,
        StageInterval re_evidence_interval);
};

struct CausalEvaluationReceipt final {
    std::string pair_snapshot_id;
    std::string memory_snapshot_id;
    std::map<std::string, StageInterval, std::less<>> common_stage_intervals;
    std::vector<CausalArmResult> arms;
    std::size_t cold_bootstrap_count{};
    std::size_t request_sequence{};
    std::size_t full_current_rebuilds_this_request{};

    CausalEvaluationReceipt(std::string pair_snapshot_id,
        std::string memory_snapshot_id,
        std::map<std::string, StageInterval, std::less<>> common_stage_intervals,
        std::vector<CausalArmResult> arms, std::size_t cold_bootstrap_count,
        std::size_t request_sequence,
        std::size_t full_current_rebuilds_this_request = 0);
};

using CausalDecisionRule = std::function<std::string(
    const HotMemoryIndex&, const MemoryActivationReceipt&)>;

class HotCausalAblationEngine final {
public:
    HotCausalAblationEngine(FullCurrentMemoryVrsSnapshot pair,
        std::shared_ptr<const HotVrsStrengthIndex> current_vrs,
        std::shared_ptr<const HotVrsStrengthIndex> frozen_vrs,
        EpisodeRoleIndex episode_roles);
    HotCausalAblationEngine(FullCurrentMemoryVrsSnapshot pair,
        std::shared_ptr<const HotVrsStrengthIndex> current_vrs,
        std::shared_ptr<const HotVrsStrengthIndex> frozen_vrs,
        std::shared_ptr<const HotEpisodeRoleIndex> episode_roles);
    [[nodiscard]] CausalEvaluationReceipt evaluate(std::string query,
        const std::vector<std::string>& current_cues,
        const EvidenceJudge& assess_current_evidence,
        const CausalDecisionRule& decide) const;
    [[nodiscard]] const FullCurrentMemoryVrsSnapshot& pair() const noexcept;
    [[nodiscard]] std::size_t cold_bootstrap_count() const noexcept;
    [[nodiscard]] std::size_t request_count() const;
private:
    FullCurrentMemoryVrsSnapshot pair_;
    std::shared_ptr<const HotVrsStrengthIndex> current_vrs_;
    std::shared_ptr<const HotVrsStrengthIndex> frozen_vrs_;
    std::shared_ptr<const HotEpisodeRoleIndex> episode_roles_;
    mutable std::mutex counter_mutex_;
    mutable std::size_t request_count_{};
};

class CompletedUnitCache final {
public:
    using Outcome = JsonValue::Object;
    explicit CompletedUnitCache(std::map<std::string, Outcome, std::less<>> completed = {});
    [[nodiscard]] std::optional<Outcome> outcome(std::string_view unit_id) const;
    [[nodiscard]] Outcome store(std::string unit_id, Outcome outcome);
private:
    mutable std::mutex mutex_;
    std::map<std::string, Outcome, std::less<>> completed_;
};

struct VisualOutcomeReceipt final {
    std::string unit_id;
    JsonValue::Object outcome;
    bool checkpoint_hit{};
    std::size_t visual_worker_calls{};
    std::size_t asr_worker_calls{};
    std::size_t ocr_worker_calls{};
    VisualOutcomeReceipt(std::string unit_id, JsonValue::Object outcome,
        bool checkpoint_hit, std::size_t visual_worker_calls,
        std::size_t asr_worker_calls = 0, std::size_t ocr_worker_calls = 0);
};

using VisualWorker = std::function<JsonValue::Object(const JsonValue&)>;
[[nodiscard]] VisualOutcomeReceipt acquire_visual_outcome_once(
    std::string unit_id, const JsonValue& visual_input,
    CompletedUnitCache& checkpoint, const VisualWorker& visual_worker);

}  // namespace swegca::world
