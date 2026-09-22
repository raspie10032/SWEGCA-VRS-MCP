#include "session_observation.hpp"

#include "digest.hpp"
#include "observation.hpp"
#include "unicode.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace swegca::vrs {
namespace {

constexpr std::size_t chunk_characters = 60000;

// SWEGCA: src/swegca_vrs2/session_capture.py@c06092a:42-50
std::string serialized_content(const Json& value) {
    if (const auto* text = std::get_if<std::string>(&value.data))
        return *text;
    if (const auto* array = std::get_if<Json::Array>(&value.data);
        array && array->size() == 1 &&
        std::holds_alternative<Json::Object>(array->front().data)) {
        const auto& item = array->front();
        if (item.contains("type") && item.contains("text") &&
            std::holds_alternative<std::string>(item.at("type").data) &&
            std::holds_alternative<std::string>(item.at("text").data)) {
            const auto& kind = item.at("type").string();
            if (kind == "text" || kind == "input_text" ||
                kind == "output_text")
                return item.at("text").string();
        }
    }
    return value.canonical();
}

// SWEGCA: src/swegca_vrs2/session_capture.py@c06092a:52-59
Json without_private_reasoning(const Json& value) {
    if (const auto* object = std::get_if<Json::Object>(&value.data)) {
        Json::Object public_fields;
        for (const auto& [name, item] : *object)
            if (name != "encrypted_content" && name != "reasoning_content")
                public_fields.emplace(name, without_private_reasoning(item));
        return Json(std::move(public_fields));
    }
    if (const auto* array = std::get_if<Json::Array>(&value.data)) {
        Json::Array public_items;
        public_items.reserve(array->size());
        for (const auto& item : *array) {
            if (std::holds_alternative<Json::Object>(item.data) &&
                item.contains("type") &&
                std::holds_alternative<std::string>(item.at("type").data)) {
                const auto& kind = item.at("type").string();
                if (kind == "thinking" || kind == "redacted_thinking" ||
                    kind == "reasoning")
                    continue;
            }
            public_items.emplace_back(without_private_reasoning(item));
        }
        return Json(std::move(public_items));
    }
    return value;
}

// SWEGCA: src/swegca_vrs2/session_capture.py@c06092a:61-111
std::string python_kind(const Json* value) {
    if (!value || std::holds_alternative<std::nullptr_t>(value->data))
        return "None";
    if (const auto* text = std::get_if<std::string>(&value->data))
        return *text;
    if (const auto* boolean = std::get_if<bool>(&value->data))
        return *boolean ? "True" : "False";
    return value->canonical();
}

// SWEGCA: src/swegca_vrs2/session_capture.py@c06092a:316-328
std::vector<std::size_t> chunk_boundaries(std::string_view text) {
    std::vector<std::size_t> coarse{0};
    std::size_t start = 0;
    std::size_t cursor = 0;
    std::size_t characters = 0;
    while (cursor < text.size()) {
        const auto lead = static_cast<unsigned char>(text[cursor]);
        const std::size_t width =
            lead < 0x80 ? 1 : lead < 0xe0 ? 2 : lead < 0xf0 ? 3 : 4;
        if (width > text.size() - cursor)
            throw std::runtime_error("session_content_invalid_utf8");
        cursor += width;
        if (++characters == chunk_characters) {
            (void)decode_utf8(text.substr(start, cursor - start));
            coarse.push_back(cursor);
            start = cursor;
            characters = 0;
        }
    }
    if (start != cursor) {
        (void)decode_utf8(text.substr(start, cursor - start));
        coarse.push_back(cursor);
    }
    std::vector<std::size_t> boundaries{0};
    for (std::size_t part = 1; part < coarse.size(); ++part) {
        const auto begin = coarse[part - 1];
        const auto end = coarse[part];
        const auto points = decode_utf8(text.substr(begin, end - begin));
        const bool only_space = std::all_of(
            points.begin(), points.end(), python_space);
        if (!only_space || points.size() <= 10000) {
            boundaries.push_back(end);
            continue;
        }
        std::size_t at = begin;
        std::size_t count = 0;
        while (at < end) {
            const auto lead = static_cast<unsigned char>(text[at]);
            at += lead < 0x80 ? 1 : lead < 0xe0 ? 2 : lead < 0xf0 ? 3 : 4;
            if (++count == 10000) {
                boundaries.push_back(at);
                count = 0;
            }
        }
        if (boundaries.back() != end) boundaries.push_back(end);
    }
    return boundaries;
}

// SWEGCA: src/swegca_vrs2/session_capture.py@c06092a:316-328
std::int64_t checked_integer(std::uint64_t value) {
    if (value > static_cast<std::uint64_t>(
            std::numeric_limits<std::int64_t>::max()))
        throw std::runtime_error("session_source_span_invalid");
    return static_cast<std::int64_t>(value);
}

}  // namespace

