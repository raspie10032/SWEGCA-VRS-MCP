#include "world/semantic_source_context.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <stdexcept>
#include <utility>

namespace swegca::world {
namespace {

[[noreturn]] void reject(const char* reason) { throw std::invalid_argument(reason); }

[[nodiscard]] JsonValue strings(const std::vector<std::string>& values) {
    JsonValue::Array result;
    result.reserve(values.size());
    for (const auto& value : values) result.emplace_back(value);
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

[[nodiscard]] std::vector<SemanticPathElement> parse_path(const JsonValue& value) {
    if (!value.is_array()) reject("invalid_semantic_input_context");
    std::vector<SemanticPathElement> result;
    result.reserve(value.as_array().size());
    for (const auto& item : value.as_array()) {
        if (const auto* integer = std::get_if<std::int64_t>(&item.storage()))
            result.emplace_back(*integer);
        else if (const auto* text = std::get_if<std::string>(&item.storage()))
            result.emplace_back(*text);
        else
            reject("invalid_semantic_input_context");
    }
    return result;
}

[[nodiscard]] std::vector<std::string> parse_strings(const JsonValue& value) {
    if (!value.is_array()) reject("invalid_semantic_input_context");
    std::vector<std::string> result;
    result.reserve(value.as_array().size());
    for (const auto& item : value.as_array()) {
        const auto* text = std::get_if<std::string>(&item.storage());
        if (!text) reject("invalid_semantic_input_context");
        result.push_back(*text);
    }
    return result;
}

[[nodiscard]] SemanticSourceContext parse_context(const JsonValue& value) {
    static const std::set<std::string, std::less<>> fields{
        "step", "path", "excluded_fields", "anchor_ids", "value_json"};
    if (!value.is_object() || value.as_object().size() != fields.size() ||
        !std::ranges::all_of(fields, [&](const auto& key) {
            return value.as_object().contains(key);
        }))
        reject("invalid_semantic_input_context");
    const auto* step = std::get_if<std::int64_t>(&value.at("step").storage());
    const auto* value_json = std::get_if<std::string>(&value.at("value_json").storage());
    if (!step || !value_json) reject("invalid_semantic_input_context");
    return {*step, parse_path(value.at("path")),
            parse_strings(value.at("excluded_fields")),
            parse_strings(value.at("anchor_ids")), *value_json};
}

[[nodiscard]] const JsonValue::Object& object_at(
    const SemanticSourceEpisode& source, const std::int64_t step,
    const std::vector<SemanticPathElement>& path) {
    if (step < 0 || static_cast<std::size_t>(step) >= source.steps.size())
        reject("authored_source_context_requires_mapping");
    const JsonValue* value = &source.steps[static_cast<std::size_t>(step)].observation;
    for (const auto& element : path) {
        if (const auto* key = std::get_if<std::string>(&element)) {
            const auto* object = std::get_if<JsonValue::Object>(&value->storage());
            if (!object) reject("authored_source_context_requires_mapping");
            const auto found = object->find(*key);
            if (found == object->end()) reject("authored_source_context_requires_mapping");
            value = &found->second;
        } else {
            const auto index = std::get<std::int64_t>(element);
            const auto* array = std::get_if<JsonValue::Array>(&value->storage());
            if (!array || index < 0 || static_cast<std::size_t>(index) >= array->size())
                reject("authored_source_context_requires_mapping");
            value = &(*array)[static_cast<std::size_t>(index)];
        }
    }
    const auto* object = std::get_if<JsonValue::Object>(&value->storage());
    if (!object) reject("authored_source_context_requires_mapping");
    return *object;
}

void append_context(std::vector<SemanticSourceContext>& contexts,
                    const SemanticSourceEpisode& source, const std::int64_t step,
                    std::vector<SemanticPathElement> path,
                    std::vector<std::string> excluded,
                    std::vector<std::string> identifiers) {
    const auto& source_value = object_at(source, step, path);
    JsonValue::Object selected;
    for (const auto& [key, value] : source_value) {
        if (std::ranges::find(excluded, key) == excluded.end()) selected.emplace(key, value);
    }
    contexts.push_back({step, std::move(path), std::move(excluded),
                        std::move(identifiers),
                        semantic_canonical_json(JsonValue(std::move(selected)))});
}

}  // namespace

JsonValue SemanticSourceContext::to_json() const {
    return JsonValue::Object{{"anchor_ids", strings(anchor_ids)},
                             {"excluded_fields", strings(excluded_fields)},
                             {"path", path_value(path)},
                             {"step", step},
                             {"value_json", value_json}};
}

SemanticContextByAnchor input_context_by_anchor(const SemanticEncoding& encoding) {
    SemanticContextByAnchor result;
    for (const auto& row : encoding.input_context) {
        const auto context = parse_context(row);
        for (const auto& identifier : context.anchor_ids)
            result[identifier].push_back(context);
    }
    return result;
}

std::vector<SemanticSourceContext> authored_source_context(
    const SemanticSourceEpisode& source,
    const std::vector<SemanticAnchor>& anchors) {
    std::map<std::int64_t,
             std::map<std::int64_t, std::vector<std::string>>> grouped;
    for (const auto& anchor : anchors) {
        if (anchor.step < 0 || static_cast<std::size_t>(anchor.step) >= source.steps.size())
            continue;
        const auto& observation = source.steps[static_cast<std::size_t>(anchor.step)].observation;
        const auto* object = std::get_if<JsonValue::Object>(&observation.storage());
        if (!object) continue;
        const auto schema = object->find("schema_version");
        const auto kind = object->find("media_kind");
        if (schema == object->end() || kind == object->end() ||
            !std::holds_alternative<std::string>(schema->second.storage()) ||
            !std::holds_alternative<std::string>(kind->second.storage()) ||
            schema->second.as_string() != authored_media_observation_schema ||
            kind->second.as_string() != "authored_text" ||
            anchor.modality != "text" || anchor.path.size() != 4 ||
            !std::holds_alternative<std::string>(anchor.path[0]) ||
            std::get<std::string>(anchor.path[0]) != "content" ||
            !std::holds_alternative<std::string>(anchor.path[1]) ||
            std::get<std::string>(anchor.path[1]) != "variants" ||
            !std::holds_alternative<std::int64_t>(anchor.path[2]) ||
            !std::holds_alternative<std::string>(anchor.path[3]) ||
            std::get<std::string>(anchor.path[3]) != "text")
            continue;
        grouped[anchor.step][std::get<std::int64_t>(anchor.path[2])].push_back(
            anchor.identifier);
    }

    std::vector<SemanticSourceContext> contexts;
    for (const auto& [step, variants] : grouped) {
        std::vector<std::string> identifiers;
        for (const auto& [_, values] : variants)
            identifiers.insert(identifiers.end(), values.begin(), values.end());
        append_context(contexts, source, step, {}, {"content"}, identifiers);
        append_context(contexts, source, step, {std::string("content")},
                       {"variants"}, identifiers);
        for (const auto& [index, values] : variants) {
            append_context(contexts, source, step,
                           {std::string("content"), std::string("variants"), index},
                           {"text"}, values);
        }
    }
    return contexts;
}

std::vector<SemanticSourceContext> restore_source_context(
    const JsonValue::Array& records, const SemanticSourceEpisode& source,
    const std::vector<SemanticAnchor>& anchors) {
    std::vector<SemanticSourceContext> actual_rows;
    actual_rows.reserve(records.size());
    std::set<std::string, std::less<>> identifiers;
    for (const auto& record : records) {
        auto row = parse_context(record);
        identifiers.insert(row.anchor_ids.begin(), row.anchor_ids.end());
        actual_rows.push_back(std::move(row));
    }
    std::set<std::string, std::less<>> known;
    for (const auto& anchor : anchors) known.insert(anchor.identifier);
    if (!std::ranges::all_of(identifiers, [&](const auto& identifier) {
            return known.contains(identifier);
        }))
        reject("semantic_input_context_changed");

    std::vector<SemanticAnchor> selected;
    for (const auto& anchor : anchors)
        if (identifiers.contains(anchor.identifier)) selected.push_back(anchor);
    auto expected = authored_source_context(source, selected);

    JsonValue::Array expected_json;
    expected_json.reserve(expected.size());
    for (const auto& row : expected) expected_json.push_back(row.to_json());
    if (semantic_canonical_json(JsonValue(records)) !=
        semantic_canonical_json(JsonValue(std::move(expected_json))))
        reject("semantic_input_context_changed");
    return expected;
}

}  // namespace swegca::world
