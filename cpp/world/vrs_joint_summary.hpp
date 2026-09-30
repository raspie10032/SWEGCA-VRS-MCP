#pragma once

#include "world/agent_interface.hpp"
#include "world/memory_activation.hpp"
#include "world/semantic_vrs_ingress.hpp"

#include <array>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace swegca::world {

inline constexpr std::string_view vrs_joint_summary_source_sha256 =
    "95e52e741ee35e4c5a361e30d329dc1eb53b3b57c17244921dca9c76f68de52c";
inline constexpr std::string_view vrs_caption_reuse_source_sha256 =
    "e6b3e5f5a4b7b48c60c83aececfcbea58f12307ee487020c98fc3f91ade4c834";
inline constexpr std::string_view joint_summary_schema = "rozephine-joint-summary-v2";
inline constexpr std::string_view joint_summary_reuse_schema = "rozephine-joint-summary-v3";
inline constexpr std::string_view caption_reuse_strategy =
    "source-bound-pixel-caption-reuse-v1";

extern const std::string joint_summary_prompt;

struct JointSummary final {
    std::string summary_id;
    std::string source_id;
    std::string source_revision;
    std::string source_digest;
    std::string outcome;
    std::vector<std::string> source_addresses;
    std::string text;
    std::vector<std::string> quotes;
    std::string profile;
    std::string model;
    std::string derivation_method{"llm-resummary-v2"};
    std::vector<std::pair<std::string, std::string>> reuse_provenance;

    [[nodiscard]] std::vector<std::string> cues() const;
    [[nodiscard]] JsonValue::Object receipt() const;
    [[nodiscard]] MemoryEpisode episode() const;
    friend bool operator==(const JointSummary&, const JointSummary&) = default;
};

[[nodiscard]] std::string joint_summary_canonical(const JsonValue& value);
[[nodiscard]] JsonValue::Object joint_summary_source_payload(
    const SemanticSourceEpisode& episode);
[[nodiscard]] JsonValue::Object joint_summary_input(const JsonValue::Object& payload);
[[nodiscard]] JsonValue::Object validate_summary_settings(const JsonValue& value);
[[nodiscard]] std::optional<JointSummary> reuse_pixel_caption(
    const SemanticSourceEpisode& episode);

struct JointSummaryGeneration final {
    std::vector<JointSummary> summaries;
    JsonValue::Array failures;
};

[[nodiscard]] JointSummaryGeneration generate_joint_summaries(
    const std::vector<SemanticSourceEpisode>& episodes,
    const ProviderRegistry& providers, std::string profile,
    std::string pair_snapshot_id,
    std::map<std::string, JointSummary, std::less<>>* cache = nullptr,
    bool reuse_embedded = true);

struct JointSummaryEdge final {
    std::size_t source{};
    std::size_t target{};
    std::int64_t signal{1};
    double weight{0.75};
};

struct JointSummaryGraph final {
    std::vector<std::string> terms;
    std::map<std::string, std::vector<double>, std::less<>> arrays;
    std::vector<JointSummaryEdge> edges;
    std::vector<double> strengths;
    std::vector<std::size_t> summary_nodes;
    JsonValue::Array receipts;
};

[[nodiscard]] JointSummaryGraph integrate_joint_summaries(
    std::vector<std::string> terms,
    std::map<std::string, std::vector<double>, std::less<>> arrays,
    std::vector<JointSummaryEdge> edges, std::vector<double> strengths,
    const std::vector<SemanticSourceEpisode>& episodes,
    const std::vector<JointSummary>& summaries);

[[nodiscard]] std::vector<MemoryEpisode> restore_joint_summaries(
    const JsonValue::Array& rows,
    const std::vector<SemanticSourceEpisode>& episodes);

}  // namespace swegca::world
