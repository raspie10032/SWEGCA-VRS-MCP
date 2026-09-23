#include "json.hpp"
#include "digest.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <limits>

namespace swegca::vrs {
namespace {

// SWEGCA: src/swegca_vrs2/store.py@7536139:82-87
void validate_utf8(std::string_view bytes) {
    for (std::size_t i = 0; i < bytes.size();) {
        const auto a = static_cast<unsigned char>(bytes[i]);
        if (a < 0x80) { ++i; continue; }
        const auto remain = bytes.size() - i;
        const auto continuation = [&](std::size_t at) {
            return (static_cast<unsigned char>(bytes[i + at]) & 0xc0) == 0x80;
        };
        if (a >= 0xc2 && a <= 0xdf && remain >= 2 && continuation(1)) {
            i += 2; continue;
        }
        if (a >= 0xe0 && a <= 0xef && remain >= 3 && continuation(1) && continuation(2)) {
            const auto b = static_cast<unsigned char>(bytes[i + 1]);
            if ((a != 0xe0 || b >= 0xa0) && (a != 0xed || b <= 0x9f)) {
                i += 3; continue;
            }
        }
        if (a >= 0xf0 && a <= 0xf4 && remain >= 4 && continuation(1) &&
            continuation(2) && continuation(3)) {
            const auto b = static_cast<unsigned char>(bytes[i + 1]);
            if ((a != 0xf0 || b >= 0x90) && (a != 0xf4 || b <= 0x8f)) {
                i += 4; continue;
            }
        }
        throw std::runtime_error("invalid_utf8");
    }
}

// SWEGCA: src/swegca_vrs2/store.py@7536139:53-54
void append_codepoint(std::string& target, std::uint32_t point) {
    if (point <= 0x7f) target.push_back(static_cast<char>(point));
    else if (point <= 0x7ff) {
        target.push_back(static_cast<char>(0xc0 | (point >> 6)));
        target.push_back(static_cast<char>(0x80 | (point & 0x3f)));
    } else if (point <= 0xffff && (point < 0xd800 || point > 0xdfff)) {
        target.push_back(static_cast<char>(0xe0 | (point >> 12)));
        target.push_back(static_cast<char>(0x80 | ((point >> 6) & 0x3f)));
        target.push_back(static_cast<char>(0x80 | (point & 0x3f)));
    } else if (point >= 0x10000 && point <= 0x10ffff) {
        target.push_back(static_cast<char>(0xf0 | (point >> 18)));
        target.push_back(static_cast<char>(0x80 | ((point >> 12) & 0x3f)));
        target.push_back(static_cast<char>(0x80 | ((point >> 6) & 0x3f)));
        target.push_back(static_cast<char>(0x80 | (point & 0x3f)));
    } else throw std::runtime_error("invalid_unicode_escape");
}

// SWEGCA: src/tinylm_slicer/mosaic_snapshot_digest.py@3bddcb7:82-89
void flush_hash_chunk(std::string& target, Sha256* hash) {
    if (hash && target.size() >= 65536) {
        hash->update(target);
        target.clear();
    }
}

// SWEGCA: src/tinylm_slicer/mosaic_snapshot_digest.py@3bddcb7:91-96
void append_string(std::string& target, std::string_view value, Sha256* hash = nullptr) {
    validate_utf8(value);
    constexpr char hex[] = "0123456789abcdef";
    target.push_back('"');
    std::size_t chunk_size = 0;
    for (unsigned char byte : value) {
        switch (byte) {
            case '"': target += "\\\""; break;
            case '\\': target += "\\\\"; break;
            case '\b': target += "\\b"; break;
            case '\f': target += "\\f"; break;
            case '\n': target += "\\n"; break;
            case '\r': target += "\\r"; break;
            case '\t': target += "\\t"; break;
            default:
                if (byte < 0x20) {
                    target += "\\u00";
                    target.push_back(hex[byte >> 4]);
                    target.push_back(hex[byte & 15]);
                } else target.push_back(static_cast<char>(byte));
        }
        if (++chunk_size == 16384) {
            flush_hash_chunk(target, hash);
            chunk_size = 0;
        }
    }
    target.push_back('"');
}

// SWEGCA: src/tinylm_slicer/mosaic_snapshot_digest.py@3bddcb7:107-108
std::string python_float(double value, bool allow_nan) {
    if (!std::isfinite(value)) {
        if (!allow_nan) throw std::runtime_error("nonfinite_json");
        if (std::isnan(value)) return "NaN";
        return std::signbit(value) ? "-Infinity" : "Infinity";
    }
    if (value == 0.0) return std::signbit(value) ? "-0.0" : "0.0";
    std::array<char, 128> buffer{};
    const auto converted = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    if (converted.ec != std::errc{}) throw std::runtime_error("float_encoding_failed");
    std::string text(buffer.data(), converted.ptr);
    const bool negative = text.starts_with('-');
    if (negative) text.erase(0, 1);
    const auto e = text.find_first_of("eE");
    int exponent = 0;
    if (e != std::string::npos) {
        auto number = text.substr(e + 1);
        auto parsed = std::from_chars(number.data(), number.data() + number.size(), exponent);
        if (parsed.ec != std::errc{} || parsed.ptr != number.data() + number.size())
            throw std::runtime_error("float_encoding_failed");
        text.resize(e);
    }
    const auto point = text.find('.');
    const auto point_index = point == std::string::npos ? text.size() : point;
    std::string digits;
    for (char c : text) if (c != '.') digits.push_back(c);
    const auto first = digits.find_first_not_of('0');
    if (first == std::string::npos) throw std::runtime_error("float_encoding_failed");
    const auto decimal_exponent = static_cast<int>(point_index) -
                                  static_cast<int>(first) - 1 + exponent;
    digits.erase(0, first);
    while (digits.size() > 1 && digits.back() == '0') digits.pop_back();
    std::string result = negative ? "-" : "";
    if (decimal_exponent >= -4 && decimal_exponent < 16) {
        const int before = decimal_exponent + 1;
        if (before <= 0) {
            result += "0.";
            result.append(static_cast<std::size_t>(-before), '0');
            result += digits;
        } else if (static_cast<std::size_t>(before) >= digits.size()) {
            result += digits;
            result.append(static_cast<std::size_t>(before) - digits.size(), '0');
            result += ".0";
        } else {
            result += digits.substr(0, static_cast<std::size_t>(before));
            result.push_back('.');
            result += digits.substr(static_cast<std::size_t>(before));
        }
        return result;
    }
    result.push_back(digits[0]);
    if (digits.size() > 1) {
        result.push_back('.');
        result += digits.substr(1);
    }
    result.push_back('e');
    result.push_back(decimal_exponent < 0 ? '-' : '+');
    auto magnitude = std::to_string(std::abs(decimal_exponent));
    if (magnitude.size() == 1) result.push_back('0');
    result += magnitude;
    return result;
}

// SWEGCA: src/swegca_vrs2/store.py@7536139:53-54
void append_canonical(std::string& target, const Json& value, std::uint32_t depth,
                      Sha256* hash = nullptr, bool allow_nan = false) {
    // The source serializer is recursive; bound the C++ representation as well.
    if (depth > 256) throw std::runtime_error("json_depth_limit");
    if (std::holds_alternative<std::nullptr_t>(value.data)) target += "null";
    else if (auto flag = std::get_if<bool>(&value.data)) target += *flag ? "true" : "false";
    else if (auto integer = std::get_if<std::int64_t>(&value.data)) target += std::to_string(*integer);
    else if (auto real = std::get_if<double>(&value.data)) target += python_float(*real, allow_nan);
    else if (auto string = std::get_if<std::string>(&value.data)) append_string(target, *string, hash);
    else if (auto array = std::get_if<Json::Array>(&value.data)) {
        target.push_back('[');
        for (std::size_t i = 0; i < array->size(); ++i) {
            if (i) target.push_back(',');
            append_canonical(target, array->at(i), depth + 1, hash, allow_nan);
            flush_hash_chunk(target, hash);
        }
        target.push_back(']');
    } else {
        const auto& object = std::get<Json::Object>(value.data);
        target.push_back('{');
        bool first = true;
        for (const auto& [key, child] : object) {
            if (!first) target.push_back(',');
            first = false;
            append_string(target, key, hash);
            target.push_back(':');
            append_canonical(target, child, depth + 1, hash, allow_nan);
            flush_hash_chunk(target, hash);
        }
        target.push_back('}');
    }
}

class Parser {
public:
    // SWEGCA: src/swegca_vrs2/native_transport.py@7536139:30-48
    explicit Parser(std::string_view text) : text_(text) {}
    // SWEGCA: src/swegca_vrs2/native_transport.py@7536139:30-48
    Json parse() {
        auto result = value(0);
        space();
        if (offset_ != text_.size()) throw std::runtime_error("json_trailing_data");
        return result;
    }

private:
    std::string_view text_;
    std::size_t offset_ = 0;

