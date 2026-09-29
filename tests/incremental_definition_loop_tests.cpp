#include "world/autonomous_cognition.hpp"
#include "world/hybrid_verification.hpp"
#include "world/sensor_counterfactual.hpp"
#include "world/sensor_definition.hpp"
#include "world/sensor_term_index.hpp"

#include <cassert>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

using namespace swegca::world;

namespace {

ContinuousSensorEvent event(const std::size_t index, std::string screen,
                            std::string audio) {
    return {index, "00:00:0" + std::to_string(index),
            "sensor:" + std::to_string(index),
            "context:" + std::to_string(index),
            "한국어 영상 — Mozilla Firefox", 1.0,
            std::move(screen), std::move(audio)};
}

const SensorDefinitionCandidate& selected_candidate(
    const std::vector<SensorDefinitionCandidate>& candidates) {
    const SensorDefinitionCandidate* selected = nullptr;
    for (const auto& candidate : candidates) {
        if (candidate.modality == "screen_ocr" && candidate.term == "게임") {
            if (selected != nullptr) throw std::runtime_error("duplicate selected candidate");
            selected = &candidate;
        }
    }
    if (selected == nullptr) throw std::runtime_error("selected candidate absent");
    return *selected;
}

Tensor tensor(const std::uint64_t slots) {
    return Tensor(TensorDType::float32, {1, slots, 8},
                  std::vector<double>(static_cast<std::size_t>(slots * 8)));
}

AutonomyEvent cognition_event(std::string id, const AutonomyEventKind kind,
                              std::optional<std::string> hypothesis,
                              std::vector<std::string> evidence,
                              JsonValue::Object payload = {}) {
    return AutonomyEvent(std::move(id), kind, std::move(hypothesis),
                         std::move(evidence), "n216-fixture", "n216-context",
                         1.0, std::move(payload));
}

}  // namespace

int main() {
    const std::vector<ContinuousSensorEvent> events{
        event(0, "게임 시작", "첫 발화"),
        event(1, "게임 메뉴", "둘 발화"),
        event(2, "설정 화면", "셋 발화"),
        event(3, "게임 도움말", "넷 발화")};

    const std::vector<ContinuousSensorEvent> base_events(
        events.begin(), events.end() - 1);
    const auto base = update_sensor_term_index(nullptr, base_events);
    const auto appended = append_sensor_term_index(base.index, events.back());
    assert(appended.reused_events == 3);
    assert(appended.tokenized_events == 1);
    assert(appended.expired_events == 0);

    const auto full_candidates = propose_sensor_definition_candidates(events, 2, 8);
    const auto indexed_candidates = propose_sensor_definition_candidates(
        events, 2, 8, &appended.index);
    assert(full_candidates.size() == indexed_candidates.size());
    for (std::size_t index = 0; index != full_candidates.size(); ++index) {
        assert(full_candidates[index].candidate_id == indexed_candidates[index].candidate_id);
        assert(full_candidates[index].modality == indexed_candidates[index].modality);
        assert(full_candidates[index].term == indexed_candidates[index].term);
        assert(full_candidates[index].source_event_indices ==
               indexed_candidates[index].source_event_indices);
        assert(full_candidates[index].evidence_refs ==
               indexed_candidates[index].evidence_refs);
    }
    const auto& candidate = selected_candidate(indexed_candidates);
    assert(candidate.definition_status == DefinitionStatus::partial);
    assert(!candidate.world_write_allowed);

    const auto executions = execute_sparse_sensor_counterfactuals(
        candidate, events, "가상항목", 2);
    assert(executions.size() == 3);
    std::vector<EvidenceObservation> observations =
        candidate_insufficient_observations(candidate, "firefox-natural-stream");
    const auto insufficient_count = observations.size();
    for (std::size_t index = 0; index != executions.size(); ++index) {
        assert(executions[index].expected_effect_observed);
        observations.push_back(counterfactual_execution_observation(
            executions[index], "firefox-natural-counterfactual",
            static_cast<std::int64_t>(candidate.evidence_refs.size() + index)));
        assert(observations.back().outcome == "support");
    }

    const EvidenceAccumulatorConfig evidence_config;
    std::int64_t current_step = 0;
    for (const auto& observation : observations) {
        current_step = std::max(current_step, observation.observed_at);
    }
    const auto cache = compile_evidence_replay(
        observations, evidence_config, candidate.candidate_id, current_step);
    assert(cache.stats.applied == 3);
    assert(cache.stats.insufficient == insufficient_count);
    const std::vector<ReplayIntervention> plans{{"normal", {}, {}, {}}};
    const auto interventions = compile_replay_interventions(cache, plans);

    const AutonomousCognitionConfig autonomy_config({"inspect"}, {"inspect"});
    const CognitiveState parent(tensor(4), tensor(2), tensor(2), {}, {},
                                {{"autonomy_phase", "abstain"}});
    auto state = advance_autonomous_cognition(
        parent,
        cognition_event("n216-observe", AutonomyEventKind::observation,
                        std::nullopt, {"sensor:0"}),
        autonomy_config).state;
    state = advance_autonomous_cognition(
        state,
        cognition_event("n216-hypothesis", AutonomyEventKind::hypothesis,
                        candidate.candidate_id, candidate.evidence_refs),
        autonomy_config).state;
    state = advance_autonomous_cognition(
        state,
        cognition_event("n216-request", AutonomyEventKind::evidence_request,
                        candidate.candidate_id, candidate.evidence_refs,
                        {{"requested_axes", JsonValue::Array{"counterfactual"}}}),
        autonomy_config).state;
    assert(parse_autonomy_phase(state.goal_state().at("autonomy_phase").as_string()) ==
           AutonomyPhase::verify);

    const AutonomyEvent verification(
        "n216-verify", AutonomyEventKind::verification,
        candidate.candidate_id, {"replay-cache:" + cache.cache_hash},
        "n216-fixture", "n216-context", 1.0,
        {{"replay_revision", static_cast<std::int64_t>(cache.stats.applied)},
         {"replay_intervention", "normal"}});
    const auto receipt = evaluate_and_advance_hybrid_verification(
        state, verification, autonomy_config, evidence_config, cache,
        interventions, "normal", HybridVerificationConfig(8192));
    assert(receipt.backend == VerificationBackend::cpu);
    assert(receipt.verification.selected.decision.status == "abstain");
    assert(receipt.verification.selected.decision.reason ==
           "minimum_effective_samples");
    assert(receipt.verification.transition.next_phase ==
           AutonomyPhase::request_evidence);
    assert(receipt.verification.transition.reason ==
           "additional_evidence_required");
    assert(!receipt.verification.transition.memory_write_allowed);
    assert(!receipt.verification.transition.tool_action_allowed);
    assert(receipt.verification.transition.state.persistent_state_count() == 1);
    assert(receipt.verification.world_tensor_objects_reused);
    assert(receipt.verification.transition.state.semantic_slots().storage_identity() ==
           parent.semantic_slots().storage_identity());
    assert(receipt.verification.transition.state.executive_slots().storage_identity() ==
           parent.executive_slots().storage_identity());
    assert(receipt.verification.transition.state.scratch_slots().storage_identity() ==
           parent.scratch_slots().storage_identity());
    assert(events[0].screen_ocr == "게임 시작");
    assert(events[3].screen_ocr == "게임 도움말");
    std::cout << "incremental definition to sparse counterfactual VERIFY loop passed\n";
}
