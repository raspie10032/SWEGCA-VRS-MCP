#pragma once

#include "world/definition_contract.hpp"
#include "world/evidence_accumulator.hpp"

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace swegca::world {

struct SensorTermIndex;

struct ContinuousSensorEvent final {
    std::size_t index = 0;
    std::string timestamp;
    std::string evidence_address;
    std::string context_hash;
    std::string window_title;
    double screen_motion_score = 0.0;
    std::string screen_ocr;
    std::string system_audio_transcript;

    friend bool operator==(const ContinuousSensorEvent&, const ContinuousSensorEvent&) = default;
};

struct CounterfactualDefinitionPlan final {
    std::string plan_id;
    std::string kind;
    std::vector<std::string> target_evidence_refs;
    std::string status{"unexecuted"};

    friend bool operator==(
        const CounterfactualDefinitionPlan&,
        const CounterfactualDefinitionPlan&) = default;
};

struct SensorDefinitionCandidate final {
    std::string candidate_id;
    std::string modality;
    std::string term;
    std::vector<std::string> evidence_refs;
    std::vector<std::string> source_event_content_hashes;
    std::vector<std::size_t> source_event_indices;
    DefinitionContract definition_contract;
    DefinitionStatus definition_status{DefinitionStatus::partial};
    std::vector<std::string> unresolved_definitions;
    std::vector<CounterfactualDefinitionPlan> counterfactual_plans;
    bool world_write_allowed = false;
};

[[nodiscard]] std::string continuous_sensor_event_content_hash(
    const ContinuousSensorEvent& event);

// Exact C++ counterpart of the pinned module's internal _candidate operation.
// Term extraction/indexing is a separate source contract and is not inferred
// inside this operation.
[[nodiscard]] SensorDefinitionCandidate make_sensor_definition_candidate(
    std::string modality, std::string term,
    std::span<const std::size_t> source_event_indices,
    std::span<const ContinuousSensorEvent> events);

[[nodiscard]] std::vector<SensorDefinitionCandidate> propose_sensor_definition_candidates(
    std::span<const ContinuousSensorEvent> events,
    std::size_t minimum_observations = 2,
    std::size_t maximum_candidates_per_modality = 8,
    const SensorTermIndex* term_index = nullptr);

[[nodiscard]] std::vector<EvidenceObservation> candidate_insufficient_observations(
    const SensorDefinitionCandidate& candidate, std::string source_family);

[[nodiscard]] constexpr std::string_view sensor_definition_source_sha256() noexcept {
    return "e34715b64e629d4e0dcb58af6becbeec7aabae139e343f7ffc521df1eba72a6a";
}

}  // namespace swegca::world
