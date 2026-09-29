#pragma once

#include "transport/json.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace swegca::world {

inline constexpr std::string_view session_message_content_source_sha256 =
    "b4657bb3fc602b2a238b11aafab9a28913e582177197f9e1d7b9e4d4f2d1e049";

struct MessageTextBlock final {
    std::size_t index{};
    std::string text;
    friend bool operator==(const MessageTextBlock&, const MessageTextBlock&) = default;
};

class SessionMessageMeaning final {
public:
    SessionMessageMeaning(
        std::optional<std::string> role,
        std::optional<std::string> recipient,
        std::vector<MessageTextBlock> text_blocks,
        std::vector<std::string> unresolved);

    const std::optional<std::string> role;
    const std::optional<std::string> recipient;
    const std::vector<MessageTextBlock> text_blocks;
    const std::vector<std::string> unresolved;

    [[nodiscard]] constexpr bool role_is_person_identity() const noexcept { return false; }
    [[nodiscard]] constexpr bool semantic_content_complete() const noexcept { return false; }
    [[nodiscard]] constexpr bool grants_authority() const noexcept { return false; }
};

// COLD structural recognition of the exact historical message envelope.
// Unknown fields remain unresolved; reported text never becomes an instruction,
// a truth verdict, an identity claim, or execution authority.
[[nodiscard]] std::optional<SessionMessageMeaning> prepare_message_meaning(
    const transport::Json& payload);

[[nodiscard]] std::string session_message_text(
    const SessionMessageMeaning& meaning, bool include_literal = true);

}  // namespace swegca::world
