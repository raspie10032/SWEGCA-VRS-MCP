#pragma once

#include "world/cognitive_state.hpp"

#include <string_view>
#include <vector>

namespace swegca::world {

inline constexpr std::string_view semantic_table_value_source_sha256 =
    "ca20bc58cdf708e2a31453c0fb6bafd03e3ee38a77dddfe81da8ab1a64f3074c";
inline constexpr std::string_view semantic_table_value_schema_id =
    "rozephine-table-value-v1";

struct TableValue final {
    JsonValue::Array columns;
    std::vector<JsonValue::Array> rows;

    TableValue(JsonValue::Array columns, std::vector<JsonValue::Array> rows);
    [[nodiscard]] JsonValue comparison_key() const;
    friend bool operator==(const TableValue& left, const TableValue& right) {
        return left.comparison_key() == right.comparison_key();
    }
};

[[nodiscard]] TableValue parse_table_value(const JsonValue& value);
[[nodiscard]] JsonValue table_value_schema();

}  // namespace swegca::world
