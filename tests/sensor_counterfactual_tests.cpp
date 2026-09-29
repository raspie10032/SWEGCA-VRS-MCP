#include "world/sensor_counterfactual.hpp"
#include "world/sensor_definition.hpp"

#include <cassert>
#include <cstddef>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace swegca::world;

namespace {

ContinuousSensorEvent event(
    const std::size_t index,
    std::string screen,
    std::string audio) {
    return {index, "00:00:0" + std::to_string(index),
            "sensor:" + std::to_string(index),
            "context:" + std::to_string(index), "테스트", 1.0,
            std::move(screen), std::move(audio)};
}

const SensorDefinitionCandidate& screen_candidate(
    const std::vector<SensorDefinitionCandidate>& candidates) {
    for (const auto& candidate : candidates) {
        if (candidate.modality == "screen_ocr" && candidate.term == "게임") {
            return candidate;
        }
    }
    throw std::runtime_error("screen candidate absent");
}

const CounterfactualDefinitionPlan& plan(
    const SensorDefinitionCandidate& candidate,
    const std::string_view kind) {
    for (const auto& value : candidate.counterfactual_plans) {
        if (value.kind == kind) return value;
    }
    throw std::runtime_error("plan absent");
}

}  // namespace

int main() {
    const std::vector<ContinuousSensorEvent> events{
        event(0, "게임 시작", "첫 발화"),
        event(1, "게임 메뉴", "둘 발화"),
        event(2, "설정 화면", "셋 발화"),
        event(3, "도움말 화면", "넷 발화")};
    const auto candidates = propose_sensor_definition_candidates(events);
    const auto& candidate = screen_candidate(candidates);
    assert(candidate.candidate_id ==
           "sensor-definition:ec96eb3802dcd6ed41b79670f3c3ad3aad2978c7ab36ffdd1375a91bd46355cc");

    const auto removal = execute_sensor_counterfactual(
        candidate, plan(candidate, "removal"), events, std::nullopt, 2);
    const auto replacement = execute_sensor_counterfactual(
        candidate, plan(candidate, "replacement"), events, "가상항목", 2);
    const auto shifted = execute_sensor_counterfactual(
        candidate, plan(candidate, "temporal_shift"), events, "가상항목", 2);

    assert(removal.status == "executed" && removal.expected_effect_observed);
    assert(removal.before_indices == std::vector<std::size_t>({0, 1}));
    assert(removal.after_indices.empty());
    assert(removal.changed_event_indices == std::vector<std::size_t>({0, 1}));
    assert(removal.source_stream_hash ==
           "13a782b4ab243ab0fe9ba8bb2f7f9b7b6bcaaa8747a37c2e2624be7897c64800");
    assert(removal.transformed_stream_hash ==
           "aa70bd7c642762c8178712456f79b99529ed740381b5289afcd58e41ae904be1");
    assert(removal.transformed_events[0].screen_ocr == " 시작");
    assert(removal.transformed_events[1].screen_ocr == " 메뉴");
    assert(removal.transformed_events[0].evidence_address ==
           "sensor-counterfactual:bf34aa0a8fad061a15418031c18a77853a24f909a76a2bc227e2b6a79900c457");

    assert(replacement.status == "executed" && replacement.expected_effect_observed);
    assert(replacement.after_indices.empty());
    assert(replacement.transformed_stream_hash ==
           "3fe94c81c1ebe897631a82a478d0a91e421da47e704c45cd36d54bbce853a6f3");
    assert(replacement.transformed_events[0].screen_ocr == "가상항목 시작");
    assert(replacement.transformed_events[1].screen_ocr == "가상항목 메뉴");

    assert(shifted.status == "executed" && shifted.expected_effect_observed);
    assert(shifted.after_indices == std::vector<std::size_t>({2, 3}));
    assert(shifted.expected_after_indices == std::vector<std::size_t>({2, 3}));
    assert(shifted.changed_event_indices == std::vector<std::size_t>({0, 1, 2, 3}));
    assert(shifted.transformed_stream_hash ==
           "3032553fa4eac8bb691ffe07356c9661f5b67281182ea2777ccc230636dc78a3");
    assert(shifted.transformed_events[2].screen_ocr == "설정 화면 게임");
    assert(shifted.transformed_events[3].screen_ocr == "도움말 화면 게임");

    const auto observation = counterfactual_execution_observation(
        removal, "fixture", 4);
    assert(observation.axis == "counterfactual");
    assert(observation.outcome == "support");
    assert(observation.producer_confidence == 1.0);
    assert(observation.evidence_address ==
           "sensor-counterfactual-execution:aa70bd7c642762c8178712456f79b99529ed740381b5289afcd58e41ae904be1");

    const auto sparse = execute_sparse_sensor_counterfactuals(
        candidate, events, "가상항목", 2);
    assert(sparse.size() == 3);
    assert(sparse[0].transformed_stream_hash ==
           "db35bb539c94c607bd985f94ce5e6cd5c48f7489160c8e8eff1e1492b4d692ba");
    assert(sparse[1].transformed_stream_hash ==
           "cdc51fda7a302de0556584789c20dc375dd996568d596cc5121acda6aec79826");
    assert(sparse[2].transformed_stream_hash ==
           "f5f808967d8e81d10f2cc466333c41f39d4fa47d9001fedc9c9427365bb796a9");
    const std::vector<SensorCounterfactualExecution> dense{
        removal, replacement, shifted};
    for (std::size_t index = 0; index != sparse.size(); ++index) {
        const auto materialized = materialize_sparse_sensor_counterfactual(
            sparse[index], events);
        assert(materialized.size() == dense[index].transformed_events.size());
        for (std::size_t event_index = 0; event_index != materialized.size(); ++event_index) {
            assert(materialized[event_index].screen_ocr ==
                   dense[index].transformed_events[event_index].screen_ocr);
            assert(materialized[event_index].system_audio_transcript ==
                   dense[index].transformed_events[event_index].system_audio_transcript);
        }
        assert(sparse[index].before_indices == dense[index].before_indices);
        assert(sparse[index].after_indices == dense[index].after_indices);
        assert(sparse[index].expected_effect_observed ==
               dense[index].expected_effect_observed);
    }

    bool stale_rejected = false;
    auto changed = events;
    changed[0].system_audio_transcript = "교체 발화";
    try {
        static_cast<void>(execute_sensor_counterfactual(
            candidate, plan(candidate, "removal"), changed));
    } catch (const std::invalid_argument&) {
        stale_rejected = true;
    }
    assert(stale_rejected);

    bool identical_rejected = false;
    try {
        static_cast<void>(execute_sensor_counterfactual(
            candidate, plan(candidate, "replacement"), events, "게임"));
    } catch (const std::invalid_argument&) {
        identical_rejected = true;
    }
    assert(identical_rejected);

    auto existing_events = events;
    existing_events[2].screen_ocr = "가상항목";
    const auto existing_candidate = screen_candidate(
        propose_sensor_definition_candidates(existing_events));
    bool existing_rejected = false;
    try {
        static_cast<void>(execute_sensor_counterfactual(
            existing_candidate, plan(existing_candidate, "replacement"),
            existing_events, "가상항목"));
    } catch (const std::invalid_argument&) {
        existing_rejected = true;
    }
    assert(existing_rejected);

    bool materialization_rejected = false;
    auto other_stream = events;
    other_stream[0].window_title = "변경";
    try {
        static_cast<void>(materialize_sparse_sensor_counterfactual(
            sparse[0], other_stream));
    } catch (const std::invalid_argument&) {
        materialization_rejected = true;
    }
    assert(materialization_rejected);

    assert(sensor_counterfactual_source_sha256() ==
           "1d758a792d2cb237936108c60f3b46319172d3ed84ff7cbbbe87d19d2a01e83b");
    std::cout << "sensor counterfactual dense and sparse parity tests passed\n";
}
