#include "graph_batch_append.hpp"

#include "digest.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace swegca::vrs {
namespace {

// SWEGCA: src/swegca_vrs2/store.py@c06092a:58-70
bool short_hangul_fragment(std::string_view cue) {
    if (cue.size() < 6 || cue.size() > 12 || cue.size() % 3 != 0)
        return false;
    for (std::size_t at = 0; at < cue.size(); at += 3) {
        const auto first = static_cast<unsigned char>(cue[at]);
        const auto second = static_cast<unsigned char>(cue[at + 1]);
        const auto third = static_cast<unsigned char>(cue[at + 2]);
        if ((first & 0xf0) != 0xe0 || (second & 0xc0) != 0x80 ||
            (third & 0xc0) != 0x80)
            return false;
        const auto point = (std::uint32_t(first & 0x0f) << 12) |
            (std::uint32_t(second & 0x3f) << 6) |
            std::uint32_t(third & 0x3f);
        if (point < 0xac00 || point > 0xd7a3) return false;
    }
    return true;
}

// SWEGCA: src/swegca_vrs2/store.py@c06092a:58-70
std::size_t nonoverlapping_count(std::string_view blob,
                                 std::string_view needle) {
    std::size_t count = 0;
    for (std::size_t at = 0; (at = blob.find(needle, at)) !=
                              std::string_view::npos; at += needle.size())
        ++count;
    return count;
}

// SWEGCA: src/swegca_vrs2/store.py@c06092a:58-70
std::vector<std::string> graph_cues(const MemoryEpisode& episode,
                                     bool substring_cues) {
    std::vector<std::string> all = episode.cues;
    if (std::find(all.begin(), all.end(), episode.episode_id) == all.end())
        all.push_back(episode.episode_id);
    if (substring_cues) return all;
    std::string blob;
    for (const auto& cue : all) {
        if (!blob.empty()) blob.push_back('\0');
        blob += cue;
    }
    std::vector<std::string> filtered;
    filtered.reserve(all.size());
    for (auto& cue : all)
        if (!short_hangul_fragment(cue) ||
            nonoverlapping_count(blob, cue) < 2)
            filtered.push_back(std::move(cue));
    return filtered;
}

// SWEGCA: src/swegca_vrs2/vrs_evidence.py@c06092a:52-62
std::int8_t record_polarity(const MemoryEpisode& episode,
                            const HotIndexRead& memory) {
    if (memory.successor_of(episode.episode_id)) return 0;
    const auto& step = episode.steps.front();
    if (step.outcome == "success") return 1;
    if (step.outcome == "failure") return -1;
    const auto& value = step.observation.at("evidence_polarity");
    if (std::holds_alternative<std::nullptr_t>(value.data)) return 0;
    const auto& polarity = value.string();
    return polarity == "support" ? 1 : polarity == "refute" ? -1 : 0;
}

// SWEGCA: src/swegca_vrs2/store.py@c06092a:461-472
std::string superseded_id(const MemoryEpisode& episode) {
    const auto& value = episode.steps.front().observation.at("supersedes");
    return std::holds_alternative<std::nullptr_t>(value.data) ?
        std::string{} : value.string();
}

// SWEGCA: src/swegca_vrs2/store.py@c06092a:482-509
std::int64_t checked_size(std::uint64_t value) {
    if (value > static_cast<std::uint64_t>(
            std::numeric_limits<std::int64_t>::max()))
        throw std::runtime_error("graph_batch_count_invalid");
    return static_cast<std::int64_t>(value);
}

// SWEGCA: src/swegca_vrs2/store.py@c06092a:482-509
Json batch_receipt(std::string_view parent_snapshot,
                   std::string_view stable_id,
                   std::uint64_t old_nodes, std::uint64_t new_nodes,
                   std::uint64_t total_edges, std::uint64_t stable_edges,
                   std::uint64_t new_edges, std::uint64_t superseded,
                   std::uint64_t resolved, std::span<const MemoryEpisode> episodes) {
    Json::Object receipt;
    receipt.emplace("version", Json(std::string("vrs-regions-consolidation-v1")));
    receipt.emplace("parent_snapshot_id", Json(std::string(parent_snapshot)));
    receipt.emplace("status", Json(std::string("appended_pending_consolidation")));
    receipt.emplace("pending_node_count", Json(std::int64_t{0}));
    receipt.emplace("pending_edges", Json(checked_size(total_edges - stable_edges)));
    receipt.emplace("stable_version_id", Json(std::string(stable_id)));
    receipt.emplace("resolved", episodes.size() == 1 ?
        Json(resolved != 0) : Json(checked_size(resolved)));
    receipt.emplace("superseded_marked", Json(checked_size(superseded)));
    receipt.emplace("legacy_numerical_equivalence", Json(false));
    receipt.emplace("whole_graph_convergence_claimed", Json(false));
    receipt.emplace("logical_implication_claimed", Json(false));
    receipt.emplace("cognitive_completion", Json(false));
    receipt.emplace("persistent_state_mutated", Json(false));
    receipt.emplace("authority_granted", Json(false));
    receipt.emplace("arithmetic_vehicle",
                    Json(std::string("region_consolidation_at_idle")));
    receipt.emplace("changed_component_nodes", Json(checked_size(new_nodes - old_nodes)));
    receipt.emplace("changed_component_edges", Json(checked_size(new_edges)));
    receipt.emplace("region_backend",
                    Json(std::string("deferred_until_idle_consolidation")));
    receipt.emplace("region_sweeps", Json(Json::Array{}));
    receipt.emplace("source_episode_count_added",
                    Json(checked_size(episodes.size())));
    receipt.emplace("historical_outcome",
                    Json(episodes.back().steps.front().outcome));
    receipt.emplace("recorded_agreement_is_not_independent_factual_corroboration",
                    Json(true));
    receipt.emplace("logical_implication_claimed_by_regions", Json(false));
    receipt.emplace("grants_authority", Json(false));
    return Json(std::move(receipt));
}

}  // namespace