    // SWEGCA: src/swegca_vrs2/native_transport.py@7536139:30-48
    void space() {
        while (offset_ < text_.size() && (text_[offset_] == ' ' || text_[offset_] == '\n' ||
               text_[offset_] == '\r' || text_[offset_] == '\t')) ++offset_;
    }
    // SWEGCA: src/swegca_vrs2/native_transport.py@7536139:30-48
    bool consume(char want) {
        space();
        if (offset_ < text_.size() && text_[offset_] == want) { ++offset_; return true; }
        return false;
    }
    // SWEGCA: src/swegca_vrs2/native_transport.py@7536139:30-48
    std::uint32_t hex4() {
        if (text_.size() - offset_ < 4) throw std::runtime_error("invalid_unicode_escape");
        std::uint32_t value = 0;
        for (int i = 0; i < 4; ++i) {
            const auto c = text_[offset_++];
            value <<= 4;
            if (c >= '0' && c <= '9') value |= static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f') value |= static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') value |= static_cast<unsigned>(c - 'A' + 10);
            else throw std::runtime_error("invalid_unicode_escape");
        }
        return value;
    }
    // SWEGCA: src/swegca_vrs2/native_transport.py@7536139:30-48
    std::string string() {
        if (offset_ >= text_.size() || text_[offset_++] != '"')
            throw std::runtime_error("json_string_required");
        std::string output;
        while (offset_ < text_.size()) {
            const auto c = text_[offset_++];
            if (c == '"') { validate_utf8(output); return output; }
            if (static_cast<unsigned char>(c) < 0x20)
                throw std::runtime_error("invalid_json_string");
            if (c != '\\') { output.push_back(c); continue; }
            if (offset_ == text_.size()) throw std::runtime_error("invalid_json_escape");
            switch (text_[offset_++]) {
                case '"': output.push_back('"'); break;
                case '\\': output.push_back('\\'); break;
                case '/': output.push_back('/'); break;
                case 'b': output.push_back('\b'); break;
                case 'f': output.push_back('\f'); break;
                case 'n': output.push_back('\n'); break;
                case 'r': output.push_back('\r'); break;
                case 't': output.push_back('\t'); break;
                case 'u': {
                    auto point = hex4();
                    if (point >= 0xd800 && point <= 0xdbff) {
                        if (text_.size() - offset_ < 6 || text_.substr(offset_, 2) != "\\u")
                            throw std::runtime_error("invalid_unicode_escape");
                        offset_ += 2;
                        const auto low = hex4();
                        if (low < 0xdc00 || low > 0xdfff)
                            throw std::runtime_error("invalid_unicode_escape");
                        point = 0x10000 + ((point - 0xd800) << 10) + (low - 0xdc00);
                    } else if (point >= 0xdc00 && point <= 0xdfff)
                        throw std::runtime_error("invalid_unicode_escape");
                    append_codepoint(output, point);
                    break;
                }
                default: throw std::runtime_error("invalid_json_escape");
            }
        }
        throw std::runtime_error("json_string_unterminated");
    }
    // SWEGCA: src/swegca_vrs2/native_transport.py@7536139:30-48
    Json number() {
        const auto start = offset_;
        if (text_[offset_] == '-') ++offset_;
        if (offset_ == text_.size()) throw std::runtime_error("invalid_json_number");
        if (text_[offset_] == '0') ++offset_;
        else {
            if (text_[offset_] < '1' || text_[offset_] > '9')
                throw std::runtime_error("invalid_json_number");
            while (offset_ < text_.size() && text_[offset_] >= '0' && text_[offset_] <= '9')
                ++offset_;
        }
        bool real = false;
        if (offset_ < text_.size() && text_[offset_] == '.') {
            real = true; ++offset_;
            const auto digits = offset_;
            while (offset_ < text_.size() && text_[offset_] >= '0' && text_[offset_] <= '9')
                ++offset_;
            if (digits == offset_) throw std::runtime_error("invalid_json_number");
        }
        if (offset_ < text_.size() && (text_[offset_] == 'e' || text_[offset_] == 'E')) {
            real = true; ++offset_;
            if (offset_ < text_.size() && (text_[offset_] == '+' || text_[offset_] == '-')) ++offset_;
            const auto digits = offset_;
            while (offset_ < text_.size() && text_[offset_] >= '0' && text_[offset_] <= '9')
                ++offset_;
            if (digits == offset_) throw std::runtime_error("invalid_json_number");
        }
        const auto token = text_.substr(start, offset_ - start);
        if (real) {
            // Source json.loads accepts 1e999 as infinity; this transport rejects
            // that overflow before the author finite-value encode boundary.
            double parsed = 0;
            const auto result = std::from_chars(token.data(), token.data() + token.size(), parsed);
            if (result.ec != std::errc{} || result.ptr != token.data() + token.size() ||
                !std::isfinite(parsed)) throw std::runtime_error("invalid_json_number");
            return Json(parsed);
        }
        std::int64_t parsed = 0;
        const auto result = std::from_chars(token.data(), token.data() + token.size(), parsed);
        if (result.ec != std::errc{} || result.ptr != token.data() + token.size())
            throw std::runtime_error("json_integer_out_of_range");
        return Json(parsed);
    }
    // SWEGCA: src/swegca_vrs2/native_transport.py@7536139:30-48
    Json value(std::uint32_t depth) {
        // Representation of the author transport recursion-error boundary.
        if (depth > 256) throw std::runtime_error("invalid_json");
        space();
        if (offset_ >= text_.size()) throw std::runtime_error("json_value_missing");
        if (text_.substr(offset_, 3) == "NaN" ||
            text_.substr(offset_, 8) == "Infinity" ||
            text_.substr(offset_, 9) == "-Infinity")
            throw std::runtime_error("nonfinite_json");
        if (text_[offset_] == '"') return Json(string());
        if (consume('[')) {
            Json::Array array;
            if (consume(']')) return Json(std::move(array));
            while (true) {
                array.push_back(value(depth + 1));
                if (consume(']')) return Json(std::move(array));
                if (!consume(',')) throw std::runtime_error("json_array_invalid");
            }
        }
        if (consume('{')) {
            Json::Object object;
            if (consume('}')) return Json(std::move(object));
            while (true) {
                space();
                auto key = string();
                if (!consume(':')) throw std::runtime_error("json_object_invalid");
                auto [at, inserted] = object.emplace(std::move(key), value(depth + 1));
                if (!inserted) throw std::runtime_error("duplicate_key");
                if (consume('}')) return Json(std::move(object));
                if (!consume(',')) throw std::runtime_error("json_object_invalid");
            }
        }
        for (const auto& [word, result] : {
                 std::pair<std::string_view, Json>{"true", Json(true)},
                 {"false", Json(false)}, {"null", Json()}}) {
            if (text_.substr(offset_, word.size()) == word) {
                offset_ += word.size();
                return result;
            }
        }
        if (text_[offset_] == '-' || (text_[offset_] >= '0' && text_[offset_] <= '9'))
            return number();
        throw std::runtime_error("json_value_invalid");
    }
};

}  // namespace

