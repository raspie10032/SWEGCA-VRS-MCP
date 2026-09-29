#include "world/sensor_term_index.hpp"

#include "swegca_architecture/sha256.hpp"
#include "world/unicode_nfkc.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace swegca::world {
namespace {

std::vector<char32_t> decode_normalized(const std::string_view input) {
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

std::string key_of(std::string value) {
    for (auto& byte : value) {
        if (byte >= 'A' && byte <= 'Z') byte = static_cast<char>(byte + ('a' - 'A'));
    }
    return value;
}

std::size_t unicode_size(const std::string_view value) {
    return static_cast<std::size_t>(std::count_if(value.begin(), value.end(), [](char byte) {
        return (static_cast<unsigned char>(byte) & 0xc0U) != 0x80U;
    }));
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
            } else {
                result.push_back(raw);
            }
        }
    }
    result.push_back('"');
    return result;
}

std::string json_strings(const std::span<const std::string> values) {
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

std::string extend_hash(const std::string& root, const IndexedSensorTerms& event) {
    return digest("[" + json_string(root) + "," + json_string(event.evidence_address) +
        "," + json_string(event.content_hash) + "," + json_strings(event.screen_terms) +
        "," + json_strings(event.audio_terms) + "]");
}

std::string complete_hash(const std::span<const IndexedSensorTerms> events) {
    auto root = digest("[\"sensor-term-index-v1\"]");
    for (const auto& event : events) root = extend_hash(root, event);
    return root;
}

bool candidate_less(const OcrTermCandidate& left, const OcrTermCandidate& right) {
    if (left.observation_indices.size() != right.observation_indices.size()) {
        return left.observation_indices.size() > right.observation_indices.size();
    }
    const auto left_size = unicode_size(left.value);
    const auto right_size = unicode_size(right.value);
    if (left_size != right_size) return left_size > right_size;
    return key_of(left.value) < key_of(right.value);
}

std::vector<OcrTermCandidate> aggregate(
    const std::span<const IndexedSensorTerms> events,
    const bool screen) {
    std::map<std::string, std::set<std::size_t>> occurrences;
    std::map<std::string, std::string> display;
    for (std::size_t index = 0; index != events.size(); ++index) {
        const auto& values = screen ? events[index].screen_terms : events[index].audio_terms;
        for (const auto& value : values) {
            const auto key = key_of(value);
            occurrences[key].insert(index);
            display.try_emplace(key, value);
        }
    }
    std::vector<OcrTermCandidate> result;
    result.reserve(occurrences.size());
    for (const auto& [key, indices] : occurrences) {
        result.push_back({display.at(key), {indices.begin(), indices.end()}});
    }
    std::sort(result.begin(), result.end(), candidate_less);
    return result;
}

std::vector<OcrTermCandidate> append_candidates(
    const std::span<const OcrTermCandidate> candidates,
    const std::span<const std::string> terms,
    const std::size_t event_index) {
    std::map<std::string, OcrTermCandidate> by_key;
    for (const auto& candidate : candidates) by_key.emplace(key_of(candidate.value), candidate);
    std::set<std::string> seen;
    for (const auto& value : terms) {
        const auto key = key_of(value);
        if (!seen.insert(key).second) continue;
        const auto iterator = by_key.find(key);
        if (iterator == by_key.end()) by_key.emplace(key, OcrTermCandidate{value, {event_index}});
        else iterator->second.observation_indices.push_back(event_index);
    }
    std::vector<OcrTermCandidate> result;
    result.reserve(by_key.size());
    for (auto& [key, candidate] : by_key) {
        static_cast<void>(key);
        result.push_back(std::move(candidate));
    }
    std::sort(result.begin(), result.end(), candidate_less);
    return result;
}

using EventKey = std::pair<std::string, std::string>;

EventKey event_key(const ContinuousSensorEvent& event) {
    return {event.evidence_address, continuous_sensor_event_content_hash(event)};
}

}  // namespace

std::vector<std::string> extract_ocr_terms(const std::string_view text) {
    const auto normalized = normalize_nfkc(text);
    const auto points = decode_normalized(normalized);
    std::vector<std::string> result;
    for (std::size_t at = 0; at < points.size();) {
        if (points[at] >= U'가' && points[at] <= U'힣') {
            std::size_t size = 1;
            while (size < 20 && at + size < points.size() &&
                   points[at + size] >= U'가' && points[at + size] <= U'힣') ++size;
            if (size >= 2) {
                auto term = encode_range(points, at, size);
                constexpr std::string_view suffix = "입니다";
                if (size > 5 && term.ends_with(suffix)) term.resize(term.size() - suffix.size());
                result.push_back(std::move(term));
                at += size;
                continue;
            }
        } else if (ascii_letter(points[at])) {
            std::size_t size = 1;
            while (size < 32 && at + size < points.size() &&
                   ascii_continuation(points[at + size])) ++size;
            if (size >= 3) {
                result.push_back(encode_range(points, at, size));
                at += size;
                continue;
            }
        }
        ++at;
    }
    return result;
}

