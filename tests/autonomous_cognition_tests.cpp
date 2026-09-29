#include "world/autonomous_cognition.hpp"

#include <cstdlib>
#include <functional>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace {

using namespace swegca::world;

void check(const bool condition, const char* expression, const int line) {
    if (!condition) {
        std::cerr << "CHECK failed at line " << line << ": " << expression << '\n';
        std::exit(1);
    }
}
#define CHECK(expression) check(static_cast<bool>(expression), #expression, __LINE__)

void rejects(const std::function<void()>& operation, const std::string_view expected) {
    try {
        operation();
    } catch (const std::exception& error) {
        CHECK(std::string_view(error.what()).find(expected) != std::string_view::npos);
        return;
    }
    CHECK(false);
}

Tensor tensor(const std::uint64_t slots) {
    return Tensor(TensorDType::float32, {1, slots, 4},
                  std::vector<double>(static_cast<std::size_t>(slots * 4)));
}

CognitiveState state(JsonValue::Object goal = {}, JsonValue::Object self = {}) {
    return CognitiveState(tensor(2), tensor(1), tensor(1), {}, {}, std::move(goal), {},
                          std::move(self));
}

AutonomyEvent event(std::string id, const AutonomyEventKind kind,
                    std::optional<std::string> hypothesis = std::string("가설-1"),
                    JsonValue::Object payload = {}, const double confidence = 1.0,
                    std::vector<std::string> evidence = {}) {
    if (evidence.empty()) evidence.push_back("증거:" + id);
    return AutonomyEvent(std::move(id), kind, std::move(hypothesis), std::move(evidence),
                         "한국어-개발", "문맥", confidence, std::move(payload));
}

AccumulatorDecision decision(std::string status) {
    return AccumulatorDecision(std::move(status), "test", 0.95, 0.8, 0.99, 16.0,
                               2, 4, 0.0, 16);
}

AutonomousCognitionConfig config() {
    return AutonomousCognitionConfig({"read_file", "search_memory"},
                                     {"read_file", "search_memory"});
}

AutonomousCognitionConfig evidence_config() {
    return AutonomousCognitionConfig({"read_file"}, {"read_file"}, 0.8, 2,
                                     {"inspect_frame"}, {"inspect_frame"}, 0.8, 2);
}

void check_same_world(const CognitiveState& before, const CognitiveState& after) {
    CHECK(before.semantic_slots().storage_identity() ==
          after.semantic_slots().storage_identity());
    CHECK(before.executive_slots().storage_identity() ==
          after.executive_slots().storage_identity());
    CHECK(before.scratch_slots().storage_identity() ==
          after.scratch_slots().storage_identity());
    CHECK(after.persistent_state_count() == 1);
}

void test_verified_loop_and_world_reuse() {
    const auto original = state();
    auto current = original;
    for (const auto& item : std::vector<AutonomyEvent>{
             event("관찰", AutonomyEventKind::observation, std::nullopt),
             event("가설", AutonomyEventKind::hypothesis),
             event("요청", AutonomyEventKind::evidence_request, std::string("가설-1"),
                   {{"requested_axes", JsonValue::Array{"observational", "counterfactual"}}})}) {
        auto transition = advance_autonomous_cognition(current, item, config());
        CHECK(transition.accepted);
        check_same_world(original, transition.state);
        current = std::move(transition.state);
    }
    const auto accepted = decision("accept");
    auto verified = advance_autonomous_cognition(
        current, event("검증", AutonomyEventKind::verification), config(), &accepted);
    CHECK(verified.next_phase == AutonomyPhase::remember);
    CHECK(verified.memory_write_allowed);
    auto remembered = advance_autonomous_cognition(
        verified.state,
        event("기억", AutonomyEventKind::memory_commit, std::string("가설-1"),
              {{"memory_ref", "memory:기억-1"}, {"content_hash", "sha256:abc"}}),
        config());
    auto action = advance_autonomous_cognition(
        remembered.state,
        event("행동", AutonomyEventKind::action_proposal, std::string("가설-1"),
              {{"action", "read_file"}}),
        config());
    CHECK(action.tool_action_allowed && action.tool_action == "read_file");
    auto result = advance_autonomous_cognition(
        action.state,
        event("결과", AutonomyEventKind::action_result, std::string("가설-1"),
              {{"success", true}}),
        config());
    CHECK(result.next_phase == AutonomyPhase::observe);
    check_same_world(original, result.state);
    CHECK(result.state.goal_state().at("autonomy_step").as_number() == 7.0);
}

