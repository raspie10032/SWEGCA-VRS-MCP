#include "world/sensor_counterfactual.hpp"

#include "swegca_architecture/sha256.hpp"
#include "world/sensor_term_index.hpp"
#include "world/unicode_nfkc.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <stdexcept>
#include <utility>

namespace swegca::world {
namespace {

std::vector<char32_t> decode_utf8(const std::string_view input) {
    // normalize_nfkc is also the strict UTF-8 validator. Preserve the raw
    // scalar sequence here because _contains_term in the pinned source runs
    // its regular expression before per-term NFKC.
    static_cast<void>(normalize_nfkc(input));
    std::vector<char32_t> result;
    for (std::size_t at = 0; at != input.size();) {
        const auto first = static_cast<unsigned char>(input[at]);
        std::size_t size = 0;
        char32_t point = 0;
        if (first < 0x80U) { size = 1; point = first; }
        else if ((first & 0xe0U) == 0xc0U) { size = 2; point = first & 0x1fU; }
        else if ((first & 0xf0U) == 0xe0U) { size = 3; point = first & 0x0fU; }
        else { size = 4; point = first & 0x07U; }
        for (std::size_t index = 1; index != size; ++index) {
            point = (point << 6U) |
                (static_cast<unsigned char>(input[at + index]) & 0x3fU);
        }
        result.push_back(point);
        at += size;
    }
    return result;
}

void append_utf8(std::string& output, const char32_t point) {
    if (point <= 0x7fU) output.push_back(static_cast<char>(point));
    else if (point <= 0x7ffU) {
        output.push_back(static_cast<char>(0xc0U | (point >> 6U)));
        output.push_back(static_cast<char>(0x80U | (point & 0x3fU)));
    } else if (point <= 0xffffU) {
        output.push_back(static_cast<char>(0xe0U | (point >> 12U)));
        output.push_back(static_cast<char>(0x80U | ((point >> 6U) & 0x3fU)));
        output.push_back(static_cast<char>(0x80U | (point & 0x3fU)));
    } else {
        output.push_back(static_cast<char>(0xf0U | (point >> 18U)));
        output.push_back(static_cast<char>(0x80U | ((point >> 12U) & 0x3fU)));
        output.push_back(static_cast<char>(0x80U | ((point >> 6U) & 0x3fU)));
        output.push_back(static_cast<char>(0x80U | (point & 0x3fU)));
    }
}

std::string encode_range(
    const std::span<const char32_t> points,
    const std::size_t first,
    const std::size_t size) {
    std::string result;
    for (std::size_t index = first; index != first + size; ++index) {
        append_utf8(result, points[index]);
    }
    return result;
}

bool ascii_letter(const char32_t point) {
    return (point >= U'A' && point <= U'Z') || (point >= U'a' && point <= U'z');
}

bool ascii_continuation(const char32_t point) {
    return ascii_letter(point) || (point >= U'0' && point <= U'9') || point == U'-';
}

std::size_t regex_match_size(
    const std::span<const char32_t> points,
    const std::size_t at) {
    if (points[at] >= U'가' && points[at] <= U'힣') {
        std::size_t size = 1;
        while (size < 20 && at + size < points.size() &&
               points[at + size] >= U'가' && points[at + size] <= U'힣') ++size;
        return size >= 2 ? size : 0;
    }
    if (ascii_letter(points[at])) {
        std::size_t size = 1;
        while (size < 32 && at + size < points.size() &&
               ascii_continuation(points[at + size])) ++size;
        return size >= 3 ? size : 0;
    }
    return 0;
}

std::size_t unicode_size(const std::string_view value) {
    return static_cast<std::size_t>(std::count_if(value.begin(), value.end(), [](char byte) {
        return (static_cast<unsigned char>(byte) & 0xc0U) != 0x80U;
    }));
}

std::string term_key(const std::string_view raw) {
    auto normalized = normalize_nfkc(raw);
    constexpr std::string_view suffix = "입니다";
    if (normalized.ends_with(suffix) && unicode_size(normalized) > 5) {
        normalized.resize(normalized.size() - suffix.size());
    }
    return unicode_casefold(normalized);
}

bool contains_term(const std::string_view text, const std::string_view term) {
    const auto key = term_key(term);
    const auto points = decode_utf8(text);
    for (std::size_t at = 0; at < points.size();) {
        const auto size = regex_match_size(points, at);
        if (size == 0) { ++at; continue; }
        if (term_key(encode_range(points, at, size)) == key) return true;
        at += size;
    }
    return false;
}

std::pair<std::string, std::size_t> rewrite_term(
    const std::string_view text,
    const std::string_view term,
    const std::string_view replacement) {
    const auto normalized = normalize_nfkc(text);
    const auto points = decode_utf8(normalized);
    const auto key = term_key(term);
    std::string result;
    std::size_t changes = 0;
    for (std::size_t at = 0; at < points.size();) {
        const auto size = regex_match_size(points, at);
        if (size == 0) {
            append_utf8(result, points[at++]);
            continue;
        }
        const auto raw = encode_range(points, at, size);
        if (term_key(raw) == key) {
            result += replacement;
            ++changes;
        } else {
            result += raw;
        }
        at += size;
    }
    return {std::move(result), changes};
}

std::string json_string(const std::string_view value) {
    std::string result{"\""};
    for (const auto raw : value) {
        const auto byte = static_cast<unsigned char>(raw);
        switch (byte) {
        case '\b': result += "\\b"; break;
        case '\t': result += "\\t"; break;
        case '\n': result += "\\n"; break;
        case '\f': result += "\\f"; break;
        case '\r': result += "\\r"; break;
        case '"': result += "\\\""; break;
        case '\\': result += "\\\\"; break;
        default:
            if (byte < 0x20U) {
                constexpr char digits[] = "0123456789abcdef";
                result += "\\u00";
                result.push_back(digits[byte >> 4U]);
                result.push_back(digits[byte & 15U]);
            } else result.push_back(raw);
        }
    }
    result.push_back('"');
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

std::vector<std::size_t> candidate_indices(
    const SensorDefinitionCandidate& candidate,
    const std::span<const ContinuousSensorEvent> events) {
    std::set<std::size_t> screen;
    std::set<std::size_t> audio;
    for (std::size_t index = 0; index != events.size(); ++index) {
        if (contains_term(events[index].screen_ocr, candidate.term)) screen.insert(index);
        if (contains_term(events[index].system_audio_transcript, candidate.term)) {
            audio.insert(index);
        }
    }
    std::vector<std::size_t> result;
    if (candidate.modality == "screen_ocr") result.assign(screen.begin(), screen.end());
    else if (candidate.modality == "system_audio_transcript") {
        result.assign(audio.begin(), audio.end());
    } else {
        std::set_intersection(screen.begin(), screen.end(), audio.begin(), audio.end(),
                              std::back_inserter(result));
    }
    return result;
}

ContinuousSensorEvent with_content_address(
    const ContinuousSensorEvent& event,
    std::string screen,
    std::string audio,
    const std::string_view plan_id) {
    const auto payload = "{\"index\":" + std::to_string(event.index) +
        ",\"plan_id\":" + json_string(plan_id) +
        ",\"screen_ocr\":" + json_string(screen) +
        ",\"source\":" + json_string(event.evidence_address) +
        ",\"system_audio_transcript\":" + json_string(audio) + "}";
    const auto hash = digest(payload);
    auto result = event;
    result.evidence_address = "sensor-counterfactual:" + hash;
    result.context_hash = "sensor-counterfactual-context:" + hash;
    result.screen_ocr = std::move(screen);
    result.system_audio_transcript = std::move(audio);
    return result;
}

std::string stream_hash(const std::span<const ContinuousSensorEvent> events) {
    std::string payload{"["};
    for (std::size_t index = 0; index != events.size(); ++index) {
        if (index != 0) payload.push_back(',');
        payload += "[" + json_string(events[index].evidence_address) + "," +
            json_string(events[index].context_hash) + "," +
            json_string(continuous_sensor_event_content_hash(events[index])) + "]";
    }
    payload.push_back(']');
    return digest(payload);
}

void validate_candidate_lineage(
    const SensorDefinitionCandidate& candidate,
    const CounterfactualDefinitionPlan& plan,
    const std::span<const ContinuousSensorEvent> events) {
    if (std::find(candidate.counterfactual_plans.begin(),
                  candidate.counterfactual_plans.end(), plan) ==
        candidate.counterfactual_plans.end()) {
        throw std::invalid_argument("counterfactual plan does not belong to candidate");
    }
    if (events.empty()) {
        throw std::invalid_argument("counterfactual execution requires sensor events");
    }
    std::vector<std::string> refs;
    std::vector<std::string> content_hashes;
    for (const auto index : candidate.source_event_indices) {
        if (index >= events.size()) {
            throw std::invalid_argument("candidate source index is outside the sensor stream");
        }
        refs.push_back(events[index].evidence_address);
        content_hashes.push_back(continuous_sensor_event_content_hash(events[index]));
    }
    if (refs != candidate.evidence_refs || refs != plan.target_evidence_refs) {
        throw std::invalid_argument("candidate evidence lineage differs from sensor stream");
    }
    if (content_hashes != candidate.source_event_content_hashes) {
        throw std::invalid_argument("candidate content lineage differs from sensor stream");
    }
}

std::size_t shifted_index(
    const std::size_t index,
    const std::int64_t raw_shift,
    const std::size_t size) {
    const auto modulus = static_cast<std::int64_t>(size);
    auto shift = raw_shift % modulus;
    if (shift < 0) shift += modulus;
    return (index + static_cast<std::size_t>(shift)) % size;
}

std::vector<std::size_t> destination_indices(
    const std::span<const std::size_t> source,
    const std::int64_t shift,
    const std::size_t size) {
    std::set<std::size_t> values;
    for (const auto index : source) values.insert(shifted_index(index, shift, size));
    return {values.begin(), values.end()};
}

SparseSensorCounterfactualExecution sparse_execution(
    const SensorDefinitionCandidate& candidate,
    const CounterfactualDefinitionPlan& plan,
    const std::span<const ContinuousSensorEvent> events,
    const std::string& source_hash,
    const std::string_view replacement_term,
    const std::int64_t temporal_shift) {
    const auto before = candidate.source_event_indices;
    std::map<std::size_t, std::pair<std::string, std::string>> values;
    for (const auto index : before) {
        values[index] = {events[index].screen_ocr, events[index].system_audio_transcript};
    }
    std::vector<std::size_t> expected_after;
    std::vector<std::size_t> after;
    if (plan.kind == "removal" || plan.kind == "replacement") {
        const auto replacement = plan.kind == "removal" ? std::string_view{} : replacement_term;
        for (const auto index : before) {
            auto& [screen, audio] = values[index];
            if (candidate.modality == "screen_ocr" ||
                candidate.modality == "cross_modal_exact_term") {
                screen = rewrite_term(screen, candidate.term, replacement).first;
            }
            if (candidate.modality == "system_audio_transcript" ||
                candidate.modality == "cross_modal_exact_term") {
                audio = rewrite_term(audio, candidate.term, replacement).first;
            }
        }
    } else {
        const auto destinations = destination_indices(before, temporal_shift, events.size());
        for (const auto index : destinations) {
            values.try_emplace(index, events[index].screen_ocr,
                                events[index].system_audio_transcript);
        }
        if (candidate.modality == "screen_ocr") {
            for (const auto index : before) {
                values[index].first = rewrite_term(
                    values[index].first, candidate.term, "").first;
            }
            for (const auto index : destinations) {
                values[index].first = strip_unicode_whitespace(
                    values[index].first + " " + candidate.term);
            }
            after = destinations;
        } else {
            for (const auto index : before) {
                values[index].second = rewrite_term(
                    values[index].second, candidate.term, "").first;
            }
            for (const auto index : destinations) {
                values[index].second = strip_unicode_whitespace(
                    values[index].second + " " + candidate.term);
            }
            if (candidate.modality == "system_audio_transcript") after = destinations;
            else {
                std::set<std::size_t> screen_indices;
                std::set<std::size_t> audio_indices;
                for (std::size_t index = 0; index != events.size(); ++index) {
                    if (contains_term(events[index].screen_ocr, candidate.term)) {
                        screen_indices.insert(index);
                    }
                    if (contains_term(events[index].system_audio_transcript, candidate.term)) {
                        audio_indices.insert(index);
                    }
                }
                for (const auto index : before) audio_indices.erase(index);
                audio_indices.insert(destinations.begin(), destinations.end());
                std::set_intersection(
                    screen_indices.begin(), screen_indices.end(),
                    audio_indices.begin(), audio_indices.end(),
                    std::back_inserter(after));
            }
        }
        expected_after = destinations;
    }
    std::vector<SensorCounterfactualPatch> patches;
    for (auto& [index, content] : values) {
        if (content.first != events[index].screen_ocr ||
            content.second != events[index].system_audio_transcript) {
            patches.push_back({index, continuous_sensor_event_content_hash(events[index]),
                               std::move(content.first), std::move(content.second)});
        }
    }
    const bool expected_effect =
        plan.kind == "temporal_shift" &&
        candidate.modality == "cross_modal_exact_term" ? after != before
                                                       : after == expected_after;
    const auto status = patches.empty() ? "insufficient" : "executed";
    auto reason = expected_effect ? "expected_effect_observed" : "expected_effect_missing";
    if (patches.empty()) reason = "no_source_evidence_changed";
    std::string patch_payload{"["};
    for (std::size_t index = 0; index != patches.size(); ++index) {
        if (index != 0) patch_payload.push_back(',');
        const auto& patch = patches[index];
        patch_payload += "[" + std::to_string(patch.event_index) + "," +
            json_string(patch.source_content_hash) + "," + json_string(patch.screen_ocr) +
            "," + json_string(patch.system_audio_transcript) + "]";
    }
    patch_payload.push_back(']');
    const auto transformed_hash = digest(
        "[" + json_string(source_hash) + "," + json_string(plan.plan_id) + "," +
        patch_payload + "]");
    std::vector<std::size_t> changed;
    for (const auto& patch : patches) changed.push_back(patch.event_index);
    return {plan.plan_id, candidate.candidate_id, plan.kind, status, reason,
            source_hash, transformed_hash, before, after, expected_after,
            std::move(changed), std::move(patches), expected_effect};
}

template <class Execution>
EvidenceObservation make_observation(
    const Execution& execution,
    std::string source_family,
    const std::int64_t observed_at) {
    if (source_family.empty()) throw std::invalid_argument("source family must be nonempty");
    std::string outcome;
    double confidence = 0.0;
    if (execution.status == "insufficient") outcome = "insufficient";
    else {
        outcome = execution.expected_effect_observed ? "support" : "refute";
        confidence = 1.0;
    }
    return {execution.candidate_id,
            "sensor-counterfactual-execution:" + execution.transformed_stream_hash,
            std::move(source_family),
            "sensor-counterfactual-context:" + execution.plan_id,
            "counterfactual", std::move(outcome), observed_at, std::nullopt,
            "bounded-sensor-counterfactual-executor", confidence};
}

}  // namespace

SensorCounterfactualExecution execute_sensor_counterfactual(
    const SensorDefinitionCandidate& candidate,
    const CounterfactualDefinitionPlan& plan,
    const std::span<const ContinuousSensorEvent> events,
    const std::optional<std::string_view> replacement_term,
    const std::int64_t temporal_shift) {
    validate_candidate_lineage(candidate, plan, events);
    const auto before = candidate_indices(candidate, events);
    if (before != candidate.source_event_indices) {
        throw std::invalid_argument("candidate indices no longer match source evidence");
    }
    std::vector<ContinuousSensorEvent> transformed(events.begin(), events.end());
    std::set<std::size_t> changed;
    std::vector<std::size_t> expected_after;
    if (plan.kind == "removal" || plan.kind == "replacement") {
        const auto replacement = plan.kind == "removal"
            ? std::string_view{} : replacement_term.value_or(std::string_view{});
        if (plan.kind == "replacement" && strip_unicode_whitespace(replacement).empty()) {
            throw std::invalid_argument("replacement counterfactual requires a nonempty term");
        }
        if (plan.kind == "replacement") {
            if (term_key(replacement) == term_key(candidate.term)) {
                throw std::invalid_argument("replacement term must differ from candidate term");
            }
            if (std::any_of(events.begin(), events.end(), [&](const auto& event) {
                    return contains_term(event.screen_ocr, replacement) ||
                           contains_term(event.system_audio_transcript, replacement);
                })) {
                throw std::invalid_argument("replacement term already exists in source evidence");
            }
        }
        for (const auto index : candidate.source_event_indices) {
            auto screen = transformed[index].screen_ocr;
            auto audio = transformed[index].system_audio_transcript;
            std::size_t changes = 0;
            if (candidate.modality == "screen_ocr" ||
                candidate.modality == "cross_modal_exact_term") {
                auto rewritten = rewrite_term(screen, candidate.term, replacement);
                screen = std::move(rewritten.first);
                changes += rewritten.second;
            }
            if (candidate.modality == "system_audio_transcript" ||
                candidate.modality == "cross_modal_exact_term") {
                auto rewritten = rewrite_term(audio, candidate.term, replacement);
                audio = std::move(rewritten.first);
                changes += rewritten.second;
            }
            if (changes != 0) {
                changed.insert(index);
                transformed[index] = with_content_address(
                    events[index], std::move(screen), std::move(audio), plan.plan_id);
            }
        }
    } else {
        const auto destinations = destination_indices(
            candidate.source_event_indices, temporal_shift, events.size());
        if (destinations == candidate.source_event_indices &&
            temporal_shift % static_cast<std::int64_t>(events.size()) == 0) {
            throw std::invalid_argument("temporal shift must move at least one event");
        }
        std::vector<std::string> screen;
        std::vector<std::string> audio;
        for (const auto& event : events) {
            screen.push_back(event.screen_ocr);
            audio.push_back(event.system_audio_transcript);
        }
        if (candidate.modality == "screen_ocr") {
            for (const auto index : candidate.source_event_indices) {
                screen[index] = rewrite_term(screen[index], candidate.term, "").first;
            }
            for (const auto index : destinations) {
                screen[index] = strip_unicode_whitespace(screen[index] + " " + candidate.term);
            }
        } else {
            for (const auto index : candidate.source_event_indices) {
                audio[index] = rewrite_term(audio[index], candidate.term, "").first;
            }
            for (const auto index : destinations) {
                audio[index] = strip_unicode_whitespace(audio[index] + " " + candidate.term);
            }
        }
        expected_after = destinations;
        for (std::size_t index = 0; index != events.size(); ++index) {
            if (screen[index] == events[index].screen_ocr &&
                audio[index] == events[index].system_audio_transcript) continue;
            changed.insert(index);
            transformed[index] = with_content_address(
                events[index], std::move(screen[index]), std::move(audio[index]), plan.plan_id);
        }
    }
    const auto after = candidate_indices(candidate, transformed);
    const bool expected_effect =
        plan.kind == "temporal_shift" &&
        candidate.modality == "cross_modal_exact_term" ? after != before
                                                       : after == expected_after;
    const auto status = changed.empty() ? "insufficient" : "executed";
    auto reason = expected_effect ? "expected_effect_observed" : "expected_effect_missing";
    if (changed.empty()) reason = "no_source_evidence_changed";
    const auto source_hash = stream_hash(events);
    const auto transformed_hash = stream_hash(transformed);
    return {plan.plan_id, candidate.candidate_id, plan.kind, status, reason,
            source_hash, transformed_hash, before, after, expected_after,
            {changed.begin(), changed.end()}, std::move(transformed), expected_effect};
}

std::vector<SparseSensorCounterfactualExecution> execute_sparse_sensor_counterfactuals(
    const SensorDefinitionCandidate& candidate,
    const std::span<const ContinuousSensorEvent> events,
    const std::string_view replacement_term,
    const std::int64_t temporal_shift) {
    if (strip_unicode_whitespace(replacement_term).empty()) {
        throw std::invalid_argument("replacement term must be nonempty");
    }
    if (term_key(replacement_term) == term_key(candidate.term)) {
        throw std::invalid_argument("replacement term must differ from candidate term");
    }
    if (std::any_of(events.begin(), events.end(), [&](const auto& event) {
            return contains_term(event.screen_ocr, replacement_term) ||
                   contains_term(event.system_audio_transcript, replacement_term);
        })) {
        throw std::invalid_argument("replacement term already exists in source evidence");
    }
    if (events.empty() || temporal_shift % static_cast<std::int64_t>(events.size()) == 0) {
        throw std::invalid_argument("temporal shift must move at least one event");
    }
    for (const auto& plan : candidate.counterfactual_plans) {
        validate_candidate_lineage(candidate, plan, events);
    }
    const auto source_hash = stream_hash(events);
    std::vector<SparseSensorCounterfactualExecution> result;
    for (const auto& plan : candidate.counterfactual_plans) {
        result.push_back(sparse_execution(
            candidate, plan, events, source_hash, replacement_term, temporal_shift));
    }
    return result;
}

std::vector<ContinuousSensorEvent> materialize_sparse_sensor_counterfactual(
    const SparseSensorCounterfactualExecution& execution,
    const std::span<const ContinuousSensorEvent> events) {
    if (stream_hash(events) != execution.source_stream_hash) {
        throw std::invalid_argument("sparse execution source stream differs");
    }
    std::vector<ContinuousSensorEvent> result(events.begin(), events.end());
    for (const auto& patch : execution.patches) {
        if (patch.event_index >= events.size() ||
            continuous_sensor_event_content_hash(events[patch.event_index]) !=
                patch.source_content_hash) {
            throw std::invalid_argument("sparse patch source content differs");
        }
        result[patch.event_index] = with_content_address(
            events[patch.event_index], patch.screen_ocr,
            patch.system_audio_transcript, execution.plan_id);
    }
    return result;
}

EvidenceObservation counterfactual_execution_observation(
    const SensorCounterfactualExecution& execution,
    std::string source_family,
    const std::int64_t observed_at) {
    return make_observation(execution, std::move(source_family), observed_at);
}

EvidenceObservation counterfactual_execution_observation(
    const SparseSensorCounterfactualExecution& execution,
    std::string source_family,
    const std::int64_t observed_at) {
    return make_observation(execution, std::move(source_family), observed_at);
}

}  // namespace swegca::world
