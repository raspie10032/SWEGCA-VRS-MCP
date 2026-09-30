#pragma once

#include "world/session_content_encoding.hpp"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace swegca::world {

inline constexpr std::string_view semantic_comparison_source_sha256 =
    "45f97f2f82aa9635b044744ec16b348dc551d392744dcbf7faa00d8aca266208";

struct SemanticComparisonRow final {
    const SessionEncodedClaim* claim{};
    const SessionEncodedEvent* event{};
};

struct SemanticComparison final {
    SessionSemanticPropositionKey proposition;
    std::vector<SemanticComparisonRow> affirmed;
    std::vector<SemanticComparisonRow> denied;
    std::vector<SemanticComparisonRow> unknown;
    std::vector<std::pair<std::string, std::string>> parent_sources;
    std::string status;
    bool current_truth_claimed{};
    bool grants_authority{};
};

[[nodiscard]] SessionSemanticPropositionKey prepare_proposition_key(
    const SemanticMeaningUnit& unit);
[[nodiscard]] std::vector<SemanticComparison> compare_semantic_claims(
    const std::vector<SemanticComparisonRow>& rows);
[[nodiscard]] std::string semantic_comparison_text(
    const SemanticComparison& group, bool include_proposition = true);

}  // namespace swegca::world
