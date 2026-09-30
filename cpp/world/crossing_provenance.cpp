#include "world/crossing_provenance.hpp"

#include <algorithm>
#include <set>
#include <stdexcept>

namespace swegca::world {
namespace {

void validate(const RelationIdentity& relation) {
    if (relation.source_term.empty() || relation.target_term.empty() ||
        (relation.sign != -1 && relation.sign != 1))
        throw std::invalid_argument("relation identity changed");
}

}  // namespace

CrossingSupportAudit audit_crossing_supports(
    const std::vector<CrossingRow>& crossings,
    const std::vector<RelationSummaryRow>& relation_summary,
    const std::vector<RelationEvidenceRow>& relation_evidence,
    const std::map<std::string, SealedSourceEpisode, std::less<>>& episodes) {
    std::map<RelationIdentity, std::vector<RelationSupport>> evidence;
    for (const auto& row : relation_evidence) {
        validate(row.relation);
        if (row.evidence.empty() || !evidence.emplace(row.relation, row.evidence).second)
            throw std::invalid_argument("duplicate or empty relation evidence");
    }
    std::map<RelationIdentity, RelationSummaryRow> summary;
    for (const auto& row : relation_summary) {
        validate(row.relation);
        if (!summary.emplace(row.relation, row).second)
            throw std::invalid_argument("duplicate relation summary");
    }
    CrossingSupportAudit result;
    std::set<std::size_t> groups;
    std::set<std::string, std::less<>> support_union;
    for (const auto& crossing : crossings) {
        validate(crossing.relation);
        if (!groups.insert(crossing.canonical_group_id).second)
            throw std::invalid_argument("duplicate crossing group");
        std::set<std::string, std::less<>> supports;
        if (crossing.causal_source_experience_id) {
            std::set<std::string, std::less<>> candidates;
            if (crossing.relation.source_term.starts_with("experience:"))
                candidates.insert(crossing.relation.source_term);
            if (crossing.relation.target_term.starts_with("experience:"))
                candidates.insert(crossing.relation.target_term);
            if (candidates != std::set<std::string, std::less<>>{
                    *crossing.causal_source_experience_id} ||
                !episodes.contains(*crossing.causal_source_experience_id))
                throw std::invalid_argument("direct source is not bound to sealed episode");
            supports.insert(*crossing.causal_source_experience_id);
        }
        if (const auto found = evidence.find(crossing.relation); found != evidence.end()) {
            const auto declared = summary.find(crossing.relation);
            if (declared == summary.end() || declared->second.edge_id != crossing.canonical_group_id)
                throw std::invalid_argument("canonical group binding changed");
            std::set<std::string, std::less<>> seen;
            std::set<std::string, std::less<>> source_items;
            for (const auto& support : found->second) {
                const auto episode = episodes.find(support.episode_id);
                if (episode == episodes.end() || !seen.insert(support.episode_id).second ||
                    !source_items.insert(support.source_item_id).second)
                    throw std::invalid_argument("support source missing or duplicated");
                const auto& sealed = episode->second;
                if (sealed.episode_id != support.episode_id ||
                    support.revision != sealed.revision || support.revision.empty() ||
                    support.source_addresses != sealed.source_addresses ||
                    support.source_addresses.empty() || support.outcome != sealed.outcome ||
                    support.source_item_id != sealed.source_item_id)
                    throw std::invalid_argument("support provenance differs from sealed episode");
            }
            std::set<std::string, std::less<>> declared_ids(
                declared->second.episode_ids.begin(), declared->second.episode_ids.end());
            if (declared_ids != seen ||
                declared->second.distinct_source_episode_count != seen.size())
                throw std::invalid_argument("summary support set differs from evidence");
            supports.insert(seen.begin(), seen.end());
        } else if (summary.contains(crossing.relation)) {
            throw std::invalid_argument("summary has no supporting evidence payload");
        }
        support_union.insert(supports.begin(), supports.end());
        CrossingSupportRow row;
        row.canonical_group_id = crossing.canonical_group_id;
        row.relation = crossing.relation;
        row.original_endpoint_source = crossing.causal_source_experience_id;
        row.support_episode_ids.assign(supports.begin(), supports.end());
        row.support_source_count = supports.size();
        row.status = supports.empty() ? "unresolved_in_available_support_artifacts"
                                      : "sealed_support_set_bound";
        result.rows.push_back(std::move(row));
    }
    result.crossing_group_count = result.rows.size();
    for (const auto& row : result.rows) {
        result.original_endpoint_unresolved_count += !row.original_endpoint_source;
        result.support_bound_group_count += !row.support_episode_ids.empty();
        result.remaining_unresolved_group_count += row.support_episode_ids.empty();
        result.unique_source_support_group_count += row.support_source_count == 1;
        result.multi_source_support_group_count += row.support_source_count > 1;
    }
    result.distinct_support_episode_count = support_union.size();
    result.support_episode_ids.assign(support_union.begin(), support_union.end());
    return result;
}

}  // namespace swegca::world
