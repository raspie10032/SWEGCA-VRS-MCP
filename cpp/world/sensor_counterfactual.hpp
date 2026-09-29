#pragma once

#include "world/evidence_accumulator.hpp"
#include "world/sensor_definition.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace swegca::world {

struct SensorCounterfactualExecution final {
    std::string plan_id;
    std::string candidate_id;
    std::string kind;
    std::string status;
    std::string reason;
    std::string source_stream_hash;
    std::string transformed_stream_hash;
    std::vector<std::size_t> before_indices;
    std::vector<std::size_t> after_indices;
    std::vector<std::size_t> expected_after_indices;
    std::vector<std::size_t> changed_event_indices;
    std::vector<ContinuousSensorEvent> transformed_events;
    bool expected_effect_observed = false;
};

struct SensorCounterfactualPatch final {
    std::size_t event_index = 0;
    std::string source_content_hash;
    std::string screen_ocr;
    std::string system_audio_transcript;

    friend bool operator==(
        const SensorCounterfactualPatch&,
        const SensorCounterfactualPatch&) = default;
};

struct SparseSensorCounterfactualExecution final {
    std::string plan_id;
    std::string candidate_id;
    std::string kind;
    std::string status;
    std::string reason;
    std::string source_stream_hash;
    std::string transformed_stream_hash;
    std::vector<std::size_t> before_indices;
    std::vector<std::size_t> after_indices;
    std::vector<std::size_t> expected_after_indices;
    std::vector<std::size_t> changed_event_indices;
    std::vector<SensorCounterfactualPatch> patches;
    bool expected_effect_observed = false;
};

[[nodiscard]] SensorCounterfactualExecution execute_sensor_counterfactual(
    const SensorDefinitionCandidate& candidate,
    const CounterfactualDefinitionPlan& plan,
    std::span<const ContinuousSensorEvent> events,
    std::optional<std::string_view> replacement_term = std::nullopt,
    std::int64_t temporal_shift = 1);

[[nodiscard]] std::vector<SparseSensorCounterfactualExecution>
execute_sparse_sensor_counterfactuals(
    const SensorDefinitionCandidate& candidate,
    std::span<const ContinuousSensorEvent> events,
    std::string_view replacement_term,
    std::int64_t temporal_shift = 1);

[[nodiscard]] std::vector<ContinuousSensorEvent>
materialize_sparse_sensor_counterfactual(
    const SparseSensorCounterfactualExecution& execution,
    std::span<const ContinuousSensorEvent> events);

[[nodiscard]] EvidenceObservation counterfactual_execution_observation(
    const SensorCounterfactualExecution& execution,
    std::string source_family,
    std::int64_t observed_at);

[[nodiscard]] EvidenceObservation counterfactual_execution_observation(
    const SparseSensorCounterfactualExecution& execution,
    std::string source_family,
    std::int64_t observed_at);

[[nodiscard]] constexpr std::string_view sensor_counterfactual_source_sha256() noexcept {
    return "1d758a792d2cb237936108c60f3b46319172d3ed84ff7cbbbe87d19d2a01e83b";
}

}  // namespace swegca::world
