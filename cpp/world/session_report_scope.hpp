#pragma once

#include "world/session_result_collection.hpp"

#include <cstddef>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace swegca::world {

inline constexpr std::string_view session_report_scope_source_sha256 =
    "104e464b86c3a8abc880ccc7a752aebcbb381630b133766ca8d36655fadf65a8";

using SessionEventCoordinate = std::pair<SessionDocumentKey, std::size_t>;

struct SessionReportAdoption final {
    SessionDocumentKey document_key;
    std::string episode_id;
    std::size_t step{};
    std::vector<SemanticAnchor> encoding_anchors;
    std::vector<std::string> referenced_anchors;
};

struct SessionPartialScope final { SessionDocumentKey document_key; };
struct SessionReportJudgment final { std::string verdict; };

class SessionReportSelection final {
public:
    SessionReportSelection(std::set<SessionEventCoordinate> selected_events,
                           std::set<SessionEventCoordinate> nonbasis_events);
    const std::set<SessionEventCoordinate> selected_events;
    const std::set<SessionEventCoordinate> nonbasis_events;

    [[nodiscard]] bool covers_obligation(const SessionUnresolvedSource& row) const;
};

[[nodiscard]] SessionReportSelection select_session_report_context(
    const std::vector<SessionReportAdoption>& adoptions,
    const std::vector<SessionPartialScope>& partial_scopes,
    const std::vector<RecordedSessionMessage>& messages,
    const std::vector<RecordedSessionResult>& results,
    const std::vector<RecordedSessionOperation>& operations,
    std::string subject,
    const std::vector<SessionUnresolvedSource>& unresolved,
    const std::map<std::string, SessionReportJudgment, std::less<>>& judgments,
    std::vector<std::string> subjects = {});

}  // namespace swegca::world