// SWEGCA: src/swegca_vrs2/session_capture.py@c06092a:61-111
std::optional<HostVisibleRecord> host_visible_record(
    SessionHost host, const Json& row) {
    if (!std::holds_alternative<Json::Object>(row.data))
        throw std::runtime_error("transcript_record_invalid");
    const auto* kind = row.contains("type") ? &row.at("type") : nullptr;
    const auto type = python_kind(kind);
    if (host == SessionHost::codex) {
        if (type == "compacted") {
            if (!row.contains("payload") ||
                !std::holds_alternative<Json::Object>(row.at("payload").data) ||
                !row.at("payload").contains("message"))
                return std::nullopt;
            const auto& value = row.at("payload").at("message");
            if (const auto* text = std::get_if<std::string>(&value.data);
                text && !text->empty())
                return HostVisibleRecord{"summary", *text, "compacted"};
            return std::nullopt;
        }
        if (type == "token_usage_record") {
            if (!row.contains("payload") ||
                !std::holds_alternative<Json::Object>(row.at("payload").data) ||
                !row.at("payload").contains("usage"))
                return std::nullopt;
            const auto& usage = row.at("payload").at("usage");
            if (std::holds_alternative<Json::Object>(usage.data))
                return HostVisibleRecord{
                    "usage", serialized_content(usage), type};
            return std::nullopt;
        }
        if (type != "response_item")
            return HostVisibleRecord{
                "session", serialized_content(without_private_reasoning(row)),
                type};
        if (!row.contains("payload") ||
            !std::holds_alternative<Json::Object>(row.at("payload").data))
            throw std::runtime_error("transcript_record_invalid");
        const auto& item = row.at("payload");
        const auto* item_type = item.contains("type") ? &item.at("type") : nullptr;
        const auto subkind = python_kind(item_type);
        if (subkind == "reasoning") {
            if (!item.contains("summary") ||
                !std::holds_alternative<Json::Array>(item.at("summary").data))
                return std::nullopt;
            Json::Array public_parts;
            for (const auto& part : item.at("summary").array()) {
                if (!std::holds_alternative<Json::Object>(part.data) ||
                    !part.contains("type") || !part.contains("text") ||
                    !std::holds_alternative<std::string>(part.at("type").data) ||
                    part.at("type").string() != "summary_text" ||
                    !std::holds_alternative<std::string>(part.at("text").data) ||
                    part.at("text").string().empty())
                    continue;
                Json::Object summary;
                summary.emplace("type", Json(std::string("summary_text")));
                summary.emplace("text", Json(part.at("text").string()));
                public_parts.emplace_back(Json(std::move(summary)));
            }
            if (public_parts.empty()) return std::nullopt;
            return HostVisibleRecord{
                "assistant", serialized_content(Json(std::move(public_parts))),
                "reasoning_summary"};
        }
        if (subkind == "message") {
            if (!item.contains("role") ||
                !std::holds_alternative<std::string>(item.at("role").data))
                return std::nullopt;
            const auto& role = item.at("role").string();
            if (role != "user" && role != "assistant" &&
                role != "system" && role != "developer")
                return std::nullopt;
            if (!item.contains("content") ||
                !std::holds_alternative<Json::Array>(item.at("content").data))
                throw std::runtime_error("transcript_message_invalid");
            const auto public_content =
                without_private_reasoning(item.at("content"));
            if (public_content.array().empty() &&
                !item.at("content").array().empty())
                return std::nullopt;
            auto content = serialized_content(public_content);
            if (content.empty()) return std::nullopt;
            return HostVisibleRecord{role, std::move(content), "message"};
        }
        Json::Object safe;
        for (const auto* name : {"type", "call_id", "name", "input",
                                 "output", "arguments", "status"})
            if (item.contains(name))
                safe.emplace(name, without_private_reasoning(item.at(name)));
        if (!safe.empty())
            return HostVisibleRecord{
                "tool", serialized_content(Json(std::move(safe))), subkind};
        return HostVisibleRecord{
            "session", serialized_content(without_private_reasoning(item)),
            subkind};
    }
    if (type == "user" || type == "assistant") {
        if (!row.contains("message") ||
            !std::holds_alternative<Json::Object>(row.at("message").data))
            throw std::runtime_error("transcript_message_invalid");
        const auto& message = row.at("message");
        if (!message.contains("content") ||
            std::holds_alternative<std::nullptr_t>(message.at("content").data))
            return std::nullopt;
        const auto public_content =
            without_private_reasoning(message.at("content"));
        if (std::holds_alternative<Json::Array>(public_content.data) &&
            public_content.array().empty() &&
            std::holds_alternative<Json::Array>(message.at("content").data) &&
            !message.at("content").array().empty())
            return std::nullopt;
        auto content = serialized_content(public_content);
        if (content.empty()) return std::nullopt;
        return HostVisibleRecord{type, std::move(content), type};
    }
    if (type == "summary") {
        if (row.contains("summary") &&
            std::holds_alternative<std::string>(row.at("summary").data) &&
            !row.at("summary").string().empty())
            return HostVisibleRecord{
                "summary", row.at("summary").string(), type};
        return std::nullopt;
    }
    return HostVisibleRecord{
        "session", serialized_content(without_private_reasoning(row)), type};
}

