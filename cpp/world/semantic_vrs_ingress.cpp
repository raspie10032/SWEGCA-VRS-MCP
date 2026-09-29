#include "world/semantic_vrs_ingress.hpp"

#include "swegca_architecture/sha256.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace swegca::world {
namespace {

[[noreturn]] void reject(const char* reason) { throw std::invalid_argument(reason); }

[[nodiscard]] bool text(const std::string_view value) noexcept {
    return std::ranges::any_of(value, [](const unsigned char byte) {
        return byte != ' ' && byte != '\t' && byte != '\r' && byte != '\n' &&
               byte != '\f' && byte != '\v';
    });
}

[[nodiscard]] bool sha256_text(const std::string_view value) noexcept {
    return value.size() == 64 && std::ranges::all_of(value, [](const char byte) {
        return (byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f');
    });
}

[[nodiscard]] bool valid_utf8(const std::string_view value) noexcept {
    std::size_t at = 0;
    while (at < value.size()) {
        const auto first = static_cast<unsigned char>(value[at++]);
        if (first <= 0x7f) continue;
        unsigned continuation = 0;
        std::uint32_t codepoint = 0;
        std::uint32_t minimum = 0;
        if (first >= 0xc2 && first <= 0xdf) {
            continuation = 1; codepoint = first & 0x1fU; minimum = 0x80;
        } else if (first >= 0xe0 && first <= 0xef) {
            continuation = 2; codepoint = first & 0x0fU; minimum = 0x800;
        } else if (first >= 0xf0 && first <= 0xf4) {
            continuation = 3; codepoint = first & 0x07U; minimum = 0x10000;
        } else {
            return false;
        }
        if (continuation > value.size() - at) return false;
        for (unsigned index = 0; index < continuation; ++index) {
            const auto next = static_cast<unsigned char>(value[at++]);
            if ((next & 0xc0U) != 0x80U) return false;
            codepoint = (codepoint << 6U) | (next & 0x3fU);
        }
        if (codepoint < minimum || codepoint > 0x10ffffU ||
            (codepoint >= 0xd800U && codepoint <= 0xdfffU)) return false;
    }
    return true;
}

[[nodiscard]] std::size_t utf8_length(const std::string_view value) {
    if (!valid_utf8(value)) reject("semantic string is not valid UTF-8");
    return static_cast<std::size_t>(std::ranges::count_if(value, [](const unsigned char byte) {
        return (byte & 0xc0U) != 0x80U;
    }));
}

void append_json_string(std::string& result, const std::string_view value) {
    if (!valid_utf8(value)) reject("semantic string is not valid UTF-8");
    static constexpr char hex[] = "0123456789abcdef";
    result.push_back('"');
    for (const auto raw : value) {
        const auto byte = static_cast<unsigned char>(raw);
        switch (byte) {
        case '"': result += "\\\""; break;
        case '\\': result += "\\\\"; break;
        case '\b': result += "\\b"; break;
        case '\t': result += "\\t"; break;
        case '\n': result += "\\n"; break;
        case '\f': result += "\\f"; break;
        case '\r': result += "\\r"; break;
        default:
            if (byte < 0x20U) {
                result += "\\u00";
                result.push_back(hex[byte >> 4U]);
                result.push_back(hex[byte & 0xfU]);
            } else {
                result.push_back(raw);
            }
        }
    }
    result.push_back('"');
}

[[nodiscard]] std::string python_float(const double value) {
    if (!std::isfinite(value)) reject("semantic JSON rejects non-finite real values");
    char buffer[128];
    const auto converted = std::to_chars(std::begin(buffer), std::end(buffer), value,
                                         std::chars_format::general);
    if (converted.ec != std::errc{}) reject("semantic binary64 conversion failed");
    std::string result(buffer, converted.ptr);
    const auto exponent_at = result.find('e');
    if (exponent_at == std::string::npos) {
        if (result.find('.') == std::string::npos) result += ".0";
        return result;
    }
    auto exponent_text = std::string_view(result).substr(exponent_at + 1);
    bool negative_exponent = false;
    if (!exponent_text.empty() &&
        (exponent_text.front() == '+' || exponent_text.front() == '-')) {
        negative_exponent = exponent_text.front() == '-';
        exponent_text.remove_prefix(1);
    }
    unsigned magnitude = 0;
    const auto parsed = std::from_chars(exponent_text.data(),
        exponent_text.data() + exponent_text.size(), magnitude);
    if (exponent_text.empty() || parsed.ec != std::errc{} ||
        parsed.ptr != exponent_text.data() + exponent_text.size() ||
        magnitude > static_cast<unsigned>(std::numeric_limits<int>::max()))
        reject("semantic binary64 exponent conversion failed");
    int exponent = static_cast<int>(magnitude);
    if (negative_exponent) exponent = -exponent;
    if (exponent >= -4 && exponent < 16) {
        const bool negative_value = result.front() == '-';
        const auto begin = negative_value ? 1U : 0U;
        std::string digits;
        for (std::size_t index = begin; index < exponent_at; ++index)
            if (result[index] != '.') digits.push_back(result[index]);
        const auto decimal = std::int64_t{1} + exponent;
        std::string fixed = negative_value ? "-" : "";
        if (decimal <= 0) {
            fixed += "0.";
            fixed.append(static_cast<std::size_t>(-decimal), '0');
            fixed += digits;
        } else if (static_cast<std::size_t>(decimal) >= digits.size()) {
            fixed += digits;
            fixed.append(static_cast<std::size_t>(decimal) - digits.size(), '0');
            fixed += ".0";
        } else {
            fixed.append(digits, 0, static_cast<std::size_t>(decimal));
            fixed.push_back('.');
            fixed.append(digits, static_cast<std::size_t>(decimal), std::string::npos);
        }
        return fixed;
    }
    std::string scientific = result.substr(0, exponent_at + 1);
    scientific.push_back(exponent < 0 ? '-' : '+');
    magnitude = static_cast<unsigned>(exponent < 0 ? -static_cast<long long>(exponent)
                                                    : exponent);
    char digits[32];
    const auto encoded = std::to_chars(std::begin(digits), std::end(digits), magnitude);
    const auto count = static_cast<std::size_t>(encoded.ptr - digits);
    if (count < 2) scientific.push_back('0');
    scientific.append(digits, encoded.ptr);
    return scientific;
}

void append_json(std::string& result, const JsonValue& value) {
    const auto& storage = value.storage();
    if (std::holds_alternative<std::nullptr_t>(storage)) {
        result += "null";
    } else if (const auto* item = std::get_if<bool>(&storage)) {
        result += *item ? "true" : "false";
    } else if (const auto* item = std::get_if<std::int64_t>(&storage)) {
        char buffer[32];
        const auto encoded = std::to_chars(std::begin(buffer), std::end(buffer), *item);
        if (encoded.ec != std::errc{}) reject("semantic integer conversion failed");
        result.append(buffer, encoded.ptr);
    } else if (const auto* item = std::get_if<double>(&storage)) {
        result += python_float(*item);
    } else if (const auto* item = std::get_if<std::string>(&storage)) {
        append_json_string(result, *item);
    } else if (const auto* items = std::get_if<JsonValue::Array>(&storage)) {
        result.push_back('[');
        for (std::size_t index = 0; index < items->size(); ++index) {
            if (index) result.push_back(',');
            append_json(result, (*items)[index]);
        }
        result.push_back(']');
    } else {
        const auto& object_items = std::get<JsonValue::Object>(storage);
        result.push_back('{');
        std::size_t index = 0;
        for (const auto& [key, item] : object_items) {
            if (index++) result.push_back(',');
            append_json_string(result, key);
            result.push_back(':');
            append_json(result, item);
        }
        result.push_back('}');
    }
}

[[nodiscard]] std::string canonical_json(const JsonValue& value) {
    std::string result;
    append_json(result, value);
    return result;
}

[[nodiscard]] std::string digest(const std::string_view value) {
    architecture::Sha256 hash;
    hash.update(value);
    const auto bytes = hash.finish();
    static constexpr char hex[] = "0123456789abcdef";
    std::string result;
    result.reserve(64);
    for (const auto byte : bytes) {
        const auto value_byte = std::to_integer<unsigned char>(byte);
        result.push_back(hex[value_byte >> 4U]);
        result.push_back(hex[value_byte & 0xfU]);
    }
    return result;
}

[[nodiscard]] JsonValue strings(const std::vector<std::string>& values) {
    JsonValue::Array result;
    result.reserve(values.size());
    for (const auto& value : values) result.emplace_back(value);
    return JsonValue(std::move(result));
}

[[nodiscard]] JsonValue integers(const std::vector<std::int64_t>& values) {
    JsonValue::Array result;
    result.reserve(values.size());
    for (const auto value : values) result.emplace_back(value);
    return JsonValue(std::move(result));
}

[[nodiscard]] JsonValue path_value(const std::vector<SemanticPathElement>& path) {
    JsonValue::Array result;
    result.reserve(path.size());
    for (const auto& element : path) {
        if (const auto* index = std::get_if<std::int64_t>(&element)) result.emplace_back(*index);
        else result.emplace_back(std::get<std::string>(element));
    }
    return JsonValue(std::move(result));
}

[[nodiscard]] JsonValue anchor_payload(const SemanticAnchor& anchor) {
    return JsonValue::Object{
        {"char_range", integers(anchor.char_range)},
        {"identifier", anchor.identifier},
        {"modality", anchor.modality},
        {"path", path_value(anchor.path)},
        {"region", integers(anchor.region)},
        {"role", anchor.role},
        {"step", anchor.step},
        {"time_ns", integers(anchor.time_ns)},
    };
}

[[nodiscard]] JsonValue qualifier_payload(const SemanticQualifier& qualifier,
                                          const bool include_anchors) {
    JsonValue::Object result{
        {"kind", qualifier.kind},
        {"value", qualifier.value},
    };
    if (include_anchors) result.emplace("anchors", strings(qualifier.anchors));
    return result;
}

[[nodiscard]] JsonValue unit_payload(const SemanticMeaningUnit& unit,
                                     const bool address_form) {
    JsonValue::Array qualifiers;
    qualifiers.reserve(unit.qualifiers.size());
    for (const auto& qualifier : unit.qualifiers)
        qualifiers.push_back(qualifier_payload(qualifier, !address_form));
    JsonValue::Object result{
        {"basis", unit.basis},
        {"polarity", unit.polarity},
        {"predicate", unit.predicate},
        {"qualifiers", JsonValue(std::move(qualifiers))},
        {"subject", unit.subject},
        {"value", unit.value},
    };
    if (!address_form) result.emplace("anchors", strings(unit.anchors));
    if (unit.value_kind != "unspecified") result.emplace("value_kind", unit.value_kind);
    return result;
}

[[nodiscard]] JsonValue encoding_receipt(const SemanticEncoding& encoding) {
    JsonValue::Array anchors;
    anchors.reserve(encoding.anchors.size());
    for (const auto& anchor : encoding.anchors) anchors.push_back(anchor_payload(anchor));
    JsonValue::Array units;
    units.reserve(encoding.units.size());
    for (const auto& unit : encoding.units) units.push_back(unit_payload(unit, false));
    JsonValue::Object result{
        {"anchors", JsonValue(std::move(anchors))},
        {"derived", true},
        {"independent_evidence_count", std::int64_t{0}},
        {"model", encoding.model},
        {"new_observation_count", std::int64_t{0}},
        {"outcomes", strings(encoding.outcomes)},
        {"persistent_write_authority", false},
        {"schema", std::string(semantic_encoding_schema)},
        {"semantic_authority", false},
        {"source_addresses", strings(encoding.source_addresses)},
        {"source_digest", encoding.source_digest},
        {"source_id", encoding.source_id},
        {"source_revision", encoding.source_revision},
        {"units", JsonValue(std::move(units))},
        {"unresolved", strings(encoding.unresolved)},
    };
    if (!encoding.partial_response_units.empty()) {
        JsonValue::Array values;
        values.reserve(encoding.partial_response_units.size());
        for (const auto value : encoding.partial_response_units) {
            if (value > static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max()))
                reject("invalid_partial_response_units");
            values.emplace_back(static_cast<std::int64_t>(value));
        }
        result.emplace("partial_response_units", JsonValue(std::move(values)));
    }
    if (!encoding.input_context.empty())
        result.emplace("input_context", JsonValue(encoding.input_context));
    return result;
}

[[nodiscard]] JsonValue source_payload(const SemanticSourceEpisode& source) {
    JsonValue::Array steps;
    steps.reserve(source.steps.size());
    for (const auto& step : source.steps) {
        steps.emplace_back(JsonValue::Object{
            {"evidence_refs", strings(step.evidence_refs)},
            {"judgment", step.judgment},
            {"observation", step.observation},
            {"outcome", step.outcome},
            {"phase", step.phase},
            {"relations", strings(step.relations)},
        });
    }
    return JsonValue::Object{
        {"cues", strings(source.cues)},
        {"revision", source.revision},
        {"source_addresses", strings(source.source_addresses)},
        {"source_id", source.episode_id},
        {"steps", JsonValue(std::move(steps))},
        {"verification_state", source.verification_state},
    };
}

[[nodiscard]] std::string address(const std::string_view kind,
                                  const std::string_view source_id,
                                  const std::string_view source_revision,
                                  const JsonValue& value) {
    JsonValue::Array scope{JsonValue(source_id), JsonValue(source_revision)};
    JsonValue::Array wire{JsonValue(std::move(scope)), value};
    return std::string(kind) + ':' + digest(canonical_json(JsonValue(std::move(wire))));
}

[[nodiscard]] const JsonValue& at_path(const SemanticSourceEpisode& source,
                                       const SemanticAnchor& anchor) {
    if (anchor.step < 0 || static_cast<std::size_t>(anchor.step) >= source.steps.size() ||
        anchor.path.empty()) reject("invalid_semantic_anchor");
    const JsonValue* value = &source.steps[static_cast<std::size_t>(anchor.step)].observation;
    for (const auto& element : anchor.path) {
        if (const auto* key = std::get_if<std::string>(&element)) {
            const auto* object = std::get_if<JsonValue::Object>(&value->storage());
            if (!object) reject("invalid_semantic_anchor_path");
            const auto found = object->find(*key);
            if (found == object->end()) reject("invalid_semantic_anchor_path");
            value = &found->second;
        } else {
            const auto index = std::get<std::int64_t>(element);
            const auto* array = std::get_if<JsonValue::Array>(&value->storage());
            if (!array || index < 0 || static_cast<std::size_t>(index) >= array->size())
                reject("invalid_semantic_anchor_path");
            value = &(*array)[static_cast<std::size_t>(index)];
        }
    }
    return *value;
}

[[nodiscard]] std::int64_t integer_member(const JsonValue::Object& object,
                                          const std::string_view key,
                                          const char* reason) {
    const auto found = object.find(key);
    if (found == object.end()) reject(reason);
    const auto* value = std::get_if<std::int64_t>(&found->second.storage());
    if (!value) reject(reason);
    return *value;
}

[[nodiscard]] bool valid_span(const std::vector<std::int64_t>& span,
                              const std::int64_t maximum) noexcept {
    return span.size() == 2 && span[0] >= 0 && span[0] < span[1] && span[1] <= maximum;
}

void validate_anchor(const SemanticSourceEpisode& source, const SemanticAnchor& anchor) {
    static const std::set<std::string_view> modalities{"text", "image", "audio", "video"};
    static const std::set<std::string_view> roles{
        "original", "tag", "caption", "subtitle", "summary"};
    if (!text(anchor.identifier) || !modalities.contains(anchor.modality) ||
        !roles.contains(anchor.role)) reject("invalid_semantic_anchor");
    const auto& value = at_path(source, anchor);
    if (anchor.modality == "text") {
        const auto* string = std::get_if<std::string>(&value.storage());
        if (!string || !valid_span(anchor.char_range,
                static_cast<std::int64_t>(utf8_length(*string))) ||
            !anchor.region.empty() || !anchor.time_ns.empty())
            reject("invalid_semantic_text_span");
        return;
    }
    if (!anchor.char_range.empty()) reject("media_descriptor_required");
    const auto* descriptor = std::get_if<JsonValue::Object>(&value.storage());
    if (!descriptor) reject("media_descriptor_required");
    const auto digest_item = descriptor->find("content_sha256");
    if (digest_item == descriptor->end()) reject("media_source_digest_required");
    const auto* content_digest = std::get_if<std::string>(&digest_item->second.storage());
    if (!content_digest || !sha256_text(*content_digest)) reject("media_source_digest_required");
    if (anchor.modality == "image" || anchor.modality == "video") {
        const auto width = integer_member(*descriptor, "width", "native_image_dimensions_required");
        const auto height = integer_member(*descriptor, "height", "native_image_dimensions_required");
        if (width <= 0 || height <= 0) reject("native_image_dimensions_required");
        if (!anchor.region.empty() &&
            (anchor.region.size() != 4 || anchor.region[0] < 0 || anchor.region[1] < 0 ||
             anchor.region[0] >= anchor.region[2] || anchor.region[2] > width ||
             anchor.region[1] >= anchor.region[3] || anchor.region[3] > height))
            reject("image_region_out_of_bounds");
    } else if (!anchor.region.empty()) {
        reject("audio_has_no_image_region");
    }
    if (anchor.modality == "audio" || anchor.modality == "video") {
        const auto duration = integer_member(*descriptor, "duration_ns",
                                             "integer_nanosecond_interval_required");
        if (!valid_span(anchor.time_ns, duration))
            reject("integer_nanosecond_interval_required");
    } else if (!anchor.time_ns.empty()) {
        reject("still_image_has_no_time_interval");
    }
}

void validate_references(const std::vector<std::string>& refs,
                         const std::unordered_set<std::string>& known) {
    if (refs.empty()) reject("invalid_semantic_anchor_reference");
    std::unordered_set<std::string> seen;
    for (const auto& ref : refs)
        if (!known.contains(ref) || !seen.insert(ref).second)
            reject("invalid_semantic_anchor_reference");
}

void validate_encoding(const SemanticEncoding& encoding,
                       const SemanticSourceEpisode& source) {
    if (encoding.source_id != source.episode_id ||
        encoding.source_revision != source.revision ||
        encoding.source_digest != semantic_source_digest(source) ||
        encoding.source_addresses != source.source_addresses || !text(encoding.model))
        reject("semantic_source_binding_changed");
    std::vector<std::string> outcomes;
    outcomes.reserve(source.steps.size());
    for (const auto& step : source.steps) outcomes.push_back(step.outcome);
    if (encoding.outcomes != outcomes) reject("semantic_source_binding_changed");
    if (encoding.anchors.empty()) reject("unique_semantic_anchors_required");
    std::unordered_set<std::string> known;
    for (const auto& anchor : encoding.anchors) {
        if (!known.insert(anchor.identifier).second) reject("unique_semantic_anchors_required");
        validate_anchor(source, anchor);
    }
    std::unordered_set<std::string> covered;
    static const std::set<std::string_view> polarities{"affirmed", "denied", "unknown"};
    static const std::set<std::string_view> bases{"observed", "reported", "inferred"};
    static const std::set<std::string_view> value_kinds{"unspecified", "literal", "entity"};
    static const std::set<std::string_view> qualifier_kinds{"time", "condition", "location"};
    for (const auto& unit : encoding.units) {
        if (!text(unit.subject) || !text(unit.predicate) ||
            !polarities.contains(unit.polarity) || !bases.contains(unit.basis) ||
            !value_kinds.contains(unit.value_kind)) reject("invalid_semantic_unit_roles");
        if (unit.value_kind == "entity") {
            const auto* entity = std::get_if<std::string>(&unit.value.storage());
            if (!entity || !text(*entity)) reject("invalid_semantic_value_kind");
        }
        if (const auto* real = std::get_if<double>(&unit.value.storage()); real && !std::isfinite(*real))
            reject("invalid_semantic_value");
        if (unit.value.is_array()) reject("invalid_semantic_value");
        if (unit.value.is_object() && unit.value_kind != "literal")
            reject("semantic_table_requires_literal_value_kind");
        validate_references(unit.anchors, known);
        covered.insert(unit.anchors.begin(), unit.anchors.end());
        for (const auto& qualifier : unit.qualifiers) {
            if (!qualifier_kinds.contains(qualifier.kind) || !text(qualifier.value))
                reject("invalid_semantic_qualifier");
            validate_references(qualifier.anchors, known);
            covered.insert(qualifier.anchors.begin(), qualifier.anchors.end());
        }
    }
    if (!encoding.unresolved.empty()) validate_references(encoding.unresolved, known);
    covered.insert(encoding.unresolved.begin(), encoding.unresolved.end());
    if (covered != known) reject("semantic_source_part_silently_omitted");
    std::size_t previous = 0;
    bool first = true;
    const std::unordered_set<std::string> unresolved(
        encoding.unresolved.begin(), encoding.unresolved.end());
    for (const auto unit_index : encoding.partial_response_units) {
        if (unit_index >= encoding.units.size() || (!first && unit_index <= previous))
            reject("invalid_partial_response_units");
        first = false;
        previous = unit_index;
        std::unordered_set<std::string> refs(
            encoding.units[unit_index].anchors.begin(), encoding.units[unit_index].anchors.end());
        for (const auto& qualifier : encoding.units[unit_index].qualifiers)
            refs.insert(qualifier.anchors.begin(), qualifier.anchors.end());
        if (!std::ranges::all_of(refs, [&](const auto& ref) { return unresolved.contains(ref); }))
            reject("invalid_partial_response_units");
    }
}

[[nodiscard]] std::vector<std::string> reference_order(const SemanticMeaningUnit& unit) {
    std::vector<std::string> result;
    std::unordered_set<std::string> seen;
    const auto add = [&](const std::string& ref) {
        if (seen.insert(ref).second) result.push_back(ref);
    };
    for (const auto& ref : unit.anchors) add(ref);
    for (const auto& qualifier : unit.qualifiers)
        for (const auto& ref : qualifier.anchors) add(ref);
    return result;
}

void validate_source(const SemanticSourceEpisode& source) {
    if (!text(source.episode_id) || !text(source.revision) ||
        !text(source.verification_state) || source.cues.empty() ||
        source.steps.empty() || source.source_addresses.empty())
        reject("memory episode is incomplete");
    std::unordered_set<std::string> addresses;
    for (const auto& item : source.source_addresses)
        if (!text(item) || !addresses.insert(item).second)
            reject("memory source addresses must be unique");
    static const std::set<std::string_view> outcomes{
        "success", "failure", "negative", "uncertain", "conflict", "pending"};
    for (const auto& step : source.steps)
        if (!text(step.phase) || !text(step.judgment) ||
            !outcomes.contains(step.outcome) || step.evidence_refs.empty() ||
            !std::ranges::all_of(step.evidence_refs, [](const auto& ref) { return text(ref); }))
            reject("invalid semantic source episode");
}

}  // namespace

