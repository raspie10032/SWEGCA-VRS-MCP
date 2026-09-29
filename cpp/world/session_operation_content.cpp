#include "world/session_operation_content.hpp"

#include "world/unicode_nfkc.hpp"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <utility>

namespace swegca::world {
namespace {

[[nodiscard]] bool string_value(const transport::Json* value,
                                std::string_view& result) noexcept {
    if (!value || value->kind != transport::Json::Kind::string) return false;
    result = value->scalar;
    return true;
}

[[nodiscard]] bool integer_token(const transport::Json* value) noexcept {
    return value && value->kind == transport::Json::Kind::number &&
        value->scalar.find_first_of(".eE") == std::string_view::npos;
}

[[nodiscard]] std::string normalized_integer(const std::string_view value) {
    return value == "-0" ? "0" : std::string(value);
}

[[nodiscard]] bool nonnegative_integer(const transport::Json* value,
                                       std::string& normalized) {
    if (!integer_token(value)) return false;
    normalized = normalized_integer(value->scalar);
    return !normalized.starts_with('-');
}

[[nodiscard]] bool nanoseconds_integer(const transport::Json* value,
                                       std::uint32_t& result) {
    std::string normalized;
    if (!nonnegative_integer(value, normalized) || normalized.size() > 9)
        return false;
    const auto converted = std::from_chars(
        normalized.data(), normalized.data() + normalized.size(), result);
    return converted.ec == std::errc{} &&
        converted.ptr == normalized.data() + normalized.size() &&
        result < 1'000'000'000U;
}

[[nodiscard]] SessionDecimalInteger duration_ns(
    const std::string_view seconds, const std::uint32_t nanoseconds) {
    if (seconds == "0") return {std::to_string(nanoseconds)};
    auto suffix = std::to_string(nanoseconds);
    suffix.insert(0, 9 - suffix.size(), '0');
    return {std::string(seconds) + suffix};
}

[[nodiscard]] std::string python_repr(const std::string_view value) {
    static constexpr char hex[] = "0123456789abcdef";
    std::string result{"'"};
    for (const auto raw : value) {
        const auto byte = static_cast<unsigned char>(raw);
        switch (byte) {
        case '\\': result += "\\\\"; break;
        case '\'': result += "\\'"; break;
        case '\t': result += "\\t"; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        default:
            if (byte < 0x20U || byte == 0x7fU) {
                result += "\\x";
                result.push_back(hex[byte >> 4U]);
                result.push_back(hex[byte & 0x0fU]);
            } else {
                result.push_back(raw);
            }
        }
    }
    result.push_back('\'');
    return result;
}

[[nodiscard]] std::string optional_repr(
    const std::optional<std::string>& value) {
    return value ? python_repr(*value) : "None";
}

[[nodiscard]] std::string optional_bool_repr(
    const std::optional<bool> value) {
    if (!value) return "None";
    return *value ? "True" : "False";
}

}  // namespace

SessionOperationMeaning::SessionOperationMeaning(
    std::string kind_value,
    std::optional<std::string> server_value,
    std::optional<std::string> tool_value,
    std::optional<std::string> action_value,
    std::optional<std::string> target_value,
    std::optional<std::string> status_value,
    std::optional<bool> tool_reported_error_value,
    std::optional<SessionDecimalInteger> reported_duration_ns_value,
    std::vector<SessionOperationFieldPath> source_field_paths_value,
    std::vector<std::string> unresolved_value)
    : kind(std::move(kind_value)), server(std::move(server_value)),
      tool(std::move(tool_value)), action(std::move(action_value)),
      target(std::move(target_value)), status(std::move(status_value)),
      tool_reported_error(std::move(tool_reported_error_value)),
      reported_duration_ns(std::move(reported_duration_ns_value)),
      source_field_paths(std::move(source_field_paths_value)),
      unresolved(std::move(unresolved_value)) {}

bool SessionOperationMeaning::addresses_subject(
    const std::string_view subject) const {
    if (!unresolved.empty()) return false;
    const auto folded = unicode_casefold(subject);
    for (const auto* value : {&server, &tool, &target})
        if (*value && unicode_casefold(**value) == folded) return true;
    return false;
}

std::optional<SessionOperationMeaning> prepare_operation_meaning(
    const transport::Json& payload) {
    if (payload.kind != transport::Json::Kind::object) return std::nullopt;
    std::string_view kind;
    if (!string_value(payload.find("type"), kind) ||
        (kind != "McpToolCall" && kind != "web_search_call"))
        return std::nullopt;

    std::vector<std::string> issues;
    std::vector<SessionOperationFieldPath> paths;
    const auto literal = [&](const transport::Json& object,
                             const std::string_view name,
                             SessionOperationFieldPath path)
        -> std::optional<std::string> {
        std::string_view text;
        if (!string_value(object.find(name), text) ||
            strip_unicode_whitespace(text).empty()) {
            std::string joined;
            for (std::size_t index = 0; index < path.size(); ++index) {
                if (index) joined.push_back('.');
                joined += path[index];
            }
            issues.emplace_back("operation_field_not_resolved:" + joined);
            return std::nullopt;
        }
        paths.push_back(std::move(path));
        return std::string(text);
    };

    auto status = literal(payload, "status", {"status"});
    std::optional<std::string> server;
    std::optional<std::string> tool;
    std::optional<std::string> action;
    std::optional<std::string> target;
    std::optional<bool> error;
    std::optional<SessionDecimalInteger> duration;

    if (kind == "McpToolCall") {
        server = literal(payload, "server", {"server"});
        tool = literal(payload, "tool", {"tool"});

        const auto* arguments = payload.find("arguments");
        paths.push_back({"arguments"});
        if (!arguments || arguments->kind != transport::Json::Kind::object ||
            !arguments->keys.empty())
            issues.emplace_back("mcp_arguments_semantics_not_resolved");

        const auto* result = payload.find("result");
        paths.push_back({"result"});
        if (!result || result->kind != transport::Json::Kind::object) {
            issues.emplace_back("mcp_result_shape_not_resolved");
        } else {
            const auto* reported = result->find("isError");
            if (reported && reported->kind == transport::Json::Kind::boolean)
                error = reported->scalar == "true";
            else
                issues.emplace_back("mcp_error_flag_not_resolved");

            const auto* content = result->find("content");
            if (!content || content->kind != transport::Json::Kind::array ||
                !content->values.empty())
                issues.emplace_back("mcp_result_content_semantics_not_resolved");
            for (const auto& key : result->keys)
                if (key != "isError" && key != "content")
                    issues.emplace_back(
                        "mcp_result_field_not_resolved:" + std::string(key));
        }

        if (const auto* duration_value = payload.find("duration")) {
            paths.push_back({"duration"});
            std::string seconds;
            std::uint32_t nanoseconds = 0;
            if (duration_value->kind == transport::Json::Kind::object &&
                duration_value->keys.size() == 2 &&
                duration_value->find("secs") && duration_value->find("nanos") &&
                nonnegative_integer(duration_value->find("secs"), seconds) &&
                nanoseconds_integer(duration_value->find("nanos"), nanoseconds)) {
                duration = duration_ns(seconds, nanoseconds);
            } else {
                issues.emplace_back("mcp_duration_not_resolved");
            }
        }

        for (const auto& key : payload.keys)
            if (key != "type" && key != "server" && key != "tool" &&
                key != "arguments" && key != "result" && key != "status" &&
                key != "duration")
                issues.emplace_back(
                    "operation_field_not_resolved:" + std::string(key));
    } else {
        const auto* request = payload.find("action");
        paths.push_back({"action"});
        if (!request || request->kind != transport::Json::Kind::object) {
            issues.emplace_back("web_action_shape_not_resolved");
        } else {
            action = literal(*request, "type", {"action", "type"});
            if (action && (*action == "open" || *action == "open_page")) {
                target = literal(*request, "url", {"action", "url"});
                for (const auto& key : request->keys)
                    if (key != "type" && key != "url")
                        issues.emplace_back(
                            "web_action_field_not_resolved:" + std::string(key));
            } else {
                issues.emplace_back("web_action_semantics_not_resolved");
            }
        }
        for (const auto& key : payload.keys)
            if (key != "type" && key != "action" && key != "status")
                issues.emplace_back(
                    "operation_field_not_resolved:" + std::string(key));
    }

    return SessionOperationMeaning(std::string(kind), std::move(server),
        std::move(tool), std::move(action), std::move(target), std::move(status),
        std::move(error), std::move(duration), std::move(paths), std::move(issues));
}

std::string session_operation_text(const SessionOperationMeaning& meaning) {
    std::string text;
    if (meaning.kind == "McpToolCall") {
        text = "과거 MCP 사건은 서버 " + optional_repr(meaning.server) +
            ", 도구 " + optional_repr(meaning.tool) +
            "의 요청과 반환을 같은 기록 안에 담고 있다. 기록된 상태는 " +
            optional_repr(meaning.status) + ", 도구 오류 표시는 " +
            optional_bool_repr(meaning.tool_reported_error) + "이다. ";
    } else {
        text = "과거 Web 사건의 기록된 요청 종류는 " +
            optional_repr(meaning.action) + ", 대상은 " +
            optional_repr(meaning.target) + ", 상태 표시는 " +
            optional_repr(meaning.status) + "이다. ";
    }
    if (meaning.reported_duration_ns)
        text += "기록된 소요시간은 " + meaning.reported_duration_ns->value +
            " 정수 나노초다. ";
    text += "이는 출처의 보고이며 실제 작업 성공, 현재 사실이나 실행 권한을 증명하지 않는다.";
    if (!meaning.unresolved.empty())
        text += " 요청 인수·반환 본문 또는 부가조건에 미해결 내용이 남아 있다.";
    return text;
}

}  // namespace swegca::world