void test_abstain_reject_duplicate_and_abandon() {
    auto current = state();
    for (const auto& item : std::vector<AutonomyEvent>{
             event("o", AutonomyEventKind::observation, std::nullopt),
             event("h", AutonomyEventKind::hypothesis),
             event("q", AutonomyEventKind::evidence_request, std::string("가설-1"),
                   {{"requested_axes", JsonValue::Array{"cross_context"}}})}) {
        current = advance_autonomous_cognition(current, item, config()).state;
    }
    const auto uncertain = decision("abstain");
    auto abstained = advance_autonomous_cognition(
        current, event("v", AutonomyEventKind::verification), config(), &uncertain);
    CHECK(abstained.next_phase == AutonomyPhase::request_evidence);
    CHECK(abstained.reason == "additional_evidence_required");
    CHECK(!abstained.memory_write_allowed && !abstained.tool_action_allowed);
    auto abandoned = advance_autonomous_cognition(
        abstained.state, event("drop", AutonomyEventKind::hypothesis_abandon), config());
    CHECK(abandoned.accepted && abandoned.next_phase == AutonomyPhase::abstain);
    CHECK(abandoned.reason == "hypothesis_abandoned");

    auto reject_state = state({{"autonomy_phase", "verify"},
                               {"active_hypothesis_id", "가설-1"}});
    const auto rejected_decision = decision("reject");
    auto rejected = advance_autonomous_cognition(
        reject_state, event("reject", AutonomyEventKind::verification), config(),
        &rejected_decision);
    CHECK(rejected.next_phase == AutonomyPhase::abstain);
    CHECK(rejected.reason == "hypothesis_rejected");

    auto first = advance_autonomous_cognition(
        state(), event("same", AutonomyEventKind::observation, std::nullopt), config());
    auto duplicate = advance_autonomous_cognition(
        first.state, event("same", AutonomyEventKind::observation, std::nullopt), config());
    CHECK(!duplicate.accepted && duplicate.reason == "duplicate_event");
    check_same_world(first.state, duplicate.state);
}

void test_action_and_evidence_failure_budgets() {
    auto action_state = state({{"autonomy_phase", "observe_result"},
                               {"active_hypothesis_id", "가설-1"},
                               {"verified_hypothesis_id", "가설-1"},
                               {"memory_ref", "memory:기억-1"}});
    auto first = advance_autonomous_cognition(
        action_state,
        event("fail-1", AutonomyEventKind::action_result, std::string("가설-1"),
              {{"success", false}}),
        config());
    CHECK(first.next_phase == AutonomyPhase::act && first.reason == "recover_action");
    auto reproposed = advance_autonomous_cognition(
        first.state,
        event("retry", AutonomyEventKind::action_proposal, std::string("가설-1"),
              {{"action", "search_memory"}}),
        config());
    auto exhausted = advance_autonomous_cognition(
        reproposed.state,
        event("fail-2", AutonomyEventKind::action_result, std::string("가설-1"),
              {{"success", false}}),
        config());
    CHECK(exhausted.next_phase == AutonomyPhase::abstain);
    CHECK(exhausted.reason == "failure_budget_exhausted");

    auto evidence_state = state(
        {{"autonomy_phase", "observe_evidence_result"},
         {"active_hypothesis_id", "가설-1"},
         {"requested_evidence_axes", JsonValue::Array{"cross_context"}}},
        {{"autonomy_evidence_action_failures", 1}});
    auto evidence_exhausted = advance_autonomous_cognition(
        evidence_state,
        event("efail", AutonomyEventKind::evidence_result, std::string("가설-1"),
              {{"success", false}}),
        evidence_config());
    CHECK(evidence_exhausted.next_phase == AutonomyPhase::abstain);
    CHECK(evidence_exhausted.reason == "evidence_failure_budget_exhausted");
}

