#include "world/session_event_index.hpp"

#include "swegca_architecture/sha256.hpp"
#include "transport/json.hpp"
#include "world/unicode_nfkc.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <charconv>
#include <limits>
#include <memory_resource>
#include <set>
#include <stdexcept>
#include <utility>

namespace swegca::world {
namespace {

[[nodiscard]] bool text(const std::string_view value) {
    return !strip_unicode_whitespace(value).empty();
}

[[nodiscard]] bool digest_text(const std::string_view value) noexcept {
    if (value.size() != 64) return false;
    return std::ranges::all_of(value, [](const unsigned char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

[[nodiscard]] std::string hex(const architecture::DigestBytes& digest) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.resize(digest.size() * 2);
    for (std::size_t index = 0; index != digest.size(); ++index) {
        const auto value = std::to_integer<unsigned>(digest[index]);
        result[index * 2] = digits[value >> 4U];
        result[index * 2 + 1] = digits[value & 15U];
    }
    return result;
}

[[nodiscard]] std::string sha256(const std::string_view value) {
    architecture::Sha256 hash;
    hash.update(value);
    return hex(hash.finish());
}

[[nodiscard]] bool finite_numbers(const transport::Json& value) {
    if (value.kind == transport::Json::Kind::number) {
        if (value.scalar.find_first_of(".eE") == std::string_view::npos)
            return value.scalar.size() - (value.scalar.starts_with('-') ? 1U : 0U) <= 4300;
        const std::string source(value.scalar);
        char* end = nullptr;
        const auto number = std::strtod(source.c_str(), &end);
        return end == source.c_str() + source.size() && std::isfinite(number);
    }
    return std::ranges::all_of(value.values, finite_numbers);
}

[[nodiscard]] SessionJsonValue freeze(const transport::Json& source) {
    SessionJsonValue result;
    result.kind = static_cast<SessionJsonValue::Kind>(source.kind);
    result.scalar.assign(source.scalar);
    result.keys.reserve(source.keys.size());
    for (const auto& key : source.keys) result.keys.emplace_back(key);
    result.values.reserve(source.values.size());
    for (const auto& child : source.values) result.values.push_back(freeze(child));
    return result;
}

void collect_strings(const transport::Json& value, std::vector<std::string_view>& output) {
    if (value.kind == transport::Json::Kind::object) {
        for (std::size_t index = 0; index != value.values.size(); ++index) {
            output.emplace_back(value.keys[index]);
            collect_strings(value.values[index], output);
        }
    } else if (value.kind == transport::Json::Kind::array) {
        for (const auto& child : value.values) collect_strings(child, output);
    } else if (value.kind == transport::Json::Kind::string) {
        output.emplace_back(value.scalar);
    }
}

struct CodePoint final { char32_t point{}; std::size_t begin{}; std::size_t end{}; };

[[nodiscard]] std::vector<CodePoint> decode(const std::string_view source) {
    std::vector<CodePoint> result;
    for (std::size_t at = 0; at != source.size();) {
        const auto begin = at;
        const auto first = static_cast<unsigned char>(source[at++]);
        std::size_t rest = 0;
        char32_t point = 0;
        char32_t minimum = 0;
        if (first < 0x80U) point = first;
        else if ((first & 0xe0U) == 0xc0U) { rest = 1; point = first & 0x1fU; minimum = 0x80; }
        else if ((first & 0xf0U) == 0xe0U) { rest = 2; point = first & 0x0fU; minimum = 0x800; }
        else if ((first & 0xf8U) == 0xf0U) { rest = 3; point = first & 0x07U; minimum = 0x10000; }
        else throw std::invalid_argument("invalid session UTF-8");
        if (rest > source.size() - at) throw std::invalid_argument("truncated session UTF-8");
        for (std::size_t index = 0; index != rest; ++index) {
            const auto next = static_cast<unsigned char>(source[at++]);
            if ((next & 0xc0U) != 0x80U) throw std::invalid_argument("invalid session UTF-8");
            point = (point << 6U) | (next & 0x3fU);
        }
        if ((rest && point < minimum) || point > 0x10ffffU ||
            (point >= 0xd800U && point <= 0xdfffU))
            throw std::invalid_argument("invalid session Unicode scalar");
        result.push_back({point, begin, at});
    }
    return result;
}

[[nodiscard]] bool token_first(const char32_t c) noexcept {
    return (c >= U'A' && c <= U'Z') || (c >= U'a' && c <= U'z') ||
        (c >= 0xac00 && c <= 0xd7a3) || (c >= 0x3041 && c <= 0x3096) ||
        (c >= 0x30a1 && c <= 0x30fa) || (c >= 0x4e00 && c <= 0x9fff);
}

[[nodiscard]] bool token_rest(const char32_t c) noexcept {
    return token_first(c) || (c >= U'0' && c <= U'9') || c == U'_';
}

void add_tokens(const std::string_view value, std::set<std::string>& keys) {
    const auto points = decode(value);
    for (std::size_t at = 0; at < points.size();) {
        if (!token_first(points[at].point)) { ++at; continue; }
        auto end = at + 1;
        while (end != points.size() && end - at < 64 && token_rest(points[end].point)) ++end;
        if (end - at >= 2) {
            const auto begin_byte = points[at].begin;
            const auto end_byte = points[end - 1].end;
            keys.insert(unicode_casefold(value.substr(begin_byte, end_byte - begin_byte)));
        }
        at = end;
    }
}

[[nodiscard]] bool exact_keys(const transport::Json& object,
                              const std::initializer_list<std::string_view> expected) {
    if (object.kind != transport::Json::Kind::object || object.keys.size() != expected.size())
        return false;
    for (const auto key : expected)
        if (!object.find(key)) return false;
    return true;
}

[[nodiscard]] bool exact_true(const transport::Json* value) noexcept {
    return value && value->kind == transport::Json::Kind::boolean && value->scalar == "true";
}

}  // namespace

const SessionJsonValue* SessionJsonValue::find(const std::string_view key) const noexcept {
    if (kind != Kind::object) return nullptr;
    for (std::size_t index = 0; index != keys.size(); ++index)
        if (keys[index] == key) return &values[index];
    return nullptr;
}

JsonValue session_json_value(const SessionJsonValue& source) {
    switch (source.kind) {
    case SessionJsonValue::Kind::null: return JsonValue(nullptr);
    case SessionJsonValue::Kind::boolean: return JsonValue(source.scalar == "true");
    case SessionJsonValue::Kind::string: return JsonValue(source.scalar);
    case SessionJsonValue::Kind::number: {
        std::int64_t integer{};
        const auto parsed = std::from_chars(source.scalar.data(),
            source.scalar.data() + source.scalar.size(), integer);
        if (parsed.ec == std::errc{} && parsed.ptr == source.scalar.data() + source.scalar.size())
            return JsonValue(integer);
        return JsonValue(std::strtod(source.scalar.c_str(), nullptr));
    }
    case SessionJsonValue::Kind::array: {
        JsonValue::Array result;
        for (const auto& child : source.values) result.push_back(session_json_value(child));
        return JsonValue(std::move(result));
    }
    case SessionJsonValue::Kind::object: {
        JsonValue::Object result;
        for (std::size_t index = 0; index != source.values.size(); ++index)
            result.emplace(source.keys[index], session_json_value(source.values[index]));
        return JsonValue(std::move(result));
    }
    }
    throw std::logic_error("unknown retained session JSON kind");
}

std::optional<std::string_view> SessionJsonValue::string_field(
    const std::string_view key) const noexcept {
    const auto* value = find(key);
    if (!value || value->kind != Kind::string) return std::nullopt;
    return value->scalar;
}

PreparedSessionArchive::PreparedSessionArchive(
    std::string source_id_value, std::string source_revision_value,
    std::string source_text_sha256_value, std::string original_text_value,
    std::vector<SessionEvent> events_value,
    std::map<std::string, std::vector<std::size_t>, std::less<>> postings_value)
    : source_id(std::move(source_id_value)), source_revision(std::move(source_revision_value)),
      source_text_sha256(std::move(source_text_sha256_value)),
      original_text(std::move(original_text_value)), events(std::move(events_value)),
      postings(std::move(postings_value)) {}

SessionEventSelection PreparedSessionArchive::lookup(
    const std::vector<std::string>& cues, const std::string_view expected_source_id,
    const std::string_view expected_source_revision) const {
    if (expected_source_id != source_id || expected_source_revision != source_revision)
        throw std::invalid_argument("session source binding changed");
    std::vector<std::string> normalized;
    std::set<std::string> seen;
    for (const auto& cue : cues) {
        if (!text(cue)) throw std::invalid_argument("explicit main-selected cue tuple required");
        auto folded = unicode_casefold(cue);
        if (seen.insert(folded).second) normalized.push_back(std::move(folded));
    }
    std::map<std::size_t, std::vector<std::string>> reasons;
    for (const auto& cue : normalized)
        if (const auto found = postings.find(cue); found != postings.end())
            for (const auto ordinal : found->second) reasons[ordinal].push_back(cue);
    std::vector<std::size_t> ordinals;
    ordinals.reserve(reasons.size());
    for (const auto& [ordinal, unused] : reasons) {
        (void)unused;
        ordinals.push_back(ordinal);
    }
    return {source_id, source_revision, source_text_sha256, std::move(normalized),
            std::move(ordinals), std::move(reasons), events.size()};
}

const SessionEvent& PreparedSessionArchive::event(
    const std::size_t ordinal, const std::string_view expected_source_revision) const {
    if (expected_source_revision != source_revision)
        throw std::invalid_argument("session source revision changed");
    if (ordinal >= events.size()) throw std::invalid_argument("source event ordinal required");
    return events[ordinal];
}

std::optional<PreparedSessionArchive> prepare_session_archive(
    std::string source_text, std::string source_id, std::string source_revision,
    std::optional<std::string> expected_text_sha256) {
    if (!text(source_id) || !text(source_revision))
        throw std::invalid_argument("source text and main source binding required");
    std::optional<std::string> digest;
    if (expected_text_sha256) {
        if (!digest_text(*expected_text_sha256))
            throw std::invalid_argument("explicit source document digest required");
        digest = sha256(source_text);
        if (*digest != *expected_text_sha256)
            throw SessionDocumentDigestMismatch("session document digest changed");
    }

    std::pmr::monotonic_buffer_resource memory;
    transport::Json document(&memory);
    try {
        document = transport::parse_json(source_text, memory);
    } catch (const std::invalid_argument&) {
        return std::nullopt;
    } catch (const std::length_error&) {
        return std::nullopt;
    }
    if (!finite_numbers(document) ||
        !exact_keys(document, {"historical_records", "commands_are_inert", "claims_unverified"}) ||
        !exact_true(document.find("commands_are_inert")) ||
        !exact_true(document.find("claims_unverified")))
        return std::nullopt;
    const auto* rows = document.find("historical_records");
    if (!rows || rows->kind != transport::Json::Kind::array) return std::nullopt;

    std::vector<SessionEvent> events;
    std::map<std::string, std::vector<std::size_t>, std::less<>> postings;
    events.reserve(rows->values.size());
    for (std::size_t ordinal = 0; ordinal != rows->values.size(); ++ordinal) {
        const auto& row = rows->values[ordinal];
        if (!exact_keys(row, {"content_sha256", "payload"})) return std::nullopt;
        const auto* declared = row.find("content_sha256");
        const auto* payload = row.find("payload");
        if (!declared || declared->kind != transport::Json::Kind::string ||
            !digest_text(declared->scalar) || !payload ||
            payload->kind != transport::Json::Kind::object)
            return std::nullopt;
        const auto encoded = transport::encode_python_json(*payload);
        events.push_back({ordinal, std::string(declared->scalar), freeze(*payload),
            sha256(encoded) == std::string_view(declared->scalar), prepare_result_meaning(*payload),
            prepare_call_meaning(*payload), prepare_message_meaning(*payload),
            prepare_operation_meaning(*payload)});
        std::vector<std::string_view> strings;
        collect_strings(*payload, strings);
        std::set<std::string> keys;
        for (const auto value : strings) add_tokens(value, keys);
        for (const auto& key : keys) postings[key].push_back(ordinal);
    }
    return PreparedSessionArchive(std::move(source_id), std::move(source_revision),
        digest ? std::move(*digest) : sha256(source_text), std::move(source_text),
        std::move(events), std::move(postings));
}

}  // namespace swegca::world
