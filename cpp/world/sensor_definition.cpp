#include "world/sensor_definition.hpp"

#include "swegca_architecture/sha256.hpp"
#include "world/sensor_term_index.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <map>
#include <set>
#include <utility>

namespace swegca::world {
namespace {

std::string json_string(const std::string_view value) {
    std::string result{"\""};
    for (std::size_t at = 0; at != value.size();) {
        const auto byte = static_cast<unsigned char>(value[at]);
        if (byte < 0x20U) {
            switch (byte) {
            case '\b': result += "\\b"; ++at; continue;
            case '\t': result += "\\t"; ++at; continue;
            case '\n': result += "\\n"; ++at; continue;
            case '\f': result += "\\f"; ++at; continue;
            case '\r': result += "\\r"; ++at; continue;
            default: break;
            }
            constexpr char digits[] = "0123456789abcdef";
            result += "\\u00";
            result.push_back(digits[byte >> 4U]);
            result.push_back(digits[byte & 15U]);
            ++at;
            continue;
        }
        if (byte == '"' || byte == '\\') {
            result.push_back('\\');
            result.push_back(static_cast<char>(byte));
            ++at;
            continue;
        }
        if (byte < 0x80U) {
            result.push_back(static_cast<char>(byte));
            ++at;
            continue;
        }
        std::size_t count = 0;
        char32_t point = 0;
        char32_t minimum = 0;
        if ((byte & 0xe0U) == 0xc0U) {
            count = 2; point = byte & 0x1fU; minimum = 0x80;
        } else if ((byte & 0xf0U) == 0xe0U) {
            count = 3; point = byte & 0x0fU; minimum = 0x800;
        } else if ((byte & 0xf8U) == 0xf0U) {
            count = 4; point = byte & 0x07U; minimum = 0x10000;
        } else {
            throw std::invalid_argument("sensor definition contains invalid UTF-8");
        }
        if (count > value.size() - at) {
            throw std::invalid_argument("sensor definition contains invalid UTF-8");
        }
        for (std::size_t offset = 1; offset != count; ++offset) {
            const auto continuation = static_cast<unsigned char>(value[at + offset]);
            if ((continuation & 0xc0U) != 0x80U) {
                throw std::invalid_argument("sensor definition contains invalid UTF-8");
            }
            point = (point << 6U) | (continuation & 0x3fU);
        }
        if (point < minimum || point > 0x10ffffU ||
            (point >= 0xd800U && point <= 0xdfffU)) {
            throw std::invalid_argument("sensor definition contains invalid UTF-8");
        }
        result.append(value.substr(at, count));
        at += count;
    }
    result.push_back('"');
    return result;
}

std::string python_float(const double value) {
    if (std::isnan(value)) return "NaN";
    if (std::isinf(value)) return std::signbit(value) ? "-Infinity" : "Infinity";
    char storage[64]{};
    const auto [end, error] = std::to_chars(
        std::begin(storage), std::end(storage), value, std::chars_format::general);
    if (error != std::errc{}) throw std::runtime_error("cannot encode sensor number");
    std::string result(storage, end);
    if (result.find_first_of(".eE") == std::string::npos) result += ".0";
    return result;
}

std::string string_array(const std::span<const std::string> values) {
    std::string result{"["};
    for (std::size_t index = 0; index != values.size(); ++index) {
        if (index != 0) result.push_back(',');
        result += json_string(values[index]);
    }
    result.push_back(']');
    return result;
}

std::string digest(const std::string_view value) {
    architecture::Sha256 sha;
    sha.update(value);
    constexpr char digits[] = "0123456789abcdef";
    const auto bytes = sha.finish();
    std::string result(bytes.size() * 2, '0');
    for (std::size_t index = 0; index != bytes.size(); ++index) {
        const auto byte = std::to_integer<unsigned>(bytes[index]);
        result[index * 2] = digits[byte >> 4U];
        result[index * 2 + 1] = digits[byte & 15U];
    }
    return result;
}

DefinitionContract partial_contract() {
    return {
        "recurrent_extracted_sensor_term",
        {"normalized_term", "observation_presence", "observation_time", "uncertainty"},
        "content_addressed_continuous_sensor_event",
        "NFKC and casefold exact extracted-term equality only; semantic referent identity is not asserted",
        "removal, replacement, and temporal shift modify evidence only",
        "sensor-event SHA-256 plus source event index",
        "unverified extraction, referent, and causality remain unknown",
        "candidate remains partial until independent execution and replication",
        {"removal", "replacement", "temporal_shift"},
        {"support", "refute", "insufficient"},
        {"extraction_correctness", "semantic_referent",
         "independent_source_replication", "counterfactual_execution"}};
}

}  // namespace

std::string continuous_sensor_event_content_hash(const ContinuousSensorEvent& event) {
    std::string json{"{\"index\":"};
    json += std::to_string(event.index);
    json += ",\"screen_motion_score\":" + python_float(event.screen_motion_score);
    json += ",\"screen_ocr\":" + json_string(event.screen_ocr);
    json += ",\"system_audio_transcript\":" + json_string(event.system_audio_transcript);
    json += ",\"timestamp\":" + json_string(event.timestamp);
    json += ",\"window_title\":" + json_string(event.window_title) + "}";
    return digest(json);
}

SensorDefinitionCandidate make_sensor_definition_candidate(
    std::string modality, std::string term,
    const std::span<const std::size_t> source_event_indices,
    const std::span<const ContinuousSensorEvent> events) {
    if (modality != "screen_ocr" && modality != "system_audio_transcript" &&
        modality != "cross_modal_exact_term") {
        throw std::invalid_argument("unsupported sensor definition modality");
    }
    std::vector<std::string> evidence_refs;
    std::vector<std::string> content_hashes;
    evidence_refs.reserve(source_event_indices.size());
    content_hashes.reserve(source_event_indices.size());
    for (const auto index : source_event_indices) {
        if (index >= events.size()) {
            throw std::out_of_range("sensor definition source index is outside events");
        }
        evidence_refs.push_back(events[index].evidence_address);
        content_hashes.push_back(continuous_sensor_event_content_hash(events[index]));
    }
    const auto candidate_payload = "[" + json_string(modality) + "," +
        json_string(term) + "," + string_array(evidence_refs) + "," +
        string_array(content_hashes) + "]";
    const auto candidate_id = "sensor-definition:" + digest(candidate_payload);
    auto contract = partial_contract();
    const auto assessment = validate_definition_contract(contract);
    if (assessment.status != DefinitionStatus::partial || assessment.writes_enabled) {
        throw std::runtime_error("unverified sensor definition became writable");
    }
    std::vector<CounterfactualDefinitionPlan> plans;
    for (const auto& kind : contract.counterfactuals) {
        const auto payload = "[" + json_string(candidate_id) + "," +
            json_string(kind) + "," + string_array(evidence_refs) + "]";
        plans.push_back({"counterfactual-plan:" + digest(payload), kind,
                         evidence_refs, "unexecuted"});
    }
    return {candidate_id, std::move(modality), std::move(term),
            std::move(evidence_refs), std::move(content_hashes),
            std::vector<std::size_t>(source_event_indices.begin(), source_event_indices.end()),
            std::move(contract), DefinitionStatus::partial,
            assessment.unresolved_definitions, std::move(plans), false};
}

std::vector<SensorDefinitionCandidate> propose_sensor_definition_candidates(
    const std::span<const ContinuousSensorEvent> events,
    const std::size_t minimum_observations,
    const std::size_t maximum_candidates_per_modality,
    const SensorTermIndex* term_index) {
    if (events.empty()) return {};
    if (minimum_observations == 0 || maximum_candidates_per_modality == 0) {
        throw std::invalid_argument("OCR proposal limits must be positive");
    }
    SensorTermIndex temporary;
    if (term_index == nullptr) {
        temporary = update_sensor_term_index(nullptr, events).index;
        term_index = &temporary;
    } else {
        validate_sensor_term_index(*term_index, events);
    }
    const auto screen = indexed_term_candidates(
        *term_index, "screen_ocr", minimum_observations,
        maximum_candidates_per_modality);
    const auto audio = indexed_term_candidates(
        *term_index, "system_audio_transcript", minimum_observations,
        maximum_candidates_per_modality);
    std::vector<SensorDefinitionCandidate> candidates;
    candidates.reserve(screen.size() + audio.size());
    for (const auto& proposal : screen) {
        candidates.push_back(make_sensor_definition_candidate(
            "screen_ocr", proposal.value, proposal.observation_indices, events));
    }
    for (const auto& proposal : audio) {
        candidates.push_back(make_sensor_definition_candidate(
            "system_audio_transcript", proposal.value,
            proposal.observation_indices, events));
    }
    std::map<std::string, const OcrTermCandidate*> screen_by_term;
    std::map<std::string, const OcrTermCandidate*> audio_by_term;
    auto fold = [](std::string value) {
        for (auto& byte : value) {
            if (byte >= 'A' && byte <= 'Z') byte = static_cast<char>(byte + ('a' - 'A'));
        }
        return value;
    };
    for (const auto& proposal : screen) screen_by_term[fold(proposal.value)] = &proposal;
    for (const auto& proposal : audio) audio_by_term[fold(proposal.value)] = &proposal;
    for (const auto& [key, ocr] : screen_by_term) {
        const auto found = audio_by_term.find(key);
        if (found == audio_by_term.end()) continue;
        std::vector<std::size_t> indices;
        std::set_intersection(
            ocr->observation_indices.begin(), ocr->observation_indices.end(),
            found->second->observation_indices.begin(),
            found->second->observation_indices.end(),
            std::back_inserter(indices));
        if (indices.size() >= minimum_observations) {
            candidates.push_back(make_sensor_definition_candidate(
                "cross_modal_exact_term", ocr->value, indices, events));
        }
    }
    std::sort(candidates.begin(), candidates.end(), [&](const auto& left, const auto& right) {
        return std::tuple{left.modality, fold(left.term), left.candidate_id} <
               std::tuple{right.modality, fold(right.term), right.candidate_id};
    });
    return candidates;
}

std::vector<EvidenceObservation> candidate_insufficient_observations(
    const SensorDefinitionCandidate& candidate, std::string source_family) {
    std::vector<EvidenceObservation> observations;
    observations.reserve(candidate.evidence_refs.size() +
                         candidate.counterfactual_plans.size());
    for (std::size_t index = 0; index != candidate.evidence_refs.size(); ++index) {
        observations.emplace_back(
            candidate.candidate_id, candidate.evidence_refs[index], source_family,
            "definition-context:" + std::to_string(index), "observational",
            "insufficient", static_cast<std::int64_t>(index), std::nullopt,
            "partial-sensor-definition", 0.0);
    }
    for (std::size_t index = 0; index != candidate.counterfactual_plans.size(); ++index) {
        // Preserve the pinned generator/list.extend evaluation order exactly:
        // len(observations) grows once per appended plan before index is added.
        const auto observed_at = observations.size() + index;
        observations.emplace_back(
            candidate.candidate_id, candidate.counterfactual_plans[index].plan_id,
            source_family, "counterfactual-context:" + std::to_string(index),
            "counterfactual", "insufficient",
            static_cast<std::int64_t>(observed_at), std::nullopt,
            "unexecuted-counterfactual-plan", 0.0);
    }
    return observations;
}

}  // namespace swegca::world
