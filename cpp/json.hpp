#pragma once

#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace swegca::vrs {

// UTF-8 object keys use byte order, which is Unicode code-point order for
// valid UTF-8, matching the author's Python sorted(str keys) canonical form.
struct Json {
    using Array = std::vector<Json>;
    using Object = std::map<std::string, Json, std::less<>>;
    using Data = std::variant<std::nullptr_t, bool, std::int64_t, double,
                              std::string, Array, Object>;
    Data data;

    Json() = default;
    // SWEGCA: src/swegca_vrs2/store.py@7536139:53-54
    template <typename T> explicit Json(T value) : data(std::move(value)) {}

    // SWEGCA: src/swegca_vrs2/native_transport.py@7536139:30-48
    [[nodiscard]] static Json parse(std::string_view text);
    // SWEGCA: src/swegca_vrs2/store.py@7536139:53-54
    [[nodiscard]] std::string canonical() const;
    // SWEGCA: src/swegca_vrs2/store.py@7536139:41-50
    [[nodiscard]] const Array& array() const;
    // SWEGCA: src/swegca_vrs2/store.py@7536139:41-50
    [[nodiscard]] const Object& object() const;
    // SWEGCA: src/swegca_vrs2/store.py@7536139:41-50
    [[nodiscard]] const std::string& string() const;
    // SWEGCA: src/swegca_vrs2/store.py@7536139:41-50
    [[nodiscard]] std::int64_t integer() const;
    // SWEGCA: src/swegca_vrs2/store.py@7536139:41-50
    [[nodiscard]] const Json& at(std::string_view key) const;
    // SWEGCA: src/swegca_vrs2/store.py@7536139:41-50
    [[nodiscard]] bool contains(std::string_view key) const;
};

// The caller supplies only the author episode-row fields until Episode is ported.
// SWEGCA: src/tinylm_slicer/mosaic_snapshot_digest.py@3bddcb7:144-187
[[nodiscard]] std::string snapshot_digest(const std::vector<Json>& episodes,
                                          const Json& postings);

}  // namespace swegca::vrs
