#pragma once

#include "world/prepared_session_cache.hpp"

#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace swegca::world {

struct SessionSelectedStep final {
    std::string revision;
    JsonValue current_verdict;
    std::string selection_reason;
};

struct SessionAnswerSource final {
    std::string episode_id;
    std::size_t step{};
    std::size_t variant{};
    std::string revision;
    std::vector<std::string> source_addresses;
    std::string historical_outcome;
    JsonValue current_verdict;
    std::string selection_reason;
};

struct RecordedSessionCall final {
    SessionCallKey call_key;
    std::shared_ptr<const SessionCallMeaning> meaning;
    std::vector<SessionAnswerSource> sources;
    std::vector<SessionSourcePosition> source_positions;
    [[nodiscard]] constexpr bool request_is_execution_proof() const noexcept { return false; }
    [[nodiscard]] constexpr bool grants_authority() const noexcept { return false; }
};

struct RecordedSessionResult final {
    SessionDocumentKey document_key;
    std::size_t event_ordinal{};
    std::shared_ptr<const SessionResultMeaning> meaning;
    std::vector<SessionAnswerSource> sources;
    std::vector<SessionCallKey> call_keys;
    std::vector<std::string> call_link_statuses;
    std::vector<RecordedSessionCall> calls;
    [[nodiscard]] constexpr bool current_truth_claimed() const noexcept { return false; }
    [[nodiscard]] constexpr bool grants_authority() const noexcept { return false; }
};

struct RecordedSessionMessage final {
    SessionDocumentKey document_key;
    std::size_t event_ordinal{};
    std::shared_ptr<const SessionMessageMeaning> meaning;
    std::vector<SessionAnswerSource> sources;
    std::vector<const BoundSessionOccurrence*> occurrences;
    [[nodiscard]] constexpr bool current_truth_claimed() const noexcept { return false; }
    [[nodiscard]] constexpr bool grants_authority() const noexcept { return false; }
};

struct RecordedSessionOperation final {
    SessionDocumentKey document_key;
    std::size_t event_ordinal{};
    std::shared_ptr<const SessionOperationMeaning> meaning;
    std::vector<SessionAnswerSource> sources;
    std::vector<const BoundSessionOccurrence*> occurrences;
    std::string binding{"fields_within_same_source_event_not_cross_event_causality"};
    [[nodiscard]] constexpr bool current_truth_claimed() const noexcept { return false; }
    [[nodiscard]] constexpr bool grants_authority() const noexcept { return false; }
};

struct SessionEventOccurrenceAddress final {
    std::string session;
    std::string turn;
    std::optional<SessionCallKey> call_key;
    std::optional<std::string> role;
    SessionSourcePosition source_position;
};

struct SessionCallLinkStatus final {
    SessionCallKey key;
    std::string status;
};

struct SessionEventObligation final {
    SessionDocumentKey document_key;
    std::size_t event_ordinal{};
    std::vector<SessionEventOccurrenceAddress> occurrences;
    std::vector<std::string> details;
    std::vector<SessionCallLinkStatus> call_links;
    std::vector<std::string> explicit_request_targets;
    [[nodiscard]] constexpr bool grants_authority() const noexcept { return false; }
};

struct SessionUnresolvedSource final {
    std::string episode_id;
    std::size_t step{};
    std::string reason;
    std::optional<SessionEventObligation> event_obligation;
};

struct SessionResultCollection final {
    std::vector<RecordedSessionResult> results;
    std::vector<SessionUnresolvedSource> unresolved;
    std::vector<RecordedSessionMessage> messages;
    std::vector<RecordedSessionOperation> operations;
    // Keeps every occurrence pointer above valid for this collection lifetime.
    std::vector<PreparedSessionEntryPtr> retained_entries;
};

[[nodiscard]] SessionResultCollection collect_session_results(
    const PreparedSessionView& view,
    const std::map<std::pair<std::string, std::size_t>, SessionSelectedStep>& selected_steps,
    bool include_messages = false,
    bool include_event_obligations = false,
    bool include_operations = false);

}  // namespace swegca::world