std::string semantic_source_digest(const SemanticSourceEpisode& source) {
    validate_source(source);
    return digest(canonical_json(source_payload(source)));
}

std::string semantic_encoding_episode_id(const SemanticEncoding& encoding) {
    return "semantic-encoding:" + digest(canonical_json(encoding_receipt(encoding)));
}

std::string semantic_scoped_address(const std::string_view kind,
                                    const JsonValue& scope,
                                    const JsonValue& value) {
    if (kind.empty()) reject("semantic address kind required");
    JsonValue::Array wire{scope, value};
    return std::string(kind) + ':' + digest(canonical_json(JsonValue(std::move(wire))));
}

std::string semantic_anchor_address(const std::string_view source_id,
                                    const std::string_view source_revision,
                                    const SemanticAnchor& anchor) {
    return address("semantic-anchor", source_id, source_revision, anchor_payload(anchor));
}

std::string semantic_anchor_address(const JsonValue& scope,
                                    const SemanticAnchor& anchor) {
    return semantic_scoped_address("semantic-anchor", scope, anchor_payload(anchor));
}

std::string semantic_unit_address(const std::string_view source_id,
                                  const std::string_view source_revision,
                                  const SemanticMeaningUnit& unit) {
    return address("semantic-unit", source_id, source_revision, unit_payload(unit, true));
}

