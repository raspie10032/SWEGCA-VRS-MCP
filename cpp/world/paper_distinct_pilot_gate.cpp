#include "world/paper_distinct_pilot_gate.hpp"

#include <algorithm>
#include <set>
#include <stdexcept>

namespace swegca::world {
namespace {

void validate_unit(const DistinctBlindPilotUnit& unit) {
    if (unit.unit_id.empty() || unit.evaluation_source_id.empty() ||
        unit.source_revision_receipt.empty() || unit.causal_source_experience_id.empty())
        throw std::invalid_argument("blind pilot unit provenance is incomplete");
}

std::set<std::string, std::less<>> replayed_endpoint_terms(
    const CausalEvaluationReceipt& receipt) {
    const auto current = std::ranges::find_if(receipt.arms, [](const auto& row) {
        return row.arm == CausalArm::current_vrs;
    });
    if (current == receipt.arms.end())
        throw std::invalid_argument("blind pilot current VRS arm is missing");
    std::set<std::string, std::less<>> result;
    for (const auto& episode : current->memory_activation.replay.episodes) {
        for (const auto& step : episode.steps) {
            for (const auto key : {"source_term", "target_term"}) {
                const auto found = step.observation.find(key);
                if (found != step.observation.end()) {
                    try {
                        const auto value = found->second.as_string();
                        if (!value.empty()) result.emplace(value);
                    } catch (const std::bad_variant_access&) {}
                }
            }
        }
    }
    return result;
}

}  // namespace

DistinctBlindPilotGateReceipt validate_distinct_blind_pilot_gate(
    const std::vector<CausalEvaluationReceipt>& receipts,
    const std::vector<DistinctBlindPilotUnit>& units,
    const std::size_t minimum_units) {
    auto base = validate_blind_pilot_gate(receipts, minimum_units);
    if (units.size() != receipts.size())
        throw std::invalid_argument("blind pilot receipt and unit counts differ");
    std::set<std::string, std::less<>> ids, sources, revisions, causal;
    for (std::size_t i = 0; i < units.size(); ++i) {
        const auto& unit = units[i];
        validate_unit(unit);
        if (!ids.insert(unit.unit_id).second)
            throw std::invalid_argument("blind pilot unit IDs are not distinct");
        if (!sources.insert(unit.evaluation_source_id).second)
            throw std::invalid_argument("blind pilot evaluation sources are not distinct");
        if (!revisions.insert(unit.source_revision_receipt).second)
            throw std::invalid_argument("blind pilot source revisions are not distinct");
        if (!causal.insert(unit.causal_source_experience_id).second)
            throw std::invalid_argument("blind pilot causal source experiences are not distinct");
        if (!replayed_endpoint_terms(receipts[i]).contains(unit.causal_source_experience_id))
            throw std::invalid_argument("blind pilot causal source is not bound to replay");
    }
    DistinctBlindPilotGateReceipt result;
    result.base = std::move(base);
    result.distinct_evaluation_source_count = sources.size();
    result.distinct_source_revision_count = revisions.size();
    result.distinct_causal_source_experience_count = causal.size();
    result.unit_provenance = units;
    return result;
}

PromotionSourceDiversityAudit audit_promotion_crossing_sources(
    const std::vector<std::string>& terms,
    const std::vector<std::uint32_t>& edge_source,
    const std::vector<std::uint32_t>& edge_target,
    const std::vector<std::int8_t>& edge_sign,
    const std::vector<double>& current_strengths,
    const std::vector<double>& frozen_strengths,
    const double promotion_threshold,
    const std::size_t minimum_distinct_sources) {
    const auto current_count = current_strengths.size();
    const auto frozen_count = frozen_strengths.size();
    if (!(promotion_threshold > 0.0) || !minimum_distinct_sources ||
        frozen_count > current_count || edge_source.size() != current_count ||
        edge_target.size() != current_count || edge_sign.size() != current_count)
        throw std::invalid_argument("promotion audit topology or threshold changed");
    for (std::size_t i = 0; i < current_count; ++i)
        if (edge_source[i] >= terms.size() || edge_target[i] >= terms.size() ||
            (edge_sign[i] != -1 && edge_sign[i] != 1))
            throw std::invalid_argument("promotion audit topology or threshold changed");
    std::vector<std::size_t> prefix_up, prefix_down, new_promoted, crossing;
    for (std::size_t i = 0; i < frozen_count; ++i) {
        if (current_strengths[i] >= promotion_threshold && frozen_strengths[i] < promotion_threshold)
            prefix_up.push_back(i);
        if (current_strengths[i] < promotion_threshold && frozen_strengths[i] >= promotion_threshold)
            prefix_down.push_back(i);
    }
    for (std::size_t i = frozen_count; i < current_count; ++i)
        if (current_strengths[i] >= promotion_threshold) new_promoted.push_back(i);
    crossing = prefix_up;
    crossing.insert(crossing.end(), new_promoted.begin(), new_promoted.end());

    PromotionSourceDiversityAudit result;
    result.promotion_threshold = promotion_threshold;
    result.current_group_count = current_count;
    result.frozen_group_count = frozen_count;
    result.new_group_count = current_count - frozen_count;
    result.prefix_cross_up_count = prefix_up.size();
    result.prefix_cross_down_count = prefix_down.size();
    result.new_promoted_group_count = new_promoted.size();
    result.promotion_crossing_group_count = crossing.size();
    result.minimum_distinct_causal_source_experience_count = minimum_distinct_sources;
    for (const auto group : crossing) {
        const auto& source = terms[edge_source[group]];
        const auto& target = terms[edge_target[group]];
        std::set<std::string, std::less<>> candidates;
        if (source.starts_with("experience:")) candidates.insert(source);
        if (target.starts_with("experience:")) candidates.insert(target);
        std::optional<std::string> causal_source;
        if (candidates.size() == 1) {
            causal_source = *candidates.begin();
            ++result.crossing_count_by_causal_source[*causal_source];
        } else {
            ++result.unresolved_source_group_count;
        }
        result.crossing_groups.push_back({group, source, target, edge_sign[group],
            current_strengths[group], group < frozen_count ? frozen_strengths[group] : 0.0,
            std::move(causal_source)});
    }
    result.distinct_causal_source_experience_count =
        result.crossing_count_by_causal_source.size();
    result.eligible = !result.unresolved_source_group_count &&
        result.distinct_causal_source_experience_count >= minimum_distinct_sources &&
        crossing.size() >= minimum_distinct_sources;
    result.status = result.eligible ? "eligible_for_distinct_source_blind_pilot"
                                    : "ineligible_distinct_causal_source_deficit";
    return result;
}

}  // namespace swegca::world
