#include "world/transport_plain.hpp"

namespace swegca::world {

JsonValue transport_plain(const JsonValue& value) {
    if (const auto* object = std::get_if<JsonValue::Object>(&value.storage())) {
        JsonValue::Object detached;
        for (const auto& [key, child] : *object)
            detached.emplace(key, transport_plain(child));
        return JsonValue(std::move(detached));
    }
    if (const auto* array = std::get_if<JsonValue::Array>(&value.storage())) {
        JsonValue::Array detached;
        detached.reserve(array->size());
        for (const auto& child : *array) detached.push_back(transport_plain(child));
        return JsonValue(std::move(detached));
    }
    return value;
}

}  // namespace swegca::world
