#include "world/accelerated_verification.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <variant>

namespace swegca::world {
namespace {

using Clock = std::chrono::steady_clock;

std::uint64_t elapsed_ns(const Clock::time_point started) {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - started).count());
}

const JsonValue* optional(const JsonValue::Object& object, const std::string_view key) {
    const auto found = object.find(key);
    return found == object.end() ? nullptr : &found->second;
}

bool revision_matches(const JsonValue* value, const std::size_t revision) {
    if (value == nullptr) return false;
    if (const auto* integer = std::get_if<std::int64_t>(&value->storage())) {
        return *integer >= 0 && static_cast<std::uint64_t>(*integer) == revision;
    }
    if (const auto* number = std::get_if<double>(&value->storage())) {
        return std::isfinite(*number) && *number >= 0.0 &&
               *number <= static_cast<double>(std::numeric_limits<std::size_t>::max()) &&
               std::trunc(*number) == *number && static_cast<std::size_t>(*number) == revision;
    }
    return false;
}

}  // namespace

SelectedReplayDecision select_replay_decision(const ReplayBatchResult& result,
                                              std::string intervention) {
    if (intervention.empty()) {
        throw std::invalid_argument("intervention must be nonempty");
    }
    const auto found = std::find_if(
        result.decisions.begin(), result.decisions.end(),
        [&](const ReplayDecision& item) { return item.intervention == intervention; });
    if (found == result.decisions.end()) {
        throw std::invalid_argument("selected intervention is not in replay result");
    }
    const auto started = Clock::now();
    const auto index = static_cast<std::size_t>(found - result.decisions.begin());
    AccumulatorDecision decision(
        found->status, found->reason, found->posterior_mean,
        found->causal_lower_bound, found->overall_upper_bound,
        found->effective_sample_size, found->source_diversity,
        found->context_diversity, found->regime_change_score, result.revision);
    return {std::move(decision), std::move(intervention), index, elapsed_ns(started)};
}

AcceleratedVerificationReceipt advance_accelerated_verification(
    const CognitiveState& state, const AutonomyEvent& event,
    const AutonomousCognitionConfig& config,
    const ReplayBatchResult& result, const std::string_view intervention) {
    if (event.kind != AutonomyEventKind::verification) {
        throw std::invalid_argument("accelerated adapter requires a verification event");
    }
    const std::string cache_ref = "replay-cache:" + result.cache_hash;
    if (std::find(event.evidence_refs.begin(), event.evidence_refs.end(), cache_ref) ==
        event.evidence_refs.end()) {
        throw std::invalid_argument("verification event is not bound to the replay cache");
    }
    if (!revision_matches(optional(event.payload, "replay_revision"), result.revision)) {
        throw std::invalid_argument("verification event replay revision is stale");
    }
    const auto* selected_intervention = optional(event.payload, "replay_intervention");
    if (selected_intervention == nullptr ||
        !std::holds_alternative<std::string>(selected_intervention->storage()) ||
        selected_intervention->as_string() != intervention) {
        throw std::invalid_argument("verification event intervention differs from selection");
    }

    auto selected = select_replay_decision(result, std::string(intervention));
    const auto started = Clock::now();
    auto transition = advance_autonomous_cognition(
        state, event, config, &selected.decision);
    const auto transition_ns = elapsed_ns(started);
    const bool reused =
        state.semantic_slots().storage_identity() ==
            transition.state.semantic_slots().storage_identity() &&
        state.executive_slots().storage_identity() ==
            transition.state.executive_slots().storage_identity() &&
        state.scratch_slots().storage_identity() ==
            transition.state.scratch_slots().storage_identity();
    if (transition.state.persistent_state_count() != 1) {
        throw std::runtime_error("accelerated verification created another cognitive state");
    }
    if (!reused) {
        throw std::runtime_error("accelerated verification replaced World tensors");
    }
    return {std::move(transition), std::move(selected), result.cache_hash,
            transition_ns, reused};
}

}  // namespace swegca::world
