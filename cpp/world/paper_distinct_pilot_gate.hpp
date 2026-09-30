#pragma once

#include "world/vrs_generation_rebind.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace swegca::world {

inline constexpr std::string_view paper_distinct_pilot_gate_source_sha256 =
    "651013322459505454f94f1c6223be3efa948640eb3047cafff58a01b2e16606";

struct DistinctBlindPilotUnit final {
    std::string unit_id;
    std::string evaluation_source_id;
    std::string source_revision_receipt;
    std::string causal_source_experience_id;
};

struct DistinctBlindPilotGateReceipt final {
    BlindPilotGateReceipt base;
    std::string schema_version{"rozephine-paper-distinct-blind-pilot-gate-v1"};
    std::string status{"eligible_for_preregistered_distinct_source_k100_ablation"};
    std::size_t distinct_evaluation_source_count{};
    std::size_t distinct_source_revision_count{};
    std::size_t distinct_causal_source_experience_count{};
    std::vector<DistinctBlindPilotUnit> unit_provenance;
    bool repeated_cues_or_relations_count_as_new_units{};
    bool growth_claimed{};
    bool semantic_authority{};
    bool world_authority{};
    bool action_authority{};
    bool persistent_write_authority{};
    bool model_update_authority{};
    bool distribution_authority{};
    bool p3_authority{};
};

struct PromotionCrossingSourceRow final {
    std::size_t canonical_group_id{};
    std::string source_term;
    std::string target_term;
    std::int8_t sign{};
    double current_strength{};
    double frozen_strength{};
    std::optional<std::string> causal_source_experience_id;
};

struct PromotionSourceDiversityAudit final {
    std::string schema_version{"rozephine-vrs-promotion-source-diversity-audit-v1"};
    std::string status;
    double promotion_threshold{};
    std::size_t current_group_count{};
    std::size_t frozen_group_count{};
    std::size_t new_group_count{};
    std::size_t prefix_cross_up_count{};
    std::size_t prefix_cross_down_count{};
    std::size_t new_promoted_group_count{};
    std::size_t promotion_crossing_group_count{};
    std::size_t distinct_causal_source_experience_count{};
    std::size_t minimum_distinct_causal_source_experience_count{};
    std::size_t unresolved_source_group_count{};
    std::map<std::string, std::size_t, std::less<>> crossing_count_by_causal_source;
    std::vector<PromotionCrossingSourceRow> crossing_groups;
    bool eligible{};
    bool same_source_relations_count_as_distinct_units{};
    bool growth_claimed{};
    bool semantic_authority{};
    bool world_authority{};
    bool action_authority{};
    bool persistent_write_authority{};
    bool model_update_authority{};
    bool distribution_authority{};
    bool p3_authority{};
};

[[nodiscard]] DistinctBlindPilotGateReceipt validate_distinct_blind_pilot_gate(
    const std::vector<CausalEvaluationReceipt>& receipts,
    const std::vector<DistinctBlindPilotUnit>& units,
    std::size_t minimum_units = 3);
[[nodiscard]] PromotionSourceDiversityAudit audit_promotion_crossing_sources(
    const std::vector<std::string>& terms,
    const std::vector<std::uint32_t>& edge_source,
    const std::vector<std::uint32_t>& edge_target,
    const std::vector<std::int8_t>& edge_sign,
    const std::vector<double>& current_strengths,
    const std::vector<double>& frozen_strengths,
    double promotion_threshold = 1.0,
    std::size_t minimum_distinct_sources = 3);

}  // namespace swegca::world
