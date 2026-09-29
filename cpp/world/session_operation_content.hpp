#pragma once

#include "transport/json.hpp"
#include "world/session_call_content.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace swegca::world {

inline constexpr std::string_view session_operation_content_source_sha256 =
    "daff9f4e46e36b493798e54867f7217819e4499a72e1fde8a15d6a1ee9f6c13f";

using SessionOperationFieldPath = std::vector<std::string>;

class SessionOperationMeaning final {
public:
    SessionOperationMeaning(
        std::string kind,
        std::optional<std::string> server,
        std::optional<std::string> tool,
        std::optional<std::string> action,
        std::optional<std::string> target,
        std::optional<std::string> status,
        std::optional<bool> tool_reported_error,
        std::optional<SessionDecimalInteger> reported_duration_ns,
        std::vector<SessionOperationFieldPath> source_field_paths,
        std::vector<std::string> unresolved);

    const std::string kind;
    const std::optional<std::string> server;
    const std::optional<std::string> tool;
    const std::optional<std::string> action;
    const std::optional<std::string> target;
    const std::optional<std::string> status;
    const std::optional<bool> tool_reported_error;
    const std::optional<SessionDecimalInteger> reported_duration_ns;
    const std::vector<SessionOperationFieldPath> source_field_paths;
    const std::vector<std::string> unresolved;

    [[nodiscard]] constexpr bool completion_is_success() const noexcept {
        return false;
    }
    [[nodiscard]] constexpr bool grants_authority() const noexcept { return false; }
    [[nodiscard]] bool addresses_subject(std::string_view subject) const;
};

// COLD structural reading of embedded MCP/Web records. Opaque arguments and
// result bodies remain unresolved; no request is executed and no page fetched.
[[nodiscard]] std::optional<SessionOperationMeaning> prepare_operation_meaning(
    const transport::Json& payload);

[[nodiscard]] std::string session_operation_text(
    const SessionOperationMeaning& meaning);

}  // namespace swegca::world
