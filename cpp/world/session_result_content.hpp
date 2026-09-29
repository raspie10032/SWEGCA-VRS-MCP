#pragma once

#include "transport/json.hpp"
#include "world/session_call_content.hpp"

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace swegca::world {

inline constexpr std::string_view session_result_content_source_sha256 =
    "f5747574eecc66c7e1738e1e61cd7cafca3e87dba658b7f3cb92a28f0b26c68d";

class SessionResultMeaning final {
public:
    SessionResultMeaning(
        std::optional<SessionDecimalInteger> exit_code,
        std::optional<bool> tool_reported_error,
        std::optional<SessionDecimalInteger> process_session_id,
        std::string interpretation,
        std::vector<std::string> unresolved);

    const std::optional<SessionDecimalInteger> exit_code;
    const std::optional<bool> tool_reported_error;
    const std::optional<SessionDecimalInteger> process_session_id;
    const std::string interpretation;
    const std::vector<std::string> unresolved;
};

// COLD recognition of explicit historical tool-return fields. Text output is
// not searched for claims, and no process, tool, model, or transport is called.
[[nodiscard]] std::optional<SessionResultMeaning> prepare_result_meaning(
    const transport::Json& payload);

// Calls must already have been joined by the separate occurrence/index layer.
// Their presence here supplies only recorded request wording, never execution
// proof, task-success proof, current truth, or authority.
[[nodiscard]] std::string session_result_text(
    const SessionResultMeaning& meaning,
    std::span<const SessionCallMeaning> calls = {});

}  // namespace swegca::world
