#include "four_stage_read.hpp"

#include <algorithm>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace swegca::vrs {
namespace {

// SWEGCA: src/swegca_vrs2/store.py@c06092a:1728-1735
std::optional<std::string> optional_observation(const Json& observation,
                                                 std::string_view key) {
    if (!observation.contains(key)) return std::nullopt;
    const auto& value = observation.at(key);
    if (std::holds_alternative<std::nullptr_t>(value.data)) return std::nullopt;
    return value.string();
}

// SWEGCA: src/swegca_vrs2/store.py@c06092a:1728-1735
std::map<std::string, std::vector<std::string>> opposing_originals(
    const PublishedHotIndex& memory, const ReplayResult& replayed) {
    std::set<std::string> propositions;
    for (const auto& episode : replayed.episodes) {
        if (episode.steps.empty()) throw std::runtime_error("replayed original has no step");
        const auto proposition = optional_observation(
            episode.steps.front().observation, "proposition_id");
        if (proposition) propositions.insert(*proposition);
    }
    std::map<std::string, std::vector<std::string>> opponents;
    for (const auto& proposition : propositions) {
        std::set<std::string> polarities;
        std::set<std::string> active;
        bool missing_polarity = false;
        for (const auto& identifier : memory.proposition_ids(proposition)) {
            if (memory.successor_of(identifier)) continue;
            const auto header = memory.episode_header(identifier);
            if (header.episode_id != identifier || header.proposition_id != proposition)
                throw std::runtime_error("current proposition directory changed");
            if (header.evidence_polarity) polarities.insert(*header.evidence_polarity);
            else missing_polarity = true;
            active.insert(identifier);
        }
        if (!missing_polarity &&
            polarities == std::set<std::string>{"support", "refute"})
            opponents.emplace(proposition,
                              std::vector<std::string>(active.begin(), active.end()));
    }
    return opponents;
}

// SWEGCA: src/swegca_vrs2/store.py@c06092a:1738-1746
CurrentEvidenceVerdict judge_selected_original(
    const ReplayedEpisode& episode, const PublishedHotIndex& memory,
    const EventVrsInputView& inputs, const GraphNodeDirectory& nodes,
    const std::map<std::string, std::vector<std::string>>& opponents) {
    if (episode.steps.empty()) throw std::runtime_error("replayed original has no step");
    const auto proposition = optional_observation(
        episode.steps.front().observation, "proposition_id");
    if (proposition) {
        const auto found = opponents.find(*proposition);
        if (found != opponents.end() && !memory.successor_of(episode.episode_id)) {
            std::vector<std::string> refs{"memory-snapshot:" + memory.snapshot_id()};
            refs.insert(refs.end(), episode.source_addresses.begin(), episode.source_addresses.end());
            return CurrentEvidenceVerdict(episode.episode_id, *proposition, "conflict",
                "Opposing recorded claims for the same explicit proposition; neither is certified true.",
                std::move(refs), found->second);
        }
    }
    return current_experience_verdict(episode, memory.snapshot_id(),
        inputs.snapshot_id(), graph_strength(episode.episode_id, inputs, nodes),
        proposition);
}

}  // namespace

// SWEGCA: src/swegca_vrs2/store.py@c06092a:1719-1768
FullFourStageRead finish_selected_four_stage_read(
    PortalNavigationRecall navigation, const PinnedReadLayer& layer) {
    const auto& pair = layer.pair;
    const auto& inputs = layer.inputs;
    const auto& nodes = layer.nodes;
    const auto& memory = pair.memory();
    if (navigation.recall.snapshot_id != memory.snapshot_id() ||
        navigation.signal.snapshot_id != memory.snapshot_id() ||
        pair.vrs_snapshot_id() != inputs.snapshot_id())
        throw std::runtime_error("four-stage read generation changed");
    nodes.require_source(inputs);
    const auto publication = layer.original_addresses.publication();
    if (!publication || publication->pair_snapshot_id != pair.snapshot_id() ||
        publication->published_rows != layer.published_row_limit)
        throw std::runtime_error("four-stage original read generation changed");
    const OriginalReplayReader read_original = [&](std::string_view identifier) {
        return replay_exact_journal_original(
            layer.journal, layer.original_addresses, identifier,
            layer.published_row_limit);
    };
    auto opened = select_replay_original(memory, navigation.recall);
    auto replayed = replay_memory(memory, opened, read_original);
    const auto opponents = opposing_originals(memory, replayed);
    const auto judge = [&](const ReplayedEpisode& episode) {
        return judge_selected_original(episode, memory, inputs, nodes, opponents);
    };
    auto evaluated = re_evidence_memory(replayed, judge);
    std::optional<RecallResult> expanded_opened;
    if (!evaluated.conflicting_propositions.empty()) {
        std::vector<RecallCandidate> candidates = opened.candidates;
        std::vector<ReplayedEpisode> episodes = replayed.episodes;
        std::set<std::string> opened_ids;
        for (const auto& episode : episodes) opened_ids.insert(episode.episode_id);
        for (const auto& selected : replayed.episodes) {
            const auto proposition = optional_observation(
                selected.steps.front().observation, "proposition_id");
            if (!proposition || std::find(evaluated.conflicting_propositions.begin(),
                                          evaluated.conflicting_propositions.end(),
                                          *proposition) == evaluated.conflicting_propositions.end())
                continue;
            const auto selected_polarity = optional_observation(
                selected.steps.front().observation, "evidence_polarity");
            for (const auto& identifier : opponents.at(*proposition)) {
                if (opened_ids.contains(identifier)) continue;
                const auto header = memory.episode_header(identifier);
                if (header.evidence_polarity == selected_polarity) continue;
                RecallCandidate candidate{identifier, {}, 0.0, header.revision,
                                          header.verification_state, header.historical_outcomes};
                auto one = RecallResult(opened.query, {candidate}, opened.snapshot_id,
                                        opened.source_dependencies);
                auto one_replay = replay_memory(memory, one, read_original);
                if (one_replay.episodes.size() != 1)
                    throw std::runtime_error("opposing original Replay changed");
                candidates.push_back(std::move(candidate));
                episodes.push_back(std::move(one_replay.episodes.front()));
                opened_ids.insert(identifier);
            }
        }
        expanded_opened.emplace(opened.query, std::move(candidates), opened.snapshot_id,
                                opened.source_dependencies);
        replayed = ReplayResult(replayed.query, std::move(episodes));
        evaluated = re_evidence_memory(replayed, judge);
    }
    const auto& receipt_recall = expanded_opened ? *expanded_opened : opened;
    auto activation = MemoryActivationReceipt(
        "rozephine-memory-activation-v1", memory.snapshot_id(),
        navigation.signal, receipt_recall, std::move(replayed),
        std::move(evaluated));
    return FullFourStageRead{std::move(navigation), std::move(activation)};
}

}  // namespace swegca::vrs
