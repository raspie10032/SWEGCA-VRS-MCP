#include "world/session_report_scope.hpp"

#include "world/unicode_nfkc.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace swegca::world {
namespace {

[[nodiscard]] bool call_content(const std::string_view reason) noexcept {
    return reason == "session_call_qualifications_unresolved" ||
        reason == "session_result_qualifications_unresolved" ||
        reason == "session_result_request_content_unresolved";
}

[[nodiscard]] bool event_content(const std::string_view reason) noexcept {
    return call_content(reason) || reason == "session_message_content_semantics_not_resolved" ||
        reason == "session_operation_content_not_resolved";
}

[[nodiscard]] bool content_obligation(const SessionUnresolvedSource& row) {
    if (!row.event_obligation || !event_content(row.reason) ||
        row.event_obligation->occurrences.empty()) return false;
    for (const auto& link : row.event_obligation->call_links)
        if (link.status != "linked_by_scoped_call_id" &&
            link.status != "counterpart_not_prepared_or_unresolved") return false;
    return !call_content(row.reason) || !row.event_obligation->call_links.empty();
}

[[nodiscard]] bool same(const std::string& left, const std::string& right) {
    return unicode_casefold(left) == unicode_casefold(right);
}

[[nodiscard]] bool names_target(const SessionCallMeaning& meaning,
                                const std::string& subject) {
    for (const auto& value : {meaning.tool_name, meaning.executable})
        if (value && same(*value, subject)) return true;
    return false;
}

[[nodiscard]] bool names_target(const SessionOperationMeaning& meaning,
                                const std::string& subject) {
    for (const auto& value : {meaning.server, meaning.tool, meaning.target})
        if (value && same(*value, subject)) return true;
    return false;
}

[[nodiscard]] bool conflict(const std::vector<SessionAnswerSource>& sources) {
    for (const auto& source : sources) {
        if (!source.current_verdict.is_object()) continue;
        const auto found = source.current_verdict.as_object().find("verdict");
        if (found != source.current_verdict.as_object().end() &&
            std::holds_alternative<std::string>(found->second.storage()) &&
            std::get<std::string>(found->second.storage()) == "conflict") return true;
    }
    return false;
}

}  // namespace

SessionReportSelection::SessionReportSelection(
    std::set<SessionEventCoordinate> selected_events_value,
    std::set<SessionEventCoordinate> nonbasis_events_value)
    : selected_events(std::move(selected_events_value)),
      nonbasis_events(std::move(nonbasis_events_value)) {}

bool SessionReportSelection::covers_obligation(const SessionUnresolvedSource& row) const {
    if (!row.event_obligation) return false;
    const SessionEventCoordinate coordinate{
        row.event_obligation->document_key, row.event_obligation->event_ordinal};
    return nonbasis_events.contains(coordinate) && content_obligation(row);
}