SensorTermIndexUpdate update_sensor_term_index(
    const SensorTermIndex* previous,
    const std::span<const ContinuousSensorEvent> events) {
    std::map<EventKey, IndexedSensorTerms> previous_by_key;
    if (previous != nullptr) {
        for (const auto& item : previous->indexed_events) {
            previous_by_key[{item.evidence_address, item.content_hash}] = item;
        }
    }
    std::set<EventKey> current_keys;
    std::vector<IndexedSensorTerms> indexed;
    std::vector<std::uintptr_t> object_ids;
    indexed.reserve(events.size());
    object_ids.reserve(events.size());
    std::size_t reused = 0;
    std::size_t tokenized = 0;
    for (const auto& event : events) {
        const auto key = event_key(event);
        current_keys.insert(key);
        const auto cached = previous_by_key.find(key);
        if (cached != previous_by_key.end()) {
            indexed.push_back(cached->second);
            ++reused;
        } else {
            indexed.push_back({event.evidence_address, key.second,
                extract_ocr_terms(event.screen_ocr),
                extract_ocr_terms(event.system_audio_transcript)});
            ++tokenized;
        }
        object_ids.push_back(reinterpret_cast<std::uintptr_t>(&event));
    }
    std::size_t expired = 0;
    for (const auto& [key, item] : previous_by_key) {
        static_cast<void>(item);
        if (!current_keys.contains(key)) ++expired;
    }
    auto screen = aggregate(indexed, true);
    auto audio = aggregate(indexed, false);
    auto hash = complete_hash(indexed);
    return {{std::move(indexed), std::move(screen), std::move(audio),
             std::move(hash), std::move(object_ids)}, reused, tokenized, expired};
}

SensorTermIndexUpdate append_sensor_term_index(
    const SensorTermIndex& previous,
    const ContinuousSensorEvent& event) {
    if (std::any_of(previous.indexed_events.begin(), previous.indexed_events.end(),
                    [&](const auto& item) { return item.evidence_address == event.evidence_address; })) {
        throw std::invalid_argument("sensor event address already exists in term index");
    }
    const auto key = event_key(event);
    IndexedSensorTerms indexed{event.evidence_address, key.second,
        extract_ocr_terms(event.screen_ocr),
        extract_ocr_terms(event.system_audio_transcript)};
    const auto event_index = previous.indexed_events.size();
    auto events = previous.indexed_events;
    events.push_back(indexed);
    auto object_ids = previous.event_object_ids;
    object_ids.push_back(reinterpret_cast<std::uintptr_t>(&event));
    SensorTermIndex result{
        std::move(events),
        append_candidates(previous.screen_candidates, indexed.screen_terms, event_index),
        append_candidates(previous.audio_candidates, indexed.audio_terms, event_index),
        extend_hash(previous.index_hash, indexed),
        std::move(object_ids)};
    return {std::move(result), previous.indexed_events.size(), 1, 0};
}

void validate_sensor_term_index(
    const SensorTermIndex& index,
    const std::span<const ContinuousSensorEvent> events) {
    std::vector<std::uintptr_t> object_ids;
    object_ids.reserve(events.size());
    for (const auto& event : events) object_ids.push_back(reinterpret_cast<std::uintptr_t>(&event));
    if (index.event_object_ids == object_ids) return;
    if (index.indexed_events.size() != events.size()) {
        throw std::invalid_argument("sensor term index differs from current event window");
    }
    for (std::size_t at = 0; at != events.size(); ++at) {
        const auto actual = event_key(events[at]);
        const auto expected = EventKey{index.indexed_events[at].evidence_address,
                                       index.indexed_events[at].content_hash};
        if (actual != expected) {
            throw std::invalid_argument("sensor term index differs from current event window");
        }
    }
}

std::vector<OcrTermCandidate> indexed_term_candidates(
    const SensorTermIndex& index,
    const std::string_view modality,
    const std::size_t minimum_observations,
    const std::size_t maximum_candidates) {
    if (minimum_observations == 0 || maximum_candidates == 0) {
        throw std::invalid_argument("candidate limits must be positive");
    }
    const std::vector<OcrTermCandidate>* candidates = nullptr;
    if (modality == "screen_ocr") candidates = &index.screen_candidates;
    else if (modality == "system_audio_transcript") candidates = &index.audio_candidates;
    else throw std::invalid_argument("unsupported indexed sensor modality");
    std::vector<OcrTermCandidate> result;
    for (const auto& candidate : *candidates) {
        if (candidate.observation_indices.size() >= minimum_observations) {
            result.push_back(candidate);
            if (result.size() == maximum_candidates) break;
        }
    }
    return result;
}

}  // namespace swegca::world
