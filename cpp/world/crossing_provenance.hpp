#pragma once

#include <cstddef>
#include <cstdint>
#include <compare>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace swegca::world {

inline constexpr std::string_view crossing_provenance_source_sha256 =
    "964b0b4ad2da8c42b5c865d0c23e08a25045b4b8f52916815eca71d6ec3eebd9";

struct RelationIdentity final {
    std::string source_term;
    std::string target_term;
    std::int8_t sign{};
    friend bool operator==(const RelationIdentity&, const RelationIdentity&) = default;
    friend auto operator<=>(const RelationIdentity&, const RelationIdentity&) = default;
};

struct RelationSupport final {
    std::string episode_id;
    std::string revision;
    std::vector<std::string> source_addresses;
    std::string source_item_id;
    std::string outcome;
};

struct RelationSummaryRow final {
    RelationIdentity relation;
    std::size_t edge_id{};
    std::vector<std::string> episode_ids;
    std::size_t distinct_source_episode_count{};
};

struct RelationEvidenceRow final {
    RelationIdentity relation;
    std::vector<RelationSupport> evidence;
};

struct CrossingRow final {
    RelationIdentity relation;
    std::size_t canonical_group_id{};
    std::optional<std::string> causal_source_experience_id;
};

struct SealedSourceEpisode final {
    std::string episode_id;
    std::string revision;
    std::vector<std::string> source_addresses;
    std::string outcome;
    std::string source_item_id;
};

struct CrossingSupportRow final {
    std::size_t canonical_group_id{};
    RelationIdentity relation;
    std::optional<std::string> original_endpoint_source;
    std::vector<std::string> support_episode_ids;
    std::size_t support_source_count{};
    std::string status;
    bool unique_source_attribution_claimed{};
    bool causal_effect_claimed{};
};

struct CrossingSupportAudit final {
    std::string schema_version{"rozephine-paper-crossing-support-provenance-audit-v1"};
    std::size_t crossing_group_count{};
    std::size_t original_endpoint_unresolved_count{};
    std::size_t support_bound_group_count{};
    std::size_t remaining_unresolved_group_count{};
    std::size_t unique_source_support_group_count{};
    std::size_t multi_source_support_group_count{};
    std::size_t distinct_support_episode_count{};
    std::vector<std::string> support_episode_ids;
    std::vector<CrossingSupportRow> rows;
    bool runtime_eligibility_changed{};
    bool unique_causal_source_count_inferred_from_supports{};
    std::size_t new_experience_count{};
    bool growth_claimed{};
};

[[nodiscard]] CrossingSupportAudit audit_crossing_supports(
    const std::vector<CrossingRow>& crossings,
    const std::vector<RelationSummaryRow>& relation_summary,
    const std::vector<RelationEvidenceRow>& relation_evidence,
    const std::map<std::string, SealedSourceEpisode, std::less<>>& episodes);

}  // namespace swegca::world