SessionReportSelection select_session_report_context(
    const std::vector<SessionReportAdoption>& adoptions,
    const std::vector<SessionPartialScope>& partial_scopes,
    const std::vector<RecordedSessionMessage>& messages,
    const std::vector<RecordedSessionResult>& results,
    const std::vector<RecordedSessionOperation>& operations,
    std::string subject,
    const std::vector<SessionUnresolvedSource>& unresolved,
    const std::map<std::string, SessionReportJudgment, std::less<>>& judgments,
    std::vector<std::string> subjects) {
    if (subjects.empty()) subjects.push_back(std::move(subject));
    std::set<SessionDocumentKey> documents;
    for (const auto& scope : partial_scopes)
        if (!scope.document_key.first.empty() || !scope.document_key.second.empty())
            documents.insert(scope.document_key);
    std::set<SessionEventCoordinate> selected;
    std::set<SessionDocumentKey> invalid_documents;
    for (const auto& adoption : adoptions) {
        if (!documents.contains(adoption.document_key)) continue;
        std::map<std::string, SemanticAnchor, std::less<>> anchors;
        for (const auto& anchor : adoption.encoding_anchors)
            anchors.emplace(anchor.identifier, anchor);
        if (adoption.referenced_anchors.empty()) invalid_documents.insert(adoption.document_key);
        for (const auto& identifier : adoption.referenced_anchors) {
            const auto found = anchors.find(identifier);
            if (found == anchors.end() || found->second.path.size() < 2 ||
                !std::holds_alternative<std::string>(found->second.path[0]) ||
                std::get<std::string>(found->second.path[0]) != "historical_records" ||
                !std::holds_alternative<std::int64_t>(found->second.path[1]) ||
                std::get<std::int64_t>(found->second.path[1]) < 0) {
                invalid_documents.insert(adoption.document_key);
            } else {
                selected.insert({adoption.document_key,
                    static_cast<std::size_t>(std::get<std::int64_t>(found->second.path[1]))});
            }
        }
    }
    for (const auto& key : invalid_documents) documents.erase(key);
    std::set<SessionEventCoordinate> nonbasis, protected_events;
    std::set<SessionCallKey> protected_calls;
    for (const auto& row : results)
        if (selected.contains({row.document_key, row.event_ordinal}))
            protected_calls.insert(row.call_keys.begin(), row.call_keys.end());
    for (const auto& row : unresolved) {
        if (!row.event_obligation) continue;
        const SessionEventCoordinate coordinate{
            row.event_obligation->document_key, row.event_obligation->event_ordinal};
        bool named = false;
        for (const auto& target : subjects)
            for (const auto& value : row.event_obligation->explicit_request_targets)
                if (same(value, target)) named = true;
        if (selected.contains(coordinate) || named) {
            protected_events.insert(coordinate);
            for (const auto& link : row.event_obligation->call_links)
                protected_calls.insert(link.key);
        }
    }
    const auto consider = [&](const SessionDocumentKey& key, const std::size_t ordinal,
                              const std::vector<SessionAnswerSource>& sources,
                              const std::vector<SessionCallKey>& call_keys,
                              const auto& meaning, const auto& calls) {
        const SessionEventCoordinate coordinate{key, ordinal};
        if (!documents.contains(key) || selected.contains(coordinate) ||
            protected_events.contains(coordinate) || conflict(sources) ||
            std::ranges::any_of(call_keys, [&](const auto& call) {
                return protected_calls.contains(call);
            })) return;
        if (!calls.empty()) {
            for (const auto& call : calls)
                for (const auto& target : subjects)
                    if (call.meaning && names_target(*call.meaning, target)) {
                        protected_calls.insert(call_keys.begin(), call_keys.end());
                        protected_events.insert(coordinate);
                        return;
                    }
        } else {
            for (const auto& target : subjects)
                if constexpr (std::is_same_v<std::decay_t<decltype(meaning)>, SessionOperationMeaning>)
                    if (names_target(meaning, target)) {
                        protected_events.insert(coordinate); return;
                    }
        }
        nonbasis.insert(coordinate);
    };
    for (const auto& row : messages) {
        const SessionEventCoordinate coordinate{row.document_key, row.event_ordinal};
        if (documents.contains(row.document_key) && !selected.contains(coordinate) &&
            !protected_events.contains(coordinate) && !conflict(row.sources))
            nonbasis.insert(coordinate);
    }
    for (const auto& row : results)
        consider(row.document_key, row.event_ordinal, row.sources, row.call_keys,
                 *row.meaning, row.calls);
    for (const auto& row : operations)
        consider(row.document_key, row.event_ordinal, row.sources, {}, *row.meaning,
                 std::vector<RecordedSessionCall>{});
    for (const auto& row : unresolved) {
        if (!row.event_obligation) continue;
        const SessionEventCoordinate coordinate{
            row.event_obligation->document_key, row.event_obligation->event_ordinal};
        const auto judgment = judgments.find(row.episode_id);
        if (!documents.contains(coordinate.first) || selected.contains(coordinate) ||
            protected_events.contains(coordinate) ||
            (judgment != judgments.end() && judgment->second.verdict == "conflict") ||
            std::ranges::any_of(row.event_obligation->call_links, [&](const auto& link) {
                return protected_calls.contains(link.key);
            })) continue;
        if (content_obligation(row)) nonbasis.insert(coordinate);
    }
    return {std::move(selected), std::move(nonbasis)};
}

}  // namespace swegca::world
