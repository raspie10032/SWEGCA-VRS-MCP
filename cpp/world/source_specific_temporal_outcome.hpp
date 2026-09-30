#pragma once

#include "world/memory_activation.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace swegca::world {

inline constexpr std::string_view source_specific_temporal_outcome_source_sha256 =
    "d21165b4ec13f68bb087a263c91960283c3cf317ac8b2b06d0a0535c7a4b57c4";
inline constexpr std::string_view temporal_task_family =
    "source-specific-later-visual-stability";
inline constexpr double temporal_stability_threshold = 0.85;

struct TemporalProposal final {
    std::string source_id;
    std::string source_revision_receipt;
    std::string worker_kind;
    std::string status;
    std::string content_or_embedding_sha256;
    std::optional<double> successive_embedding_cosine;
    std::optional<std::pair<std::size_t, std::size_t>> source_dimensions;
    std::optional<std::string> text;
};

struct PrefixTemporalEvidence final {
    std::string source_id;
    std::string source_revision_receipt;
    double median_successive_cosine{};
    double minimum_successive_cosine{};
    double maximum_successive_cosine{};
    bool ocr_observed{};
    std::pair<std::size_t, std::size_t> source_dimensions;
    std::vector<std::string> profile_cues;
    std::vector<std::string> content_cues;
    std::string receipt_sha256;
    [[nodiscard]] std::vector<std::string> candidate_cues() const;
    [[nodiscard]] JsonValue::Object receipt() const;
};

struct LaterVisualStabilityOutcome final {
    std::string source_id;
    std::string source_revision_receipt;
    std::string outcome;
    std::string operational_label;
    double stability_threshold{};
    std::vector<double> successive_cosines;
    std::optional<double> median_successive_cosine;
    std::string receipt_sha256;
    [[nodiscard]] JsonValue::Object receipt() const;
};

struct TemporalAnalogicalJudgment final {
    std::string snapshot_id;
    std::string query;
    std::string status;
    std::optional<std::string> hypothesis;
    std::vector<std::string> selected_episode_ids;
    std::vector<std::string> rejected_episode_ids;
    std::map<std::string, std::string, std::less<>> rejection_reasons;
    std::vector<std::string> historical_outcomes;
    bool current_evidence_required{true};
    bool semantic_authority{};
    bool action_authority{};
    bool persistent_write_authority{};
};

struct TemporalJudgmentResult final {
    RuntimeCueSelection selection;
    MemoryActivationReceipt activation;
    TemporalAnalogicalJudgment judgment;
};

[[nodiscard]] PrefixTemporalEvidence extract_prefix_temporal_evidence(
    const std::vector<TemporalProposal>& proposals);
[[nodiscard]] LaterVisualStabilityOutcome evaluate_later_visual_stability(
    const std::vector<TemporalProposal>& proposals,
    double threshold = temporal_stability_threshold);
[[nodiscard]] MemoryEpisode development_projection_episode(
    const PrefixTemporalEvidence& evidence,
    const LaterVisualStabilityOutcome& outcome,
    std::string prefix_proposals_sha256,
    std::string later_proposals_sha256);
[[nodiscard]] TemporalJudgmentResult judge_later_visual_stability(
    const HotMemoryIndex& memory, const PrefixTemporalEvidence& evidence);

}  // namespace swegca::world
