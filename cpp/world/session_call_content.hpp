#pragma once

#include "transport/json.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace swegca::world {

inline constexpr std::string_view session_call_content_source_sha256 =
    "06ee0cdf201c863af7a2e32b621357c7e4b0f0c26ce03de40d2209470b5f1f16";

struct SessionDecimalInteger final {
    std::string value;
    friend bool operator==(const SessionDecimalInteger&,
                           const SessionDecimalInteger&) = default;
};

using SessionCallOptionValue =
    std::variant<std::string, bool, SessionDecimalInteger>;

struct SessionCallOption final {
    std::string name;
    SessionCallOptionValue value;
    friend bool operator==(const SessionCallOption&, const SessionCallOption&) = default;
};

class SessionCallMeaning final {
public:
    SessionCallMeaning(
        std::optional<std::string> tool_name,
        std::string operation,
        std::optional<std::string> executable,
        std::vector<std::string> arguments,
        std::vector<SessionCallOption> requested_options,
        std::vector<std::string> unresolved,
        std::optional<std::string> name_space = std::nullopt);

    const std::optional<std::string> tool_name;
    const std::string operation;
    const std::optional<std::string> executable;
    const std::vector<std::string> arguments;
    const std::vector<SessionCallOption> requested_options;
    const std::vector<std::string> unresolved;
    const std::optional<std::string> name_space;

    [[nodiscard]] bool addresses_subject(std::string_view subject) const;
};

// COLD reading of a recorded invocation envelope. No process, shell, model, or
// transport call occurs here; complex shell syntax remains unresolved.
[[nodiscard]] std::optional<SessionCallMeaning> prepare_call_meaning(
    const transport::Json& payload);

[[nodiscard]] std::string call_request_text(const SessionCallMeaning& meaning);

}  // namespace swegca::world