std::string semantic_unit_address(const JsonValue& scope,
                                  const SemanticMeaningUnit& unit) {
    return semantic_scoped_address("semantic-unit", scope, unit_payload(unit, true));
}

std::vector<std::vector<SemanticGraphCoordinate>> prepare_claim_graph_addresses(
    const SemanticEncoding& encoding, const std::string_view derivative_id) {
    std::unordered_map<std::string, std::string> anchors;
    for (const auto& anchor : encoding.anchors)
        anchors.emplace(anchor.identifier, semantic_anchor_address(
            encoding.source_id, encoding.source_revision, anchor));
    std::vector<std::vector<SemanticGraphCoordinate>> rows;
    rows.reserve(encoding.units.size());
    for (const auto& unit : encoding.units) {
        const auto unit_address = semantic_unit_address(
            encoding.source_id, encoding.source_revision, unit);
        std::vector<SemanticGraphCoordinate> links{
            {"source_derivation", encoding.source_id, std::string(derivative_id)},
            {"encoding_unit", std::string(derivative_id), unit_address},
        };
        for (const auto& ref : reference_order(unit)) {
            const auto found = anchors.find(ref);
            if (found == anchors.end()) reject("invalid_semantic_anchor_reference");
            links.push_back({"unit_anchor_proposal", unit_address, found->second});
            links.push_back({"anchor_source_location", found->second, encoding.source_id});
        }
        rows.push_back(std::move(links));
    }
    return rows;
}

