#pragma once

#include "world/sensor_definition.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace swegca::world {

struct OcrTermCandidate final {
    std::string value;
    std::vector<std::size_t> observation_indices;

    friend bool operator==(const OcrTermCandidate&, const OcrTermCandidate&) = default;
};

struct IndexedSensorTerms final {
    std::string evidence_address;
    std::string content_hash;
    std::vector<std::string> screen_terms;
    std::vector<std::string> audio_terms;

    friend bool operator==(const IndexedSensorTerms&, const IndexedSensorTerms&) = default;
};

struct SensorTermIndex final {
    std::vector<IndexedSensorTerms> indexed_events;
    std::vector<OcrTermCandidate> screen_candidates;
    std::vector<OcrTermCandidate> audio_candidates;
    std::string index_hash;
    std::vector<std::uintptr_t> event_object_ids;

    friend bool operator==(const SensorTermIndex&, const SensorTermIndex&) = default;
};

struct SensorTermIndexUpdate final {
    SensorTermIndex index;
    std::size_t reused_events = 0;
    std::size_t tokenized_events = 0;
    std::size_t expired_events = 0;
};

[[nodiscard]] std::vector<std::string> extract_ocr_terms(std::string_view text);

[[nodiscard]] SensorTermIndexUpdate update_sensor_term_index(
    const SensorTermIndex* previous,
    std::span<const ContinuousSensorEvent> events);

[[nodiscard]] SensorTermIndexUpdate append_sensor_term_index(
    const SensorTermIndex& previous,
    const ContinuousSensorEvent& event);

void validate_sensor_term_index(
    const SensorTermIndex& index,
    std::span<const ContinuousSensorEvent> events);

[[nodiscard]] std::vector<OcrTermCandidate> indexed_term_candidates(
    const SensorTermIndex& index,
    std::string_view modality,
    std::size_t minimum_observations,
    std::size_t maximum_candidates);

[[nodiscard]] constexpr std::string_view sensor_term_index_source_sha256() noexcept {
    return "9f294603250cd6e635c79346eab86d0469fbdfd2728fdabfe4e903194f2638bf";
}

}  // namespace swegca::world
