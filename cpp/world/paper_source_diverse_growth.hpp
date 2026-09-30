#pragma once

#include "world/source_specific_temporal_outcome.hpp"

#include <string>
#include <string_view>

namespace swegca::world {

inline constexpr std::string_view paper_source_diverse_growth_source_sha256 =
    "e2bba3a74350ebdd3df5c126d18845e0344bb75a02db42089b48e1c27f81f1fc";

struct PrefixVisualJudgment final {
    std::string source_id;
    std::string source_revision_receipt;
    std::string prefix_evidence_receipt_sha256;
    std::string predicted_operational_label;
    std::string predicted_outcome;
    double threshold{};
    std::string receipt_sha256;
    [[nodiscard]] JsonValue::Object receipt() const;
};

[[nodiscard]] PrefixVisualJudgment prefix_judgment(
    const PrefixTemporalEvidence& evidence, double threshold);
[[nodiscard]] MemoryEpisode build_source_diverse_outcome_episode(
    const PrefixTemporalEvidence& evidence,
    const PrefixVisualJudgment& prefix,
    const LaterVisualStabilityOutcome& outcome,
    std::string source_family,
    std::string source_item_id,
    std::string prefix_proposals_sha256,
    std::string later_proposals_sha256);

}  // namespace swegca::world