// SWEGCA: src/swegca_vrs2/session_capture.py@c06092a:316-328
// SWEGCA: user@2026-09-22:54-62
std::uint64_t visit_session_observations_for_line(
    SessionHost host, std::string_view session_key,
    std::string_view transcript_path, std::string_view raw_line,
    std::uint64_t line_start_byte, std::uint64_t line_number,
    const std::function<void(Json&&)>& emit) {
    if (!emit || transcript_path.empty() ||
        session_key.size() != 64 || line_number == 0 ||
        raw_line.empty() || raw_line.back() != '\n')
        throw std::runtime_error("session_source_line_invalid");
    for (char digit : session_key)
        if (!((digit >= '0' && digit <= '9') ||
              (digit >= 'a' && digit <= 'f')))
            throw std::runtime_error("session_source_line_invalid");
    if (raw_line.size() > std::numeric_limits<std::uint64_t>::max() -
            line_start_byte)
        throw std::runtime_error("session_source_span_invalid");
    const auto end_byte = line_start_byte + raw_line.size();
    const auto parsed = Json::parse(raw_line);
    const auto visible = host_visible_record(host, parsed);
    if (!visible || visible->content.empty()) return 0;
    const auto boundaries = chunk_boundaries(visible->content);
    if (boundaries.size() < 2) return 0;
    const auto host_name = host == SessionHost::codex ? "codex" : "claude";
    const auto source_key =
        sha256_hex(transcript_path) + ":" + std::to_string(line_start_byte) +
        ":" + sha256_hex(raw_line);
    const auto content_digest = sha256_hex(visible->content);
    const auto count = boundaries.size() - 1;
    for (std::size_t at = 0; at < count; ++at) {
        const auto original_part = visible->content.substr(
            boundaries[at], boundaries[at + 1] - boundaries[at]);
        const auto points = decode_utf8(original_part);
        const bool only_space = std::all_of(
            points.begin(), points.end(), python_space);
        const auto stored_part = only_space ?
            Json(original_part).canonical() : original_part;
        const auto request_id = sha256_hex(
            "transcript:" + std::string(host_name) + ":" +
            std::string(session_key) + ":" + source_key + ":" +
            std::to_string(at));
        const auto source =
            "transcript:" + std::string(host_name) + ":" +
            std::string(session_key) + ":" + source_key + ":part:" +
            std::to_string(at + 1);
        Json::Object metadata;
        metadata.emplace("origin", Json(std::string("session_transcript")));
        metadata.emplace("host", Json(std::string(host_name)));
        metadata.emplace("role", Json(visible->role));
        metadata.emplace("record_type", Json(visible->record_type));
        metadata.emplace("line", Json(checked_integer(line_number)));
        metadata.emplace("part", Json(checked_integer(at + 1)));
        metadata.emplace("parts", Json(checked_integer(count)));
        metadata.emplace("epistemic_status",
                         Json(std::string("unverified_transcript")));
        metadata.emplace("raw_line_start_byte",
                         Json(checked_integer(line_start_byte)));
        metadata.emplace("raw_line_end_byte", Json(checked_integer(end_byte)));
        metadata.emplace("content_start_byte",
                         Json(checked_integer(boundaries[at])));
        metadata.emplace("content_end_byte",
                         Json(checked_integer(boundaries[at + 1])));
        metadata.emplace("content_sha256", Json(content_digest));
        metadata.emplace("part_sha256", Json(sha256_hex(original_part)));
        metadata.emplace("content_encoding", Json(std::string(
            only_space ? "json_string_literal" : "raw_utf8")));
        Json::Array cues;
        cues.emplace_back(std::string("hook-session:") +
                          std::string(session_key));
        cues.emplace_back(std::string("hook-role:") + visible->role);
        Json::Object arguments;
        arguments.emplace("request_id", Json(request_id));
        arguments.emplace("text", Json(stored_part));
        arguments.emplace("source", Json(source));
        arguments.emplace("revision", Json(std::string("1")));
        arguments.emplace("outcome", Json(std::string("pending")));
        arguments.emplace("cues", Json(std::move(cues)));
        arguments.emplace("metadata", Json(std::move(metadata)));
        emit(observation(Json(std::move(arguments))));
    }
    return checked_integer(count);
}

}  // namespace swegca::vrs
