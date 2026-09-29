#include "world/hypothesis_proposer.hpp"

#include "swegca_architecture/sha256.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
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
            throw std::invalid_argument("hypothesis JSON contains invalid UTF-8");
        }
        if (count > value.size() - at) {
            throw std::invalid_argument("hypothesis JSON contains invalid UTF-8");
        }
        for (std::size_t offset = 1; offset != count; ++offset) {
            const auto continuation = static_cast<unsigned char>(value[at + offset]);
            if ((continuation & 0xc0U) != 0x80U) {
                throw std::invalid_argument("hypothesis JSON contains invalid UTF-8");
            }
            point = (point << 6U) | (continuation & 0x3fU);
        }
        if (point < minimum || point > 0x10ffffU ||
            (point >= 0xd800U && point <= 0xdfffU)) {
            throw std::invalid_argument("hypothesis JSON contains invalid UTF-8");
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
    if (error != std::errc{}) throw std::runtime_error("cannot encode hypothesis number");
    std::string result(storage, end);
    if (result.find_first_of(".eE") == std::string::npos) result += ".0";
    return result;
}

std::string canonical_json(const JsonValue& value) {
    const auto& storage = value.storage();
    if (std::holds_alternative<std::nullptr_t>(storage)) return "null";
    if (const auto* item = std::get_if<bool>(&storage)) return *item ? "true" : "false";
    if (const auto* item = std::get_if<std::int64_t>(&storage)) return std::to_string(*item);
    if (const auto* item = std::get_if<double>(&storage)) return python_float(*item);
    if (const auto* item = std::get_if<std::string>(&storage)) return json_string(*item);
    if (const auto* items = std::get_if<JsonValue::Array>(&storage)) {
        std::string result{"["};
        for (std::size_t index = 0; index != items->size(); ++index) {
            if (index != 0) result.push_back(',');
            result += canonical_json((*items)[index]);
        }
        result.push_back(']');
        return result;
    }
    const auto& items = std::get<JsonValue::Object>(storage);
    std::string result{"{"};
    bool first = true;
    for (const auto& [key, item] : items) {
        if (!first) result.push_back(',');
        first = false;
        result += json_string(key) + ":" + canonical_json(item);
    }
    result.push_back('}');
    return result;
}

std::string hex(const architecture::DigestBytes& digest) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result(digest.size() * 2, '0');
    for (std::size_t index = 0; index != digest.size(); ++index) {
        const auto byte = std::to_integer<unsigned>(digest[index]);
        result[index * 2] = digits[byte >> 4U];
        result[index * 2 + 1] = digits[byte & 15U];
    }
    return result;
}

std::string hypothesis_id(const std::optional<std::string>& subject,
                          const std::string& predicate,
                          const JsonValue& value) {
    JsonValue::Array payload;
    payload.emplace_back(subject ? JsonValue(*subject) : JsonValue(nullptr));
    payload.emplace_back(predicate);
    payload.push_back(value);
    const auto canonical = canonical_json(payload);
    architecture::Sha256 digest;
    digest.update(canonical);
    return "hypothesis:" + hex(digest.finish()).substr(0, 20);
}

void insert_unique(std::vector<std::string>& values, const std::string_view value) {
    if (std::find(values.begin(), values.end(), value) == values.end()) {
        values.emplace_back(value);
    }
}

struct Group final {
    std::optional<std::string> subject;
    std::string predicate;
    std::string canonical_value;
    JsonValue value;
    std::vector<double> confidences;
    std::vector<std::string> evidence_refs;
    std::vector<std::string> source_families;
    std::vector<std::string> source_event_ids;
};

struct Competing final {
    std::optional<std::string> subject;
    std::string predicate;
    std::vector<std::pair<std::string, JsonValue>> values;
};

}  // namespace

