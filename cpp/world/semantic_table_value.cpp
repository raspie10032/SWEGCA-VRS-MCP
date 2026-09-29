#include "world/semantic_table_value.hpp"

#include "world/unicode_nfkc.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace swegca::world {
namespace {

bool scalar(const JsonValue& value) {
    if (value.is_array() || value.is_object()) return false;
    if (const auto* real = std::get_if<double>(&value.storage())) return std::isfinite(*real);
    return true;
}

std::string type_name(const JsonValue& value) {
    if (std::holds_alternative<std::nullptr_t>(value.storage())) return "NoneType";
    if (std::holds_alternative<bool>(value.storage())) return "bool";
    if (std::holds_alternative<std::int64_t>(value.storage()) ||
        std::holds_alternative<JsonInteger>(value.storage())) return "int";
    if (std::holds_alternative<double>(value.storage())) return "float";
    return "str";
}

}  // namespace

TableValue::TableValue(JsonValue::Array columns_value, std::vector<JsonValue::Array> rows_value)
    : columns(std::move(columns_value)), rows(std::move(rows_value)) {
    if (columns.empty()) throw std::invalid_argument("invalid_semantic_table_value");
    for (const auto& column : columns) {
        if (std::holds_alternative<std::nullptr_t>(column.storage())) continue;
        if (!std::holds_alternative<std::string>(column.storage()) ||
            strip_unicode_whitespace(column.as_string()).empty())
            throw std::invalid_argument("invalid_semantic_table_value");
    }
    for (const auto& row : rows)
        if (row.size() != columns.size() ||
            std::ranges::any_of(row, [](const auto& cell) { return !scalar(cell); }))
            throw std::invalid_argument("invalid_semantic_table_value");
}

JsonValue TableValue::comparison_key() const {
    JsonValue::Array typed_rows;
    for (const auto& row : rows) {
        JsonValue::Array typed;
        for (const auto& cell : row) typed.emplace_back(JsonValue::Array{type_name(cell), cell});
        typed_rows.emplace_back(std::move(typed));
    }
    return JsonValue::Array{JsonValue(columns), JsonValue(std::move(typed_rows))};
}

TableValue parse_table_value(const JsonValue& value) {
    if (!value.is_object() || value.as_object().size() != 3 ||
        !value.as_object().contains("schema") || !value.as_object().contains("columns") ||
        !value.as_object().contains("rows") ||
        !std::holds_alternative<std::string>(value.at("schema").storage()) ||
        value.at("schema").as_string() != semantic_table_value_schema_id ||
        !value.at("columns").is_array() || !value.at("rows").is_array())
        throw std::invalid_argument("invalid_semantic_table_value");
    std::vector<JsonValue::Array> rows;
    for (const auto& row : value.at("rows").as_array()) {
        if (!row.is_array()) throw std::invalid_argument("invalid_semantic_table_value");
        rows.push_back(row.as_array());
    }
    return {value.at("columns").as_array(), std::move(rows)};
}

JsonValue table_value_schema() {
    return JsonValue::Object{{"type", "object"}, {"additionalProperties", false},
        {"properties", JsonValue::Object{
            {"schema", JsonValue::Object{{"type", "string"}, {"const", std::string(semantic_table_value_schema_id)}}},
            {"columns", JsonValue::Object{{"type", "array"}, {"minItems", 1},
                {"items", JsonValue::Object{{"anyOf", JsonValue::Array{
                    JsonValue::Object{{"type", "string"}, {"minLength", 1}},
                    JsonValue::Object{{"type", "null"}}}}}}},
            {"rows", JsonValue::Object{{"type", "array"},
                {"items", JsonValue::Object{{"type", "array"},
                    {"items", JsonValue::Object{{"anyOf", JsonValue::Array{
                        JsonValue::Object{{"type", "string"}}, JsonValue::Object{{"type", "number"}},
                        JsonValue::Object{{"type", "boolean"}}, JsonValue::Object{{"type", "null"}}}}}}}}}}},
        {"required", JsonValue::Array{"schema", "columns", "rows"}}};
}

}  // namespace swegca::world
