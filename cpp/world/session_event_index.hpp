#pragma once

#include "world/session_call_content.hpp"
#include "world/cognitive_state.hpp"
#include "world/session_message_content.hpp"
#include "world/session_operation_content.hpp"
#include "world/session_result_content.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace swegca::world {

inline constexpr std::string_view session_event_index_source_sha256 =
    "0a338c1531352c37a2bbd1414c9810fa39087eb48fd7821282b789462377d2e8";

class SessionDocumentDigestMismatch final : public std::invalid_argument {
public:
    using std::invalid_argument::invalid_argument;
};

// Owned, insertion-order-preserving JSON retained from the historical source.
// Number spellings remain source exact; no payload field is normalized.
struct SessionJsonValue final {
    enum class Kind : std::uint8_t { null, boolean, number, string, array, object };
    Kind kind{Kind::null};
    std::string scalar;
    std::vector<SessionJsonValue> values;
    std::vector<std::string> keys;

    [[nodiscard]] const SessionJsonValue* find(std::string_view key) const noexcept;
    [[nodiscard]] std::optional<std::string_view> string_field(std::string_view key) const noexcept;
    friend bool operator==(const SessionJsonValue&, const SessionJsonValue&) = default;
};

[[nodiscard]] JsonValue session_json_value(const SessionJsonValue& value);

struct SessionEvent final {
    std::size_t ordinal{};
    std::string declared_content_sha256;
    SessionJsonValue payload;
    bool content_digest_matches{false};
    std::optional<SessionResultMeaning> result_meaning;
    std::optional<SessionCallMeaning> call_meaning;
    std::optional<SessionMessageMeaning> message_meaning;
    std::optional<SessionOperationMeaning> operation_meaning;
};

struct SessionEventSelection final {
    std::string source_id;
    std::string source_revision;
    std::string source_text_sha256;
    std::vector<std::string> query_cues;
    std::vector<std::size_t> selected_ordinals;
    std::map<std::size_t, std::vector<std::string>> match_reasons;
    std::size_t total_events{};
    std::string unselected_reason{
        "no_exact_lexical_posting_match_not_semantic_rejection"};

    [[nodiscard]] constexpr bool semantic_relevance_verified() const noexcept { return false; }
    [[nodiscard]] constexpr bool grants_authority() const noexcept { return false; }
    [[nodiscard]] constexpr std::size_t new_experience_count() const noexcept { return 0; }
};

class PreparedSessionArchive final {
public:
    PreparedSessionArchive(std::string source_id, std::string source_revision,
        std::string source_text_sha256, std::string original_text,
        std::vector<SessionEvent> events,
        std::map<std::string, std::vector<std::size_t>, std::less<>> postings);

    const std::string source_id;
    const std::string source_revision;
    const std::string source_text_sha256;
    const std::string original_text;
    const std::vector<SessionEvent> events;
    const std::map<std::string, std::vector<std::size_t>, std::less<>> postings;

    [[nodiscard]] constexpr bool commands_are_inert() const noexcept { return true; }
    [[nodiscard]] constexpr bool claims_unverified() const noexcept { return true; }
    [[nodiscard]] SessionEventSelection lookup(
        const std::vector<std::string>& cues,
        std::string_view expected_source_id,
        std::string_view expected_source_revision) const;
    [[nodiscard]] const SessionEvent& event(
        std::size_t ordinal, std::string_view expected_source_revision) const;
};

// Explicit COLD preparation. nullopt means opaque, never absent experience.
[[nodiscard]] std::optional<PreparedSessionArchive> prepare_session_archive(
    std::string text, std::string source_id, std::string source_revision,
    std::optional<std::string> expected_text_sha256 = std::nullopt);

}  // namespace swegca::world
