#include "world/session_report_scope.hpp"

#include <cassert>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <vector>

using namespace swegca::world;

namespace {

SemanticAnchor anchor(std::string identifier, const std::int64_t ordinal) {
    return {std::move(identifier), 0,
        {std::string("historical_records"), ordinal},
        "text", "original", {0, 1}, {}, {}};
}

SessionUnresolvedSource unresolved_operation(
    SessionDocumentKey key, std::string episode_id, std::string target) {
    SessionEventObligation obligation{std::move(key), 3,
        {{"session", "turn", std::nullopt, std::nullopt, {}}},
        {"opaque"}, {}, {std::move(target)}};
    return {std::move(episode_id), 0,
        "session_operation_content_not_resolved", std::move(obligation)};
}

void test_none_subject_uses_primary_but_explicit_empty_does_not() {
    const SessionDocumentKey key{"document", "revision"};
    const std::vector<SessionPartialScope> scopes{{key}};
    const std::vector<SessionUnresolvedSource> unresolved{
        unresolved_operation(key, "episode", "requested-tool")};
    const std::map<std::string, SessionReportJudgment, std::less<>> judgments{
        {"episode", {"available"}}};

    const auto default_subjects = select_session_report_context(
        {}, scopes, {}, {}, {}, "requested-tool", unresolved, judgments);
    assert(default_subjects.nonbasis_events.empty());

    const auto explicit_empty = select_session_report_context(
        {}, scopes, {}, {}, {}, "requested-tool", unresolved, judgments,
        std::vector<std::string>{});
    assert((explicit_empty.nonbasis_events ==
            std::set<SessionEventCoordinate>{{key, 3}}));
}

void test_excluded_document_short_circuits_missing_judgment() {
    const SessionDocumentKey key{"document", "revision"};
    const std::vector<SessionUnresolvedSource> unresolved{
        unresolved_operation(key, "missing-judgment", "tool")};
    const auto selected = select_session_report_context(
        {}, {}, {}, {}, {}, "subject", unresolved, {});
    assert(selected.selected_events.empty());
    assert(selected.nonbasis_events.empty());
}

void test_empty_document_key_is_not_a_scope() {
    const SessionDocumentKey empty{"", ""};
    const std::vector<SessionUnresolvedSource> unresolved{
        unresolved_operation(empty, "missing-judgment", "tool")};
    const auto selected = select_session_report_context(
        {}, {{empty}}, {}, {}, {}, "subject", unresolved, {});
    assert(selected.selected_events.empty());
    assert(selected.nonbasis_events.empty());
}

void test_first_anchor_map_is_reused_for_same_source_event() {
    const SessionDocumentKey key{"document", "revision"};
    const std::vector<SessionReportAdoption> adoptions{
        {key, "episode", 7, {anchor("first", 0)}, {"first"}},
        {key, "episode", 7, {anchor("second", 1)}, {"second"}},
    };
    const auto selected = select_session_report_context(
        adoptions, {{key}}, {}, {}, {}, "subject", {}, {});
    assert((selected.selected_events ==
            std::set<SessionEventCoordinate>{{key, 0}}));
}

}  // namespace

int main() {
    test_none_subject_uses_primary_but_explicit_empty_does_not();
    test_excluded_document_short_circuits_missing_judgment();
    test_empty_document_key_is_not_a_scope();
    test_first_anchor_map_is_reused_for_same_source_event();
    std::cout << "PASS session report scope source-order and subject semantics\n";
}