// SWEGCA: src/swegca_vrs2/store.py@c06092a:427-512
GraphBatchAppendPlan plan_graph_batch_append(
    std::span<const MemoryEpisode> episodes, std::string_view batch_snapshot,
    const HotIndexRead& memory, const GraphNodeDirectory& nodes,
    const ValidatedEventVrsInputs& current_inputs,
    std::string_view stable_version_id, std::uint64_t stable_edge_count,
    bool graph_substring_cues) {
    if (episodes.empty() || batch_snapshot.empty() ||
        stable_version_id.empty())
        throw std::runtime_error("graph_batch_invalid");
    const auto& old = current_inputs.require_validated_immutable();
    nodes.require_source(old);
    if (nodes.node_count() != old.node_count() ||
        old.node_count() > std::uint64_t{0x100000000ULL} ||
        old.edge_count() > std::uint64_t{0x100000000ULL} ||
        (stable_version_id == "none" && stable_edge_count != 0) ||
        stable_edge_count > old.edge_count())
        throw std::runtime_error("graph_batch_source_changed");
    GraphBatchAppendPlan plan;
    plan.parent_snapshot_id = old.snapshot_id();
    const auto old_nodes = old.node_count();
    std::map<std::string, std::uint32_t> added;
    std::map<std::uint32_t, std::size_t> late_superseded;
    std::uint64_t resolved = 0;
    std::uint64_t superseded = 0;
    Json::Array identifiers;
    for (const auto& episode : episodes) {
        if (!memory.contains_episode(episode.episode_id) ||
            nodes.contains(episode.episode_id) ||
            added.contains(episode.episode_id))
            throw std::runtime_error("graph_batch_episode_changed");
        const auto cues = graph_cues(episode, graph_substring_cues);
        const auto center_address = old_nodes + plan.new_nodes.size();
        if (center_address > std::numeric_limits<std::uint32_t>::max())
            throw std::runtime_error("graph_batch_node_address_exhausted");
        const auto center = static_cast<std::uint32_t>(center_address);
        plan.new_nodes.emplace_back(episode.episode_id, center);
        added.emplace(episode.episode_id, center);
        std::vector<std::uint32_t> fresh;
        std::vector<std::uint32_t> endpoints;
        endpoints.reserve(cues.size());
        for (const auto& cue : cues) {
            const auto name = "cue:" + cue;
            if (const auto found = added.find(name); found != added.end()) {
                endpoints.push_back(found->second);
                continue;
            }
            if (nodes.contains(name)) {
                endpoints.push_back(nodes.address(name));
                continue;
            }
            const auto address = old_nodes + plan.new_nodes.size();
            if (address > std::numeric_limits<std::uint32_t>::max())
                throw std::runtime_error("graph_batch_node_address_exhausted");
            const auto numbered = static_cast<std::uint32_t>(address);
            plan.new_nodes.emplace_back(name, numbered);
            added.emplace(name, numbered);
            fresh.push_back(numbered);
            endpoints.push_back(numbered);
        }
        const auto polarity = record_polarity(episode, memory);
        if (polarity != 0) ++resolved;
        plan.changes.appended_direct.push_back(
            polarity == 0 ? 0.0f : static_cast<float>(std::tanh(1.0)));
        plan.changes.appended_unresolved.push_back(polarity == 0);
        for (std::size_t at = 0; at < fresh.size(); ++at) {
            plan.changes.appended_direct.push_back(0.0f);
            plan.changes.appended_unresolved.push_back(1);
        }
        const auto replaced = superseded_id(episode);
        if (!replaced.empty()) {
            const auto found = added.find(replaced);
            if (found != added.end()) {
                late_superseded[found->second] =
                    static_cast<std::size_t>(found->second - old_nodes);
            } else if (nodes.contains(replaced)) {
                const auto address = nodes.address(replaced);
                plan.changes.direct_edits.emplace_back(address, 0.0f);
                plan.changes.unresolved_edits.emplace_back(address, 1);
                ++superseded;
            }
        }
        if (endpoints.size() >
            (std::uint64_t{0x100000000ULL} - old.edge_count() -
             plan.changes.appended_edges.size()) / 2)
            throw std::runtime_error("graph_batch_edge_address_exhausted");
        for (const auto endpoint : endpoints) {
            const auto sign = polarity == 0 ? std::int8_t{1} : polarity;
            plan.changes.appended_edges.push_back(
                EventEdge{center, endpoint, sign, 0.75f});
            plan.changes.appended_edges.push_back(
                EventEdge{endpoint, center, sign, 0.75f});
            plan.changes.appended_strength.push_back(0.75f);
            plan.changes.appended_strength.push_back(0.75f);
        }
        identifiers.emplace_back(episode.episode_id);
    }
    for (const auto& [address, offset] : late_superseded) {
        if (offset >= plan.changes.appended_direct.size())
            throw std::runtime_error("graph_batch_supersession_changed");
        plan.changes.appended_direct[offset] = 0.0f;
        plan.changes.appended_unresolved[offset] = 1;
        ++superseded;
    }
    plan.changes.appended_score.assign(plan.new_nodes.size(), 0.0f);
    Json::Array identity;
    identity.emplace_back(std::string(batch_snapshot));
    identity.emplace_back(std::string("append"));
    identity.emplace_back(episodes.size() == 1 ?
        Json(episodes.front().episode_id) : Json(std::move(identifiers)));
    const auto total_edges = old.edge_count() +
        plan.changes.appended_edges.size();
    identity.emplace_back(checked_size(total_edges));
    identity.emplace_back(std::string(stable_version_id));
    plan.snapshot_id = sha256_hex(Json(std::move(identity)).canonical());
    plan.receipt = batch_receipt(plan.parent_snapshot_id, stable_version_id,
        old_nodes, old_nodes + plan.new_nodes.size(), total_edges,
        stable_edge_count, plan.changes.appended_edges.size(), superseded,
        resolved, episodes);
    return plan;
}

// SWEGCA: src/swegca_vrs2/store.py@c06092a:479-509
std::shared_ptr<const ValidatedEventVrsInputs>
prepare_graph_batch_event_delta(
    const GraphBatchAppendPlan& plan,
    std::shared_ptr<const ValidatedEventVrsInputs> parent) {
    if (!parent || parent->require_validated_immutable().snapshot_id() !=
                       plan.parent_snapshot_id ||
        plan.changes.appended_direct.size() != plan.new_nodes.size() ||
        plan.changes.appended_unresolved.size() != plan.new_nodes.size() ||
        plan.changes.appended_edges.size() !=
            plan.changes.appended_strength.size())
        throw std::runtime_error("graph_batch_parent_changed");
    return prepare_event_delta(std::move(parent), plan.snapshot_id,
                               plan.changes);
}

}  // namespace swegca::vrs
