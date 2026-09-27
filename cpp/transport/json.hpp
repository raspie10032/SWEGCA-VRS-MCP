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
    [[nodiscard]] Json& at(std::string_view key) {
        return const_cast<Json&>(static_cast<const Json&>(*this).at(key));
    }
    [[nodiscard]] std::string_view string() const;
};
// Source range for one selected object-member path; no per-node metadata.
// Offsets refer only to the unchanged input used for this successful parse.
struct JsonMemberSource {
    std::size_t offset=0,size=0;
    [[nodiscard]] std::string_view bytes(std::string_view input) const;
};
[[nodiscard]] Json parse_json_member(std::string_view text,std::pmr::memory_resource&,
    std::span<const std::string_view> path,JsonMemberSource&,std::size_t max_depth=64);
[[nodiscard]] Json parse_json(std::string_view text,std::pmr::memory_resource&,std::size_t max_depth=64);
// Validate the complete JSON, retaining only the named object-member subtree.
// Missing paths throw; skipped objects still check duplicate keys. Memory for
// those keys follows object width, not the size of skipped arrays/string values.
[[nodiscard]] Json parse_json_selected(std::string_view text,std::pmr::memory_resource&,
    std::span<const std::string_view> path,std::size_t max_depth=64);
// Validate all syntax and return only a source range, without retaining values.
[[nodiscard]] JsonMemberSource locate_json_member(std::string_view text,std::pmr::memory_resource&,
    std::span<const std::string_view> path,std::size_t max_depth=64);
[[nodiscard]] std::pmr::string encode_json(const Json&,std::pmr::memory_resource&);
// Append directly to an exclusively owned destination. The caller must discard
// the unfinished message on failure; value must not alias the destination.
// Optional suffix capacity reserves space for a caller-owned enclosing frame.
void append_json(std::pmr::string& destination,const Json& value,std::size_t suffix_capacity=0);
void append_json_string(std::pmr::string& destination,std::string_view text,std::size_t suffix_capacity=0);
// Validate before output and emit quoted runs without a payload-sized copy.
void write_json_string(std::ostream&,std::string_view);
void write_json_string_content(std::ostream&,std::string_view);
void write_json_hex(std::ostream&,std::span<const std::byte>);
[[nodiscard]] std::pmr::string quote_json(std::string_view,std::pmr::memory_resource&);
} // namespace swegca::transport