// SWEGCA: src/swegca_vrs2/native_transport.py@7536139:30-48
Json Json::parse(std::string_view text) { return Parser(text).parse(); }

// SWEGCA: src/swegca_vrs2/store.py@7536139:53-54
std::string Json::canonical() const {
    std::string result;
    append_canonical(result, *this, 0);
    return result;
}

// SWEGCA: src/swegca_vrs2/store.py@7536139:41-50
const Json::Array& Json::array() const { return std::get<Array>(data); }

// SWEGCA: src/swegca_vrs2/store.py@7536139:41-50
const Json::Object& Json::object() const { return std::get<Object>(data); }

// SWEGCA: src/swegca_vrs2/store.py@7536139:41-50
const std::string& Json::string() const { return std::get<std::string>(data); }

// SWEGCA: src/swegca_vrs2/store.py@7536139:41-50
std::int64_t Json::integer() const { return std::get<std::int64_t>(data); }

// SWEGCA: src/swegca_vrs2/store.py@7536139:41-50
const Json& Json::at(std::string_view key) const {
    const auto& fields = object();
    const auto found = fields.find(key);
    if (found == fields.end()) throw std::runtime_error("json_key_missing");
    return found->second;
}

// SWEGCA: src/swegca_vrs2/store.py@7536139:41-50
bool Json::contains(std::string_view key) const {
    const auto* fields = std::get_if<Object>(&data);
    return fields && fields->contains(key);
}

