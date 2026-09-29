#include "world/semantic_speech_value.hpp"

#include "world/unicode_nfkc.hpp"

#include <algorithm>
#include <set>
#include <stdexcept>

namespace swegca::world {
namespace {

const std::set<std::string, std::less<>> kinds{
    "assertion", "question", "request", "suggestion", "greeting", "promise",
    "disbelief", "quotation", "other", "unknown"};

bool text(const std::string_view value) { return !strip_unicode_whitespace(value).empty(); }

std::vector<std::string> references(
    const JsonValue& value, const std::set<std::string, std::less<>>& known) {
    if (!value.is_array() || value.as_array().empty())
        throw std::invalid_argument("speech_anchor_outside_enclosing_unit");
    std::vector<std::string> result;
    for (const auto& row : value.as_array()) {
        if (!std::holds_alternative<std::string>(row.storage()))
            throw std::invalid_argument("speech_anchor_outside_enclosing_unit");
        auto identifier = std::string(row.as_string());
        if (!known.contains(identifier) || std::ranges::find(result, identifier) != result.end())
            throw std::invalid_argument("speech_anchor_outside_enclosing_unit");
        result.push_back(std::move(identifier));
    }
    return result;
}

SpeechReferent referent(
    const JsonValue& value, const std::set<std::string, std::less<>>& known) {
    if (!value.is_object() || value.as_object().size() != 2 ||
        !value.as_object().contains("entity") || !value.as_object().contains("anchors") ||
        !std::holds_alternative<std::string>(value.at("entity").storage()) ||
        !text(value.at("entity").as_string()))
        throw std::invalid_argument("invalid_semantic_speech_referent");
    return {std::string(value.at("entity").as_string()), references(value.at("anchors"), known)};
}

std::vector<SpeechReferent> referent_list(
    const JsonValue& value, const std::set<std::string, std::less<>>& known) {
    if (!value.is_array()) throw std::invalid_argument("invalid_semantic_speech_referents");
    std::vector<SpeechReferent> result;
    for (const auto& row : value.as_array()) result.push_back(referent(row, known));
    return result;
}

}  // namespace

std::vector<SpeechReferent> SpeechValue::referents() const {
    std::vector<SpeechReferent> result;
    if (speaker) result.push_back(*speaker);
    result.insert(result.end(), addressees.begin(), addressees.end());
    result.insert(result.end(), topics.begin(), topics.end());
    return result;
}

bool SpeechValue::addresses_entity(const std::string_view identifier) const {
    const auto rows = referents();
    return std::ranges::any_of(rows, [&](const auto& row) { return row.entity == identifier; });
}

std::tuple<std::string, std::optional<std::string>, std::vector<std::string>,
           std::vector<std::string>, std::string>
SpeechValue::comparison_key() const {
    std::vector<std::string> addressee_entities, topic_entities;
    for (const auto& row : addressees) addressee_entities.push_back(row.entity);
    for (const auto& row : topics) topic_entities.push_back(row.entity);
    return {kind, speaker ? std::optional<std::string>{speaker->entity} : std::nullopt,
            std::move(addressee_entities), std::move(topic_entities), content};
}

SpeechValue parse_speech_value(
    const JsonValue& value, const std::vector<std::string>& unit_anchors) {
    static const std::set<std::string, std::less<>> fields{
        "schema", "kind", "speaker", "addressees", "topics", "content", "content_anchors"};
    if (!value.is_object() || value.as_object().size() != fields.size() ||
        !std::ranges::all_of(fields, [&](const auto& key) { return value.as_object().contains(key); }) ||
        !std::holds_alternative<std::string>(value.at("schema").storage()) ||
        value.at("schema").as_string() != semantic_speech_value_schema_id ||
        !std::holds_alternative<std::string>(value.at("kind").storage()) ||
        !kinds.contains(std::string(value.at("kind").as_string())) ||
        !std::holds_alternative<std::string>(value.at("content").storage()) ||
        !text(value.at("content").as_string()))
        throw std::invalid_argument("invalid_semantic_speech_value");
    const std::set<std::string, std::less<>> known(unit_anchors.begin(), unit_anchors.end());
    std::optional<SpeechReferent> speaker;
    if (!std::holds_alternative<std::nullptr_t>(value.at("speaker").storage()))
        speaker = referent(value.at("speaker"), known);
    return {std::string(value.at("kind").as_string()), std::move(speaker),
        referent_list(value.at("addressees"), known), referent_list(value.at("topics"), known),
        std::string(value.at("content").as_string()), references(value.at("content_anchors"), known)};
}

JsonValue speech_value_schema(const JsonValue& references_schema) {
    JsonValue text_schema = JsonValue::Object{{"type", "string"}, {"minLength", 1}};
    JsonValue referent_schema = JsonValue::Object{
        {"type", "object"}, {"additionalProperties", false},
        {"properties", JsonValue::Object{{"entity", text_schema}, {"anchors", references_schema}}},
        {"required", JsonValue::Array{"entity", "anchors"}}};
    JsonValue::Array kind_values;
    for (const auto& value : kinds) kind_values.emplace_back(value);
    return JsonValue::Object{{"type", "object"}, {"additionalProperties", false},
        {"properties", JsonValue::Object{
            {"schema", JsonValue::Object{{"type", "string"}, {"const", std::string(semantic_speech_value_schema_id)}}},
            {"kind", JsonValue::Object{{"type", "string"}, {"enum", JsonValue(std::move(kind_values))}}},
            {"speaker", JsonValue::Object{{"anyOf", JsonValue::Array{referent_schema, JsonValue::Object{{"type", "null"}}}}},
            {"addressees", JsonValue::Object{{"type", "array"}, {"items", referent_schema}}},
            {"topics", JsonValue::Object{{"type", "array"}, {"items", referent_schema}}},
            {"content", text_schema}, {"content_anchors", references_schema}}},
        {"required", JsonValue::Array{"schema", "kind", "speaker", "addressees", "topics", "content", "content_anchors"}}};
}

}  // namespace swegca::world