HypothesisProposerConfig::HypothesisProposerConfig(
    const double minimum_claim_confidence_value,
    const std::uint64_t minimum_source_diversity_value,
    const double conflict_score_multiplier_value,
    const std::size_t maximum_candidates_value,
    const std::size_t maximum_requested_axes_value,
    std::vector<std::string> ignored_predicates_value,
    std::vector<std::pair<std::string, std::string>> axis_request_kinds_value)
    : minimum_claim_confidence(minimum_claim_confidence_value),
      minimum_source_diversity(minimum_source_diversity_value),
      conflict_score_multiplier(conflict_score_multiplier_value),
      maximum_candidates(maximum_candidates_value),
      maximum_requested_axes(maximum_requested_axes_value),
      ignored_predicates(std::move(ignored_predicates_value)),
      axis_request_kinds(std::move(axis_request_kinds_value)) {
    if (!std::isfinite(minimum_claim_confidence) ||
        minimum_claim_confidence < 0.0 || minimum_claim_confidence > 1.0) {
        throw std::invalid_argument("minimum claim confidence must be in [0, 1]");
    }
    if (minimum_source_diversity == 0) {
        throw std::invalid_argument("minimum source diversity must be positive");
    }
    if (!std::isfinite(conflict_score_multiplier) ||
        conflict_score_multiplier < 0.0 || conflict_score_multiplier > 1.0) {
        throw std::invalid_argument("conflict multiplier must be in [0, 1]");
    }
    if (maximum_candidates == 0 || maximum_requested_axes == 0) {
        throw std::invalid_argument("candidate and request limits must be positive");
    }
    if (axis_request_kinds.empty()) {
        throw std::invalid_argument("axis request policy must not be empty");
    }
    std::set<std::string> axes;
    for (const auto& [axis, kind] : axis_request_kinds) {
        static_cast<void>(kind);
        if (!axes.insert(axis).second) {
            throw std::invalid_argument("axis request policy contains duplicates");
        }
    }
}

std::vector<HypothesisCandidate> propose_hypotheses(
    const std::span<const CognitiveEvent> events,
    const HypothesisProposerConfig& config) {
    std::vector<Group> groups;
    std::vector<Competing> competing;
    for (const auto& event : events) {
        for (const auto& claim : event.claims()) {
            if (std::find(config.ignored_predicates.begin(),
                          config.ignored_predicates.end(), claim.predicate) !=
                    config.ignored_predicates.end() ||
                claim.confidence < config.minimum_claim_confidence) {
                continue;
            }
            const auto canonical = canonical_json(claim.value);
            auto group = std::find_if(groups.begin(), groups.end(), [&](const Group& item) {
                return item.subject == claim.subject && item.predicate == claim.predicate &&
                       item.canonical_value == canonical;
            });
            if (group == groups.end()) {
                groups.push_back({claim.subject, claim.predicate, canonical, claim.value,
                                  {}, {}, {}, {}});
                group = std::prev(groups.end());
            }
            group->confidences.push_back(claim.confidence);
            for (const auto& reference : event.evidence_refs()) {
                insert_unique(group->evidence_refs, reference);
            }
            insert_unique(group->source_families, event.source().representation);
            insert_unique(group->source_event_ids, event.event_id());

            auto alternatives = std::find_if(
                competing.begin(), competing.end(), [&](const Competing& item) {
                    return item.subject == claim.subject && item.predicate == claim.predicate;
                });
            if (alternatives == competing.end()) {
                competing.push_back({claim.subject, claim.predicate, {}});
                alternatives = std::prev(competing.end());
            }
            if (std::none_of(alternatives->values.begin(), alternatives->values.end(),
                             [&](const auto& item) { return item.first == canonical; })) {
                alternatives->values.emplace_back(canonical, claim.value);
            }
        }
    }

    std::vector<HypothesisCandidate> result;
    result.reserve(groups.size());
    for (const auto& group : groups) {
        const auto alternatives = std::find_if(
            competing.begin(), competing.end(), [&](const Competing& item) {
                return item.subject == group.subject && item.predicate == group.predicate;
            });
        std::vector<JsonValue> conflicts;
        for (const auto& [canonical, value] : alternatives->values) {
            if (canonical != group.canonical_value) conflicts.push_back(value);
        }
        double confidence = 0.0;
        for (const auto value : group.confidences) confidence += value;
        confidence /= static_cast<double>(group.confidences.size());
        confidence *= std::min(1.0,
            static_cast<double>(group.source_families.size()) /
                static_cast<double>(config.minimum_source_diversity));
        if (!conflicts.empty()) confidence *= config.conflict_score_multiplier;
        result.push_back({hypothesis_id(group.subject, group.predicate, group.value),
                          group.subject, group.predicate, group.value, confidence,
                          group.evidence_refs, group.source_families,
                          group.source_event_ids, std::move(conflicts)});
    }
    std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
        if (left.confidence != right.confidence) return left.confidence > right.confidence;
        if (left.has_conflict() != right.has_conflict()) {
            return !left.has_conflict();
        }
        return left.hypothesis_id < right.hypothesis_id;
    });
    if (result.size() > config.maximum_candidates) result.resize(config.maximum_candidates);
    return result;
}