// SWEGCA: src/tinylm_slicer/mosaic_snapshot_digest.py@3bddcb7:144-187
std::string snapshot_digest(const std::vector<Json>& episodes, const Json& postings) {
    std::vector<const Json*> ordered;
    ordered.reserve(episodes.size());
    for (const auto& episode : episodes) ordered.push_back(&episode);
    std::sort(ordered.begin(), ordered.end(), [](const Json* left, const Json* right) {
        return left->at("episode_id").string() < right->at("episode_id").string();
    });
    Sha256 hash;
    hash.update("{\"episodes\":[");
    std::string chunk;
    std::string previous;
    for (std::size_t i = 0; i < ordered.size(); ++i) {
        const auto& id = ordered[i]->at("episode_id").string();
        if (i && id == previous) throw std::runtime_error("duplicate_episode_id");
        if (i) chunk.push_back(',');
        append_canonical(chunk, *ordered[i], 0, &hash, true);
        flush_hash_chunk(chunk, &hash);
        previous = id;
    }
    if (!chunk.empty()) {
        hash.update(chunk);
        chunk.clear();
    }
    hash.update("],\"postings\":");
    append_canonical(chunk, postings, 0, &hash, true);
    if (!chunk.empty()) hash.update(chunk);
    hash.update("}");
    const auto digest = hash.finish();
    constexpr char hex[] = "0123456789abcdef";
    std::string result;
    result.reserve(64);
    for (auto byte : digest) {
        const auto value = std::to_integer<unsigned char>(byte);
        result.push_back(hex[value >> 4]);
        result.push_back(hex[value & 15]);
    }
    return result;
}

}  // namespace swegca::vrs
