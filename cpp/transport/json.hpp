#pragma once
#include <memory_resource>
#include <iosfwd>
#include <span>
#include <string_view>
#include <string>
#include <vector>

namespace swegca::transport {
// Transport syntax only; no evidence classification or VRS decisions.
struct Json {
    enum class Kind { null, boolean, number, string, array, object };
    explicit Json(std::pmr::memory_resource* resource): scalar(resource),values(resource),keys(resource) {}
    Kind kind=Kind::null;
    std::pmr::string scalar;
    std::pmr::vector<Json> values;
    std::pmr::vector<std::pmr::string> keys;
    [[nodiscard]] const Json* find(std::string_view key) const noexcept;
    [[nodiscard]] const Json& at(std::string_view key) const;
    [[nodiscard]] std::string_view string() const;
};
[[nodiscard]] Json parse_json(std::string_view text,std::pmr::memory_resource&,std::size_t max_depth=64);
[[nodiscard]] std::pmr::string encode_json(const Json&,std::pmr::memory_resource&);
// Validate before output and emit quoted runs without a payload-sized copy.
void write_json_string(std::ostream&,std::string_view);
[[nodiscard]] std::pmr::string quote_json(std::string_view,std::pmr::memory_resource&);
} // namespace swegca::transport