EvidenceRequestPlan plan_evidence_request(
    const HypothesisCandidate& candidate,
    const EvidenceAccumulatorState* accumulator,
    const EvidenceAccumulatorConfig& accumulator_config,
    const HypothesisProposerConfig& proposer_config,
    const bool external_refutation) {
    std::map<std::string, std::string, std::less<>> policy;
    for (const auto& [axis, kind] : proposer_config.axis_request_kinds) {
        policy.emplace(axis, kind);
    }
    for (const auto& axis : accumulator_config.required_axes) {
        if (!policy.contains(axis)) {
            throw std::invalid_argument("missing request policy for axis: " + axis);
        }
    }
    std::map<std::string, double, std::less<>> samples;
    for (const auto& axis : accumulator_config.required_axes) samples[axis] = 0.0;
    if (accumulator != nullptr) {
        if (accumulator->hypothesis_id() != candidate.hypothesis_id) {
            throw std::invalid_argument("candidate and accumulator hypothesis differ");
        }
        for (const auto& axis : accumulator->axes()) {
            samples[axis.name] = axis.effective_samples();
        }
    }
    std::vector<std::size_t> ranked(accumulator_config.required_axes.size());
    for (std::size_t index = 0; index != ranked.size(); ++index) ranked[index] = index;
    std::stable_sort(ranked.begin(), ranked.end(), [&](const auto left, const auto right) {
        const auto key = [&](const std::size_t index) {
            const auto& axis = accumulator_config.required_axes[index];
            const int priority = (candidate.has_conflict() || external_refutation) &&
                    axis == "counterfactual" &&
                    samples[axis] < static_cast<double>(
                        accumulator_config.minimum_effective_samples_per_axis)
                ? 0 : 1;
            return std::tuple(priority, samples[axis], index);
        };
        return key(left) < key(right);
    });
    const auto count = std::min(ranked.size(), proposer_config.maximum_requested_axes);
    EvidenceRequestPlan plan;
    plan.hypothesis_id = candidate.hypothesis_id;
    for (std::size_t output = 0; output != count; ++output) {
        const auto& axis = accumulator_config.required_axes[ranked[output]];
        plan.requested_axes.push_back(axis);
        plan.request_kinds.push_back(policy.at(axis));
    }
    if (external_refutation) plan.reason = "resolve_external_refutation";
    else if (candidate.has_conflict()) plan.reason = "resolve_conflicting_values";
    else if (candidate.source_families.size() < proposer_config.minimum_source_diversity)
        plan.reason = "increase_source_diversity";
    else plan.reason = "fill_weakest_evidence_axes";
    return plan;
}

}  // namespace swegca::world
