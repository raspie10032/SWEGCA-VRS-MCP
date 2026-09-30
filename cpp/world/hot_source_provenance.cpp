#include "world/hot_source_provenance.hpp"

#include <algorithm>
#include <cstdint>
#include <set>
#include <stdexcept>

namespace swegca::world {
namespace {

bool digest_id(const std::string_view value) {
    return value.size() == 64 && std::ranges::all_of(value, [](const unsigned char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

const JsonValue* find(const JsonValue::Object& object, const std::string_view key) {
    const auto found = object.find(key);
    return found == object.end() ? nullptr : &found->second;
}

std::string field_string(const JsonValue::Object& object, const std::string_view key) {
    const auto* value = find(object, key);
    if (!value || !std::holds_alternative<std::string>(value->storage()))
        throw std::invalid_argument("relation source observation field changed");
    return std::get<std::string>(value->storage());
}

std::int64_t field_integer(const JsonValue::Object& object, const std::string_view key) {
    const auto* value = find(object, key);
    if (!value || !std::holds_alternative<std::int64_t>(value->storage()))
        throw std::invalid_argument("relation source observation field changed");
    return std::get<std::int64_t>(value->storage());
}

SourceBinding source_binding(const MemoryEpisode& episode) {
    if (episode.steps.size() != 1)
        throw std::invalid_argument("single outcome-bearing source step required for this assay");
    const auto item = field_string(episode.steps.front().observation, "source_item_id");
    if (!episode.episode_id.starts_with("experience:") || episode.revision.empty() ||
        episode.source_addresses.empty() || item.empty())
        throw std::invalid_argument("source provenance incomplete");
    return {episode.episode_id, episode.revision, episode.source_addresses,
            item, episode.steps.front().outcome};
}

RelationIdentity replay_relation(const ReplayedEpisode& replay) {
    if (replay.steps.size() != 1)
        throw std::invalid_argument("single relation step required");
    const auto& observation = replay.steps.front().observation;
    const auto sign = field_integer(observation, "sign");
    if (sign != -1 && sign != 1)
        throw std::invalid_argument("directed relation sign changed");
    return {field_string(observation, "source_term"),
            field_string(observation, "target_term"), static_cast<std::int8_t>(sign)};
}

std::size_t replay_group(const ReplayedEpisode& replay) {
    const auto value = field_integer(replay.steps.front().observation, "canonical_group_id");
    if (value < 0) throw std::invalid_argument("canonical group changed");
    return static_cast<std::size_t>(value);
}

}  // namespace

HotSourceProvenance::HotSourceProvenance(
    std::shared_ptr<const HotMemoryIndex> memory_value, std::string snapshot,
    Bindings bindings_value)
    : memory(std::move(memory_value)), vrs_snapshot_id(std::move(snapshot)),
      bindings(std::move(bindings_value)) {
    if (!memory || memory->lookup_requires_io())
        throw std::invalid_argument("source memory must be hot");
    if (!digest_id(vrs_snapshot_id))
        throw std::invalid_argument("source consumer VRS snapshot must be a SHA-256 digest");
}

std::shared_ptr<const HotSourceProvenance> HotSourceProvenance::build(
    std::shared_ptr<const HotMemoryIndex> memory, std::string snapshot,
    const std::vector<RelationSummaryRow>& summaries,
    const std::vector<RelationEvidenceRow>& evidence) {
    if (!memory || memory->lookup_requires_io())
        throw std::invalid_argument("source memory must be hot");
    if (!digest_id(snapshot))
        throw std::invalid_argument("source ledger VRS snapshot must be a SHA-256 digest");
    std::set<RelationIdentity> summary_keys, evidence_keys;
    std::map<std::string, SourceBinding, std::less<>> sources;
    std::map<std::string, SealedSourceEpisode, std::less<>> episodes;
    for (const auto& row : summaries) summary_keys.insert(row.relation);
    for (const auto& row : evidence) {
        evidence_keys.insert(row.relation);
        for (const auto& support : row.evidence) {
            if (sources.contains(support.episode_id)) continue;
            const auto& episode = memory->episode(support.episode_id);
            auto bound = source_binding(episode);
            episodes.emplace(bound.episode_id, SealedSourceEpisode{bound.episode_id,
                bound.revision, bound.source_addresses, bound.outcome, bound.source_item_id});
            sources.emplace(bound.episode_id, std::move(bound));
        }
    }
    if (summary_keys != evidence_keys)
        throw std::invalid_argument("complete summary and evidence relation sets required");
    std::vector<CrossingRow> crossings;
    crossings.reserve(summaries.size());
    for (const auto& row : summaries)
        crossings.push_back({row.relation, row.edge_id, std::nullopt});
    const auto audit = audit_crossing_supports(crossings, summaries, evidence, episodes);
    Bindings bindings;
    for (const auto& row : audit.rows) {
        std::vector<SourceBinding> bound;
        for (const auto& identifier : row.support_episode_ids) bound.push_back(sources.at(identifier));
        bindings.emplace(row.canonical_group_id,
                         Binding{row.relation, std::move(bound)});
    }
    return std::shared_ptr<const HotSourceProvenance>(
        new HotSourceProvenance(std::move(memory), std::move(snapshot), std::move(bindings)));
}

RelationSources HotSourceProvenance::resolve(const ReplayedEpisode& replay) const {
    try {
        const auto& original = memory->episode(replay.episode_id);
        if (replay.source_addresses != original.source_addresses || replay.steps != original.steps)
            throw std::invalid_argument("replay differs from main-owned experience");
        if (original.revision != "vrs-report-sha256:" + vrs_snapshot_id)
            throw std::invalid_argument("relation VRS generation differs");
        const auto relation = replay_relation(replay);
        const auto group = replay_group(replay);
        const auto declared = bindings.find(group);
        if (declared != bindings.end() && declared->second.first != relation)
            throw std::invalid_argument("directed relation or sign binding differs");
        std::map<std::string, SourceBinding, std::less<>> selected;
        if (declared != bindings.end())
            for (const auto& source : declared->second.second) selected.emplace(source.episode_id, source);
        std::set<std::string, std::less<>> direct;
        if (relation.source_term.starts_with("experience:")) direct.insert(relation.source_term);
        if (relation.target_term.starts_with("experience:")) direct.insert(relation.target_term);
        for (const auto& identifier : direct)
            if (!selected.contains(identifier) && declared != bindings.end())
                throw std::invalid_argument("direct source absent from complete support ledger");
        for (const auto& identifier : direct)
            selected.try_emplace(identifier, source_binding(memory->episode(identifier)));
        for (const auto& [identifier, bound] : selected)
            if (source_binding(memory->episode(identifier)) != bound)
                throw std::invalid_argument("source revision or outcome binding differs");
        if (selected.empty())
            throw std::invalid_argument("no source provenance for selected relation");
        RelationSources result{replay.episode_id, {}, std::nullopt};
        for (const auto& [unused, source] : selected) {
            static_cast<void>(unused); result.sources.push_back(source);
        }
        return result;
    } catch (const std::exception& error) {
        return {replay.episode_id, {}, std::string(error.what())};
    }
}

SourceDecision HotSourceProvenance::explain(
    const HotMemoryIndex& candidate_memory,
    const MemoryActivationReceipt& activation) const {
    if (&candidate_memory != memory.get() || activation.snapshot_id != memory->snapshot_id())
        throw std::invalid_argument("source consumer memory snapshot changed");
    for (const auto& row : activation.recall.candidates)
        if (row.revision != memory->episode(row.episode_id).revision)
            throw std::invalid_argument("recalled source revision changed");
    const auto& evidenced = activation.re_evidence;
    std::set<std::string, std::less<>> replay_ids, judged_ids, selected, expected_support,
        refutation, expected_refutation;
    for (const auto& row : activation.replay.episodes)
        if (!replay_ids.insert(row.episode_id).second)
            throw std::invalid_argument("activation contributor receipt is inconsistent");
    for (const auto& row : evidenced.judgments) {
        if (!judged_ids.insert(row.episode_id).second)
            throw std::invalid_argument("activation contributor receipt is inconsistent");
        if (row.verdict == "support") expected_support.insert(row.episode_id);
        if (row.verdict == "refute") expected_refutation.insert(row.episode_id);
    }
    selected.insert(evidenced.selected_support.begin(), evidenced.selected_support.end());
    refutation.insert(evidenced.selected_refutation.begin(), evidenced.selected_refutation.end());
    std::set<std::string, std::less<>> recalled_ids;
    for (const auto& row : activation.recall.candidates) recalled_ids.insert(row.episode_id);
    if (replay_ids != judged_ids || replay_ids != recalled_ids ||
        selected != expected_support || refutation != expected_refutation)
        throw std::invalid_argument("activation contributor receipt is inconsistent");
    std::vector<RelationSources> contributors;
    for (const auto& row : activation.replay.episodes)
        if (selected.contains(row.episode_id)) contributors.push_back(resolve(row));
    std::vector<std::pair<std::string, std::string>> rejected;
    for (const auto& row : evidenced.judgments)
        if (!selected.contains(row.episode_id)) rejected.emplace_back(row.episode_id, row.verdict);
    std::set<std::string, std::less<>> outcomes;
    for (const auto& row : contributors)
        for (const auto& source : row.sources) outcomes.insert(source.outcome);
    std::string decision = "abstain";
    std::string reason = "no resolved current support";
    if (evidenced.should_abstain || !evidenced.selected_refutation.empty())
        reason = "current re-evidence abstention or refutation retained";
    else if (std::ranges::any_of(contributors, [](const auto& row) { return row.unresolved_reason.has_value(); }))
        reason = "selected contributor provenance unresolved";
    else if (std::ranges::any_of(outcomes, [](const auto& value) {
        return value != "success" && value != "failure";
    })) reason = "nonbinary historical outcome retained without coercion";
    else if (outcomes.size() > 1) reason = "opposing source outcomes retained";
    else if (outcomes.size() == 1) {
        decision = *outcomes.begin(); reason = "current support with consistent source outcomes";
    }
    return {std::string(memory->snapshot_id()), vrs_snapshot_id, std::move(decision),
            std::move(reason), std::move(contributors), std::move(rejected)};
}

std::string HotSourceProvenance::decide(
    const HotMemoryIndex& candidate_memory,
    const MemoryActivationReceipt& activation) const {
    return explain(candidate_memory, activation).decision;
}

ProvenanceEpisodeRoles::ProvenanceEpisodeRoles(
    std::shared_ptr<const HotSourceProvenance> provenance,
    std::set<std::string, std::less<>> repair_source_ids)
    : provenance_(std::move(provenance)), repair_source_ids_(std::move(repair_source_ids)) {
    if (!provenance_) throw std::invalid_argument("source provenance required");
}

EpisodeRole ProvenanceEpisodeRoles::role(const std::string_view episode_id) const {
    const auto& episode = provenance_->memory->episode(episode_id);
    if (episode.steps.empty()) return EpisodeRole::base;
    const ReplayedEpisode replay(episode.episode_id, {}, episode.steps,
        episode.source_addresses, episode.verification_state);
    const auto relation = replay_relation(replay);
    const auto group = replay_group(replay);
    std::set<std::string, std::less<>> identities;
    if (const auto declared = provenance_->bindings.find(group);
        declared != provenance_->bindings.end())
        for (const auto& source : declared->second.second) identities.insert(source.episode_id);
    if (relation.source_term.starts_with("experience:")) identities.insert(relation.source_term);
    if (relation.target_term.starts_with("experience:")) identities.insert(relation.target_term);
    if (identities.empty()) return EpisodeRole::base;
    bool repair{}, base{};
    for (const auto& identity : identities) {
        repair = repair || repair_source_ids_.contains(identity);
        base = base || !repair_source_ids_.contains(identity);
    }
    if (repair && base) return EpisodeRole::other;
    return repair ? EpisodeRole::repair : EpisodeRole::base;
}

}  // namespace swegca::world
