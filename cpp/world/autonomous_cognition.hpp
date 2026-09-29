#pragma once

#include "world/cognitive_state.hpp"
#include "world/evidence_accumulator.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace swegca::world {

enum class AutonomyPhase : std::uint8_t {
    observe,
    hypothesize,
    request_evidence,
    collect_evidence,
    observe_evidence_result,
    verify,
    remember,
    act,
    observe_result,
    abstain,
};

enum class AutonomyEventKind : std::uint8_t {
    observation,
    hypothesis,
    evidence_request,
    evidence_action,
    evidence_result,
    verification,
    memory_commit,
    action_proposal,
    action_result,
    hypothesis_abandon,
};

[[nodiscard]] std::string_view autonomy_phase_name(AutonomyPhase phase) noexcept;
[[nodiscard]] AutonomyPhase parse_autonomy_phase(std::string_view value);

class AutonomousCognitionConfig final {
public:
    AutonomousCognitionConfig(
        std::vector<std::string> allowed_tool_actions,
        std::vector<std::string> reversible_tool_actions,
        double minimum_action_confidence = 0.8,
        std::uint64_t maximum_action_failures = 2,
        std::vector<std::string> allowed_evidence_tool_actions = {},
        std::vector<std::string> reversible_evidence_tool_actions = {},
        double minimum_evidence_action_confidence = 0.8,
        std::uint64_t maximum_evidence_action_failures = 2);

    const std::vector<std::string> allowed_tool_actions;
    const std::vector<std::string> reversible_tool_actions;
    const double minimum_action_confidence;
    const std::uint64_t maximum_action_failures;
    const std::vector<std::string> allowed_evidence_tool_actions;
    const std::vector<std::string> reversible_evidence_tool_actions;
    const double minimum_evidence_action_confidence;
    const std::uint64_t maximum_evidence_action_failures;
};

class AutonomyEvent final {
public:
    AutonomyEvent(std::string event_id, AutonomyEventKind kind,
                  std::optional<std::string> hypothesis_id,
                  std::vector<std::string> evidence_refs,
                  std::string source_family, std::string context_hash,
                  double confidence, JsonValue::Object payload = {});

    const std::string event_id;
    const AutonomyEventKind kind;
    const std::optional<std::string> hypothesis_id;
    const std::vector<std::string> evidence_refs;
    const std::string source_family;
    const std::string context_hash;
    const double confidence;
    const JsonValue::Object payload;
};

struct AutonomyTransition final {
    CognitiveState state;
    AutonomyPhase prior_phase;
    AutonomyPhase next_phase;
    bool accepted;
    std::string reason;
    bool memory_write_allowed{false};
    bool tool_action_allowed{false};
    std::optional<std::string> tool_action;
    bool evidence_tool_action_allowed{false};
    std::optional<std::string> evidence_tool_action;
};

[[nodiscard]] AutonomyTransition advance_autonomous_cognition(
    const CognitiveState& state, const AutonomyEvent& event,
    const AutonomousCognitionConfig& config,
    const AccumulatorDecision* evidence_decision = nullptr);

[[nodiscard]] constexpr std::string_view autonomous_cognition_source_sha256() noexcept {
    return "fc8eba5669003ffae65c8b724b0f0e50d90f8bba59a18cddd9147ed2ba24b95f";
}

}  // namespace swegca::world