SemanticGraphDelta::SemanticGraphDelta(
    std::shared_ptr<const TermAddressIndex> address_index_value,
    const std::size_t edge_start_value,
    std::vector<std::string> appended_terms_value,
    std::vector<EventSignalEdge> edge_rows_value,
    std::vector<SemanticGraphReceipt> records_value)
    : address_index(std::move(address_index_value)),
      edge_start(edge_start_value),
      appended_terms(std::move(appended_terms_value)),
      edge_rows(std::move(edge_rows_value)),
      records_(std::move(records_value)) {
    if (!address_index) reject("unverified term address index");
}

SemanticGraphDelta prepare_semantic_graph_delta(
    std::shared_ptr<const TermAddressIndex> address_index,
    const std::size_t edge_count,
    const std::vector<SemanticSourceEpisode>& episodes,
    const std::vector<SemanticEncoding>& proposals) {
    if (!address_index) reject("unverified term address index");
    std::unordered_map<std::string, const SemanticSourceEpisode*> sources;
    for (const auto& source : episodes) {
        validate_source(source);
        sources[source.episode_id] = &source;
    }
    if (address_index->size() > std::numeric_limits<std::uint32_t>::max())
        reject("semantic graph exceeds uint32 addressing");
    std::vector<std::string> appended_terms;
    std::vector<EventSignalEdge> edge_rows;
    std::vector<SemanticGraphReceipt> records;
    std::unordered_map<std::string, std::uint32_t> new_nodes;
    const auto node = [&](const std::string& term) -> std::uint32_t {
        if (const auto existing = address_index->lookup(term)) {
            if (*existing > std::numeric_limits<std::uint32_t>::max())
                reject("semantic graph exceeds uint32 addressing");
            return static_cast<std::uint32_t>(*existing);
        }
        if (const auto found = new_nodes.find(term); found != new_nodes.end())
            return found->second;
        if (appended_terms.size() >
            std::numeric_limits<std::uint32_t>::max() - address_index->size())
            reject("semantic graph exceeds uint32 addressing");
        const auto value = static_cast<std::uint32_t>(
            address_index->size() + appended_terms.size());
        new_nodes.emplace(term, value);
        appended_terms.push_back(term);
        return value;
    };

    for (const auto& proposal : proposals) {
        const auto found_source = sources.find(proposal.source_id);
        if (found_source == sources.end()) reject("semantic_original_required_in_same_wave");
        const auto& source = *found_source->second;
        validate_encoding(proposal, source);
        const auto derivative = semantic_encoding_episode_id(proposal);
        if (address_index->contains(derivative) || new_nodes.contains(derivative))
            reject("semantic_derivative_already_in_graph");
        const auto source_node = address_index->lookup(source.episode_id);
        if (!source_node) reject("semantic_original_required_in_same_graph");
        const auto root = node(derivative);
        std::vector<std::pair<std::string, std::uint32_t>> anchors;
        std::unordered_map<std::string, std::uint32_t> anchor_nodes;
        anchors.reserve(proposal.anchors.size());
        for (const auto& anchor : proposal.anchors) {
            const auto value = node(semantic_anchor_address(
                source.episode_id, source.revision, anchor));
            anchors.emplace_back(anchor.identifier, value);
            anchor_nodes.emplace(anchor.identifier, value);
        }
        if (edge_rows.size() > std::numeric_limits<std::size_t>::max() - edge_count)
            throw std::overflow_error("semantic edge start overflow");
        const auto start = edge_count + edge_rows.size();
        std::vector<SemanticEdgeRole> roles;
        std::set<std::tuple<std::uint32_t, std::uint32_t, std::string>> seen;
        const auto link = [&](const std::uint32_t left, const std::uint32_t right,
                              std::string kind, std::optional<std::string> anchor_id = {},
                              std::optional<std::size_t> unit_index = {}) {
            if (!seen.emplace(left, right, kind).second) return;
            edge_rows.push_back({left, right, 1, .75F});
            roles.push_back({std::move(kind), left, right,
                             std::move(anchor_id), unit_index});
        };
        link(static_cast<std::uint32_t>(*source_node), root, "source_derivation");
        for (const auto& [identifier, anchor_node] : anchors)
            link(anchor_node, static_cast<std::uint32_t>(*source_node),
                 "anchor_source_location", identifier);
        std::vector<std::uint32_t> unit_nodes;
        unit_nodes.reserve(proposal.units.size());
        for (std::size_t ordinal = 0; ordinal < proposal.units.size(); ++ordinal) {
            const auto& unit = proposal.units[ordinal];
            const auto unit_node = node(semantic_unit_address(
                source.episode_id, source.revision, unit));
            unit_nodes.push_back(unit_node);
            link(root, unit_node, "encoding_unit", {}, ordinal);
            for (const auto& ref : reference_order(unit)) {
                const auto anchor = anchor_nodes.find(ref);
                if (anchor == anchor_nodes.end()) reject("invalid_semantic_anchor_reference");
                link(unit_node, anchor->second, "unit_anchor_proposal", ref, ordinal);
            }
        }
        for (const auto& ref : proposal.unresolved) {
            const auto anchor = anchor_nodes.find(ref);
            if (anchor == anchor_nodes.end()) reject("invalid_semantic_anchor_reference");
            link(root, anchor->second, "unresolved_anchor", ref);
        }
        records.push_back({proposal, derivative, root, std::move(anchors),
                           std::move(unit_nodes), start, std::move(roles)});
    }
    return SemanticGraphDelta(address_index, edge_count, std::move(appended_terms),
                              std::move(edge_rows), std::move(records));
}

}  // namespace swegca::world