void test_evidence_tool_collection() {
    auto current = state();
    for (const auto& item : std::vector<AutonomyEvent>{
             event("eo", AutonomyEventKind::observation, std::nullopt),
             event("eh", AutonomyEventKind::hypothesis),
             event("eq", AutonomyEventKind::evidence_request, std::string("가설-1"),
                   {{"requested_axes", JsonValue::Array{"observational"}},
                    {"collect_with_tool", true}})}) {
        current = advance_autonomous_cognition(current, item, evidence_config()).state;
    }
    CHECK(parse_autonomy_phase(current.goal_state().at("autonomy_phase").as_string()) ==
          AutonomyPhase::collect_evidence);
    auto action = advance_autonomous_cognition(
        current,
        event("ea", AutonomyEventKind::evidence_action, std::string("가설-1"),
              {{"action", "inspect_frame"}}),
        evidence_config());
    CHECK(action.evidence_tool_action_allowed);
    CHECK(action.evidence_tool_action == "inspect_frame");
    CHECK(action.next_phase == AutonomyPhase::observe_evidence_result);
    auto success = advance_autonomous_cognition(
        action.state,
        event("er", AutonomyEventKind::evidence_result, std::string("가설-1"),
              {{"success", true}}),
        evidence_config());
    CHECK(success.next_phase == AutonomyPhase::verify);
    CHECK(success.state.self_state().at("autonomy_evidence_action_failures").as_number() == 0.0);
}

void test_inert_rejections_and_validation() {
    auto act = state({{"autonomy_phase", "act"},
                      {"active_hypothesis_id", "가설-1"},
                      {"verified_hypothesis_id", "가설-1"},
                      {"memory_ref", "memory:기억-1"}});
    auto unlisted = advance_autonomous_cognition(
        act,
        event("danger", AutonomyEventKind::action_proposal, std::string("가설-1"),
              {{"action", "delete_file"}}),
        config());
    CHECK(!unlisted.accepted && unlisted.reason == "action_not_allowed");
    CHECK(unlisted.state.exact_equal(act));
    check_same_world(act, unlisted.state);

    rejects([] { AutonomousCognitionConfig({}, {}); }, "nonempty");
    rejects([] { AutonomousCognitionConfig({"read"}, {"write"}); }, "subset");
    rejects([] { AutonomousCognitionConfig({"read"}, {"read"},
                                            std::numeric_limits<double>::quiet_NaN()); },
            "[0, 1]");
    rejects([] {
        AutonomyEvent(" ", AutonomyEventKind::observation, std::nullopt, {}, "source",
                      "context", 1.0);
    }, "event_id");
    rejects([] {
        AutonomyEvent("id", AutonomyEventKind::observation, std::nullopt, {}, "source",
                      "context", 2.0);
    }, "confidence");
    CHECK(autonomous_cognition_source_sha256() ==
          "fc8eba5669003ffae65c8b724b0f0e50d90f8bba59a18cddd9147ed2ba24b95f");
}

}  // namespace

int main() {
    test_verified_loop_and_world_reuse();
    test_abstain_reject_duplicate_and_abandon();
    test_action_and_evidence_failure_budgets();
    test_evidence_tool_collection();
    test_inert_rejections_and_validation();
    std::cout << "autonomous cognition transition tests passed\n";
}
