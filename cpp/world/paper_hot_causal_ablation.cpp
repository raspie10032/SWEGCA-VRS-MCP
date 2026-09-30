#include "world/paper_hot_causal_ablation.hpp"

#include "swegca_architecture/sha256.hpp"
#include "world/semantic_vrs_ingress.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <set>
#include <span>
#include <stdexcept>
#include <tuple>

namespace swegca::world {
namespace {

std::string hex(const architecture::DigestBytes& digest) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result(digest.size() * 2, '0');
    for (std::size_t i = 0; i < digest.size(); ++i) {
        const auto value = std::to_integer<unsigned>(digest[i]);
        result[2 * i] = digits[value >> 4U];
        result[2 * i + 1] = digits[value & 15U];
    }
    return result;
}

bool digest_id(const std::string_view value) {
    return value.size() == 64 && std::ranges::all_of(value, [](const unsigned char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

std::string digest(std::string_view value) {
    return hex(architecture::Sha256::of(std::as_bytes(std::span(value.data(), value.size()))));
}

bool text(const std::string_view value) {
    return std::ranges::any_of(value, [](const unsigned char c) { return !std::isspace(c); });
}

std::string_view role_name(const EpisodeRole role) noexcept {
    if (role == EpisodeRole::base) return "base";
    if (role == EpisodeRole::repair) return "repair";
    return "other";
}

std::uint64_t now_ns() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

template<class Function>
auto timed(Function&& function) {
    const auto started = now_ns();
    auto result = std::forward<Function>(function)();
    const auto ended = now_ns();
    return std::pair{std::move(result), StageInterval(started, ended)};
}

bool has_role(const std::vector<EpisodeRole>& roles, const EpisodeRole role) {
    return std::ranges::find(roles, role) != roles.end();
}

}  // namespace

std::string_view causal_arm_name(const CausalArm arm) noexcept {
    switch (arm) {
    case CausalArm::current_vrs: return "full_current_current_vrs";
    case CausalArm::frozen_vrs: return "full_current_frozen_vrs";
    case CausalArm::no_vrs: return "full_current_no_vrs";
    case CausalArm::base_promotion_only:
        return "full_current_current_vrs_base_promotion_only";
    case CausalArm::repair_promotion_only:
        return "full_current_current_vrs_repair_promotion_only";
    }
    return {};
}

VrsPromotionProjection::VrsPromotionProjection(
    std::string snapshot, std::map<std::string, double, std::less<>> strengths,
    const bool requires_io)
    : strengths_by_episode_id(std::move(strengths)),
      projection_sha256([&] {
          JsonValue::Object rows;
          for (const auto& [episode_id, strength] : strengths_by_episode_id)
              rows.emplace(episode_id, strength);
          return digest(semantic_canonical_json(JsonValue::Object{
              {"schema_version", "rozephine-hot-vrs-strength-projection-v1"},
              {"source_vrs_snapshot_id", snapshot},
              {"strengths_by_episode_id", std::move(rows)}}));
      }()), snapshot_id_(std::move(snapshot)), lookup_requires_io_(requires_io) {
    if (!digest_id(snapshot_id_)) throw std::invalid_argument("VRS snapshot must be a SHA-256 digest");
    for (const auto& [episode_id, strength_value] : strengths_by_episode_id)
        if (!text(episode_id) || strength_value < 0)
            throw std::invalid_argument("VRS projection contains invalid strength");
    if (lookup_requires_io_) throw std::invalid_argument("hot VRS projection cannot require I/O");
}

std::string_view VrsPromotionProjection::snapshot_id() const noexcept { return snapshot_id_; }
bool VrsPromotionProjection::lookup_requires_io() const noexcept { return lookup_requires_io_; }
double VrsPromotionProjection::strength(const std::string_view episode_id) const noexcept {
    const auto found = strengths_by_episode_id.find(episode_id);
    return found == strengths_by_episode_id.end() ? 0.0 : found->second;
}

EpisodeRoleIndex::EpisodeRoleIndex(
    std::map<std::string, EpisodeRole, std::less<>> overrides,
    const EpisodeRole default_role, const bool allowlist)
    : overrides_(std::move(overrides)), default_role_(default_role) {
    if (allowlist) throw std::invalid_argument("episode role metadata became a retrieval allowlist");
    for (const auto& [identifier, unused] : overrides_) {
        static_cast<void>(unused);
        if (!text(identifier)) throw std::invalid_argument("episode role index changed");
    }
}

EpisodeRole EpisodeRoleIndex::role(const std::string_view episode_id) const noexcept {
    const auto found = overrides_.find(episode_id);
    return found == overrides_.end() ? default_role_ : found->second;
}

StageInterval::StageInterval(const std::uint64_t started, const std::uint64_t ended)
    : started_monotonic_ns(started), ended_monotonic_ns(ended),
      elapsed_ns(ended >= started ? ended - started : 0) {
    if (ended < started) throw std::invalid_argument("stage clock moved backwards");
}

CausalArmResult::CausalArmResult(
    const CausalArm arm_value, std::optional<std::string> effective,
    std::vector<EpisodeRole> roles, MemoryActivationReceipt activation,
    std::string decision_value, const bool decisive_value, const bool invoked,
    StageInterval interval)
    : arm(arm_value), effective_vrs_snapshot_id(std::move(effective)),
      promotion_roles(std::move(roles)), memory_activation(std::move(activation)),
      decision(std::move(decision_value)), decisive(decisive_value),
      decision_rule_invoked(invoked), re_evidence_interval(interval) {
    if (!text(decision)) throw std::invalid_argument("causal decision is empty");
    if (memory_activation.re_evidence.should_abstain &&
        (decision != "abstain" || decisive || decision_rule_invoked))
        throw std::invalid_argument("Re-evidence abstention was bypassed");
}

CausalEvaluationReceipt::CausalEvaluationReceipt(
    std::string pair_id, std::string memory_id,
    std::map<std::string, StageInterval, std::less<>> intervals,
    std::vector<CausalArmResult> results, const std::size_t bootstraps,
    const std::size_t sequence, const std::size_t rebuilds)
    : pair_snapshot_id(std::move(pair_id)), memory_snapshot_id(std::move(memory_id)),
      common_stage_intervals(std::move(intervals)), arms(std::move(results)),
      cold_bootstrap_count(bootstraps), request_sequence(sequence),
      full_current_rebuilds_this_request(rebuilds) {
    if (common_stage_intervals.size() != 3 ||
        !common_stage_intervals.contains("deja_vu") ||
        !common_stage_intervals.contains("recall") ||
        !common_stage_intervals.contains("replay"))
        throw std::invalid_argument("common memory activation stage order changed");
    if (arms.size() != causal_arms.size())
        throw std::invalid_argument("causal arm order changed");
    for (std::size_t i = 0; i < arms.size(); ++i)
        if (arms[i].arm != causal_arms[i])
            throw std::invalid_argument("causal arm order changed");
    if (cold_bootstrap_count != 1 || full_current_rebuilds_this_request)
        throw std::invalid_argument("hot request rebuilt full-current cognition");
    std::optional<std::vector<std::string>> recalled;
    for (const auto& result : arms) {
        if (result.memory_activation.snapshot_id != memory_snapshot_id)
            throw std::invalid_argument("causal arms did not share one memory snapshot");
        std::vector<std::string> identifiers;
        for (const auto& candidate : result.memory_activation.recall.candidates)
            identifiers.push_back(candidate.episode_id);
        if (recalled && *recalled != identifiers)
            throw std::invalid_argument("diagnostic arm changed memory addressability");
        recalled = std::move(identifiers);
    }
}

HotCausalAblationEngine::HotCausalAblationEngine(
    FullCurrentMemoryVrsSnapshot pair_value,
    std::shared_ptr<const HotVrsStrengthIndex> current,
    std::shared_ptr<const HotVrsStrengthIndex> frozen,
    EpisodeRoleIndex roles)
    : HotCausalAblationEngine(std::move(pair_value), std::move(current),
          std::move(frozen), std::make_shared<const EpisodeRoleIndex>(std::move(roles))) {}

HotCausalAblationEngine::HotCausalAblationEngine(
    FullCurrentMemoryVrsSnapshot pair_value,
    std::shared_ptr<const HotVrsStrengthIndex> current,
    std::shared_ptr<const HotVrsStrengthIndex> frozen,
    std::shared_ptr<const HotEpisodeRoleIndex> roles)
    : pair_(std::move(pair_value)), current_vrs_(std::move(current)),
      frozen_vrs_(std::move(frozen)), episode_roles_(std::move(roles)) {
    if (!current_vrs_ || !frozen_vrs_ || !episode_roles_ || current_vrs_->lookup_requires_io() ||
        frozen_vrs_->lookup_requires_io())
        throw std::invalid_argument("VRS strength index is not hot");
    if (current_vrs_->snapshot_id() != pair_.vrs_snapshot_id)
        throw std::invalid_argument("current VRS projection is not bound to the full-current pair");
    if (frozen_vrs_->snapshot_id() == current_vrs_->snapshot_id())
        throw std::invalid_argument("frozen VRS diagnostic must use a distinct projection");
    if (pair_.memory->lookup_requires_io())
        throw std::invalid_argument("full-current memory is not hot");
}

CausalEvaluationReceipt HotCausalAblationEngine::evaluate(
    std::string query, const std::vector<std::string>& current_cues,
    const EvidenceJudge& assess_current_evidence,
    const CausalDecisionRule& decide) const {
    std::size_t sequence{};
    {
        std::lock_guard lock(counter_mutex_);
        sequence = ++request_count_;
    }
    auto [signal, deja] = timed([&] { return detect_deja_vu(*pair_.memory, query, current_cues); });
    auto [recalled, recall] = timed([&] { return recall_memory(*pair_.memory, signal); });
    auto [replayed, replay] = timed([&] { return replay_memory(*pair_.memory, recalled); });
    struct Definition final {
        CausalArm arm;
        std::shared_ptr<const HotVrsStrengthIndex> projection;
        std::vector<EpisodeRole> roles;
    };
    const std::vector<Definition> definitions{
        {CausalArm::current_vrs, current_vrs_, {EpisodeRole::base, EpisodeRole::repair, EpisodeRole::other}},
        {CausalArm::frozen_vrs, frozen_vrs_, {EpisodeRole::base, EpisodeRole::repair, EpisodeRole::other}},
        {CausalArm::no_vrs, {}, {}},
        {CausalArm::base_promotion_only, current_vrs_, {EpisodeRole::base}},
        {CausalArm::repair_promotion_only, current_vrs_, {EpisodeRole::repair}}};
    std::vector<CausalArmResult> results;
    for (const auto& definition : definitions) {
        auto judge = [&](const ReplayedEpisode& episode) {
            const auto strength = !definition.projection ||
                !has_role(definition.roles, episode_roles_->role(episode.episode_id))
                    ? 0.0 : definition.projection->strength(episode.episode_id);
            if (strength < vrs_promotion_threshold)
                return CurrentEvidenceVerdict(episode.episode_id,
                    "unpromoted:" + episode.episode_id, "insufficient",
                    "episode was recalled and replayed but the effective VRS projection did not promote it", {});
            return assess_current_evidence(episode);
        };
        auto [evidenced, interval] = timed([&] { return re_evidence_memory(replayed, judge); });
        MemoryActivationReceipt activation("rozephine-memory-activation-v1",
            std::string(pair_.memory->snapshot_id()), signal, recalled, replayed,
            std::move(evidenced));
        const bool abstain = activation.re_evidence.should_abstain;
        std::string decision = abstain ? "abstain" : decide(*pair_.memory, activation);
        if (!abstain && !text(decision))
            throw std::invalid_argument("decision rule returned an empty proposal");
        results.emplace_back(definition.arm,
            definition.projection ? std::optional<std::string>(
                                        std::string(definition.projection->snapshot_id()))
                                  : std::nullopt,
            definition.roles, std::move(activation), std::move(decision),
            !abstain, !abstain, interval);
    }
    return {pair_.snapshot_id, std::string(pair_.memory->snapshot_id()),
        {{"deja_vu", deja}, {"recall", recall}, {"replay", replay}},
        std::move(results), 1, sequence};
}

const FullCurrentMemoryVrsSnapshot& HotCausalAblationEngine::pair() const noexcept { return pair_; }
std::size_t HotCausalAblationEngine::cold_bootstrap_count() const noexcept { return 1; }
std::size_t HotCausalAblationEngine::request_count() const {
    std::lock_guard lock(counter_mutex_); return request_count_;
}

CompletedUnitCache::CompletedUnitCache(std::map<std::string, Outcome, std::less<>> completed)
    : completed_(std::move(completed)) {}

std::optional<CompletedUnitCache::Outcome> CompletedUnitCache::outcome(
    const std::string_view unit_id) const {
    std::lock_guard lock(mutex_);
    const auto found = completed_.find(unit_id);
    return found == completed_.end() ? std::nullopt : std::optional(found->second);
}

CompletedUnitCache::Outcome CompletedUnitCache::store(std::string unit_id, Outcome outcome_value) {
    if (!text(unit_id) || outcome_value.empty())
        throw std::invalid_argument("completed outcome checkpoint is incomplete");
    std::lock_guard lock(mutex_);
    const auto [found, inserted] = completed_.try_emplace(std::move(unit_id), outcome_value);
    if (!inserted && found->second != outcome_value)
        throw std::invalid_argument("completed checkpoint changed");
    return found->second;
}

VisualOutcomeReceipt::VisualOutcomeReceipt(
    std::string id, JsonValue::Object result, const bool hit,
    const std::size_t visual_calls, const std::size_t asr_calls,
    const std::size_t ocr_calls)
    : unit_id(std::move(id)), outcome(std::move(result)), checkpoint_hit(hit),
      visual_worker_calls(visual_calls), asr_worker_calls(asr_calls),
      ocr_worker_calls(ocr_calls) {
    if (!text(unit_id) || outcome.empty() || asr_worker_calls || ocr_worker_calls ||
        visual_worker_calls != (checkpoint_hit ? 0U : 1U))
        throw std::invalid_argument("visual-only outcome contract changed");
}

VisualOutcomeReceipt acquire_visual_outcome_once(
    std::string unit_id, const JsonValue& visual_input,
    CompletedUnitCache& checkpoint, const VisualWorker& visual_worker) {
    if (auto existing = checkpoint.outcome(unit_id))
        return {std::move(unit_id), std::move(*existing), true, 0};
    auto proposed = visual_worker(visual_input);
    if (proposed.empty()) throw std::invalid_argument("visual outcome worker returned an invalid proposal");
    auto stored = checkpoint.store(unit_id, std::move(proposed));
    return {std::move(unit_id), std::move(stored), false, 1};
}

}  // namespace swegca::world
