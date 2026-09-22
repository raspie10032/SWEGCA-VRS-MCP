#include "memory_evidence.hpp"

#include "unicode.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace swegca::vrs {
namespace {

// SWEGCA: src/swegca_vrs2/engine/mosaic_memory_activation.py@7536139:15-19
bool nonblank(std::string_view value) {
    const auto points = decode_utf8(value);
    return std::any_of(points.begin(), points.end(), [](auto point) {
        return !python_space(point);
    });
}

// SWEGCA: src/swegca_vrs2/engine/mosaic_memory_activation.py@7536139:10-10
bool valid_verdict(std::string_view verdict) {
    constexpr std::array<std::string_view, 6> verdicts{
        "support", "refute", "insufficient", "conflict", "available", "retained"};
    return std::find(verdicts.begin(), verdicts.end(), verdict) != verdicts.end();
}

// SWEGCA: src/swegca_vrs2/engine/mosaic_memory_activation.py@7536139:497-515
std::vector<std::string> expected_conflicts(
    const std::vector<CurrentEvidenceVerdict>& judgments) {
    std::set<std::string> explicit_conflicts;
    std::set<std::string> support;
    std::set<std::string> refute;
    for (const auto& row : judgments) {
        if (row.verdict == "conflict") explicit_conflicts.insert(row.proposition);
        if (row.verdict == "support") support.insert(row.proposition);
        if (row.verdict == "refute") refute.insert(row.proposition);
    }
    for (const auto& proposition : support)
        if (refute.contains(proposition)) explicit_conflicts.insert(proposition);
    return {explicit_conflicts.begin(), explicit_conflicts.end()};
}

}  // namespace

// SWEGCA: src/swegca_vrs2/engine/mosaic_memory_activation.py@7536139:350-364
ReplayedEpisode::ReplayedEpisode(std::string new_episode_id,
                                 std::vector<std::string> new_matched_cues, std::vector<MemoryStep> new_steps,
                                 std::vector<std::string> new_source_addresses, std::string new_verification_state,
                                 bool new_historical_truth_authorized)
    : episode_id(std::move(new_episode_id)), matched_cues(std::move(new_matched_cues)),
      steps(std::move(new_steps)), source_addresses(std::move(new_source_addresses)),
      verification_state(std::move(new_verification_state)),
      historical_truth_authorized(new_historical_truth_authorized) {
    if (!nonblank(verification_state))
        throw std::runtime_error("replayed verification state must not be empty");
    if (historical_truth_authorized)
        throw std::runtime_error("replay is reconstruction, not historical truth");
}

// SWEGCA: src/swegca_vrs2/engine/mosaic_memory_activation.py@7536139:366-375
ReplayResult::ReplayResult(std::string new_query,
                           std::vector<ReplayedEpisode> new_episodes,
                           bool new_action_authorized,
                           bool new_persistent_write_authorized)
    : query(std::move(new_query)), episodes(std::move(new_episodes)),
      action_authorized(new_action_authorized),
      persistent_write_authorized(new_persistent_write_authorized) {
    if (action_authorized || persistent_write_authorized)
        throw std::runtime_error("replay grants no authority");
}

// SWEGCA: src/swegca_vrs2/engine/mosaic_memory_activation.py@7536139:378-398
ReplayResult replay_memory(const PublishedHotIndex& index, const RecallResult& selected) {
    if (selected.snapshot_id != index.snapshot_id())
        throw std::runtime_error("recall snapshot changed before replay");
    // SWEGCA: user@2026-09-22:24-29
    if (selected.candidates.size() > 1)
        throw std::runtime_error("replay requires one selected original");
    std::vector<ReplayedEpisode> episodes;
    for (const auto& candidate : selected.candidates) {
        const auto source = index.episode(candidate.episode_id);
        episodes.emplace_back(candidate.episode_id, candidate.matched_cues,
                              source.steps, source.source_addresses,
                              source.verification_state);
    }
    return ReplayResult(selected.query, std::move(episodes));
}

// SWEGCA: src/swegca_vrs2/engine/mosaic_memory_activation.py@7536139:401-429
CurrentEvidenceVerdict::CurrentEvidenceVerdict(
    std::string new_episode_id, std::string new_proposition,
    std::string new_verdict, std::string new_rationale,
    std::vector<std::string> new_current_evidence_refs,
    std::vector<std::string> new_contradiction_refs)
    : episode_id(std::move(new_episode_id)), proposition(std::move(new_proposition)),
      verdict(std::move(new_verdict)), rationale(std::move(new_rationale)),
      current_evidence_refs(std::move(new_current_evidence_refs)),
      contradiction_refs(std::move(new_contradiction_refs)) {
    if (!nonblank(episode_id)) throw std::runtime_error("re-evidence episode_id must not be empty");
    if (!nonblank(proposition)) throw std::runtime_error("re-evidence proposition must not be empty");
    if (!valid_verdict(verdict)) throw std::runtime_error("unsupported re-evidence verdict");
    if (!nonblank(rationale)) throw std::runtime_error("re-evidence rationale must not be empty");
    if ((verdict == "support" || verdict == "refute") && current_evidence_refs.empty())
        throw std::runtime_error("support or refute requires current evidence");
    if ((verdict == "available" || verdict == "retained") && current_evidence_refs.empty())
        throw std::runtime_error("experience availability requires current snapshot provenance");
    if (verdict == "conflict" && current_evidence_refs.empty() &&
        contradiction_refs.empty())
        throw std::runtime_error("conflict requires current or contradiction evidence");
    for (const auto& ref : current_evidence_refs)
        if (!nonblank(ref)) throw std::runtime_error("current evidence provenance changed");
    for (const auto& ref : contradiction_refs)
        if (!nonblank(ref)) throw std::runtime_error("contradiction evidence provenance changed");
}

// SWEGCA: src/swegca_vrs2/engine/mosaic_memory_activation.py@7536139:432-458
CurrentEvidenceVerdict current_experience_verdict(
    const ReplayedEpisode& episode, std::string memory_snapshot_id,
    std::string vrs_snapshot_id, std::optional<double> current_strength,
    std::optional<std::string> proposition) {
    if (current_strength && (!std::isfinite(*current_strength) || *current_strength < 0))
        throw std::runtime_error("invalid current VRS strength");
    if (!nonblank(memory_snapshot_id))
        throw std::runtime_error("current memory snapshot must not be empty");
    if (!nonblank(vrs_snapshot_id))
        throw std::runtime_error("current VRS snapshot must not be empty");
    const bool retained = current_strength && *current_strength >= 1.0;
    std::vector<std::string> refs{"memory-snapshot:" + memory_snapshot_id,
                                  "vrs-snapshot:" + vrs_snapshot_id};
    refs.insert(refs.end(), episode.source_addresses.begin(), episode.source_addresses.end());
    return CurrentEvidenceVerdict(
        episode.episode_id,
        proposition && !proposition->empty() ? *proposition : "experience:" + episode.episode_id,
        retained ? "retained" : "available",
        retained ? "current VRS promotion retained; absence of fresh evidence is not refutation"
                 : "record available with original uncertainty; not promoted or fresh factual support",
        std::move(refs));
}

// SWEGCA: src/swegca_vrs2/engine/mosaic_memory_activation.py@7536139:461-516
ReEvidenceResult::ReEvidenceResult(
    std::string new_query, std::vector<CurrentEvidenceVerdict> new_judgments,
    std::vector<std::string> new_selected_support, std::vector<std::string> new_selected_refutation,
    std::vector<std::string> new_conflicting_propositions, bool new_unresolved_conflict,
    bool new_insufficient_evidence, bool new_should_abstain, bool new_action_authorized,
    bool new_persistent_write_authorized, bool new_semantic_promotion_authorized) {
    query = std::move(new_query);
    judgments = std::move(new_judgments);
    selected_support = std::move(new_selected_support);
    selected_refutation = std::move(new_selected_refutation);
    conflicting_propositions = std::move(new_conflicting_propositions);
    unresolved_conflict = new_unresolved_conflict;
    insufficient_evidence = new_insufficient_evidence;
    should_abstain = new_should_abstain;
    action_authorized = new_action_authorized;
    persistent_write_authorized = new_persistent_write_authorized;
    semantic_promotion_authorized = new_semantic_promotion_authorized;
    if (action_authorized || persistent_write_authorized || semantic_promotion_authorized)
        throw std::runtime_error("re-evidence authority changed");
    if (should_abstain != (unresolved_conflict || insufficient_evidence))
        throw std::runtime_error("conflict or insufficient re-evidence must abstain");
    std::set<std::string> identifiers;
    std::vector<std::string> expected_support;
    std::vector<std::string> expected_refutation;
    for (const auto& row : judgments) {
        if (!identifiers.insert(row.episode_id).second)
            throw std::runtime_error("re-evidence judged an episode more than once");
        if (row.verdict == "support") expected_support.push_back(row.episode_id);
        if (row.verdict == "refute") expected_refutation.push_back(row.episode_id);
    }
    if (selected_support != expected_support)
        throw std::runtime_error("re-evidence support selection changed");
    if (selected_refutation != expected_refutation)
        throw std::runtime_error("re-evidence refutation selection changed");
    if (conflicting_propositions != expected_conflicts(judgments))
        throw std::runtime_error("re-evidence conflict selection changed");
}

// SWEGCA: src/swegca_vrs2/engine/mosaic_memory_activation.py@7536139:518-555
ReEvidenceResult re_evidence_memory(const ReplayResult& replayed,
                                     const EvidenceJudge& judge) {
    std::vector<CurrentEvidenceVerdict> judgments;
    judgments.reserve(replayed.episodes.size());
    for (const auto& episode : replayed.episodes) {
        auto judgment = judge(episode);
        if (judgment.episode_id != episode.episode_id)
            throw std::runtime_error("re-evidence judgment episode identity changed");
        judgments.push_back(std::move(judgment));
    }
    std::vector<std::string> support;
    std::vector<std::string> refutation;
    bool usable_experience = false;
    for (const auto& row : judgments) {
        if (row.verdict == "support") support.push_back(row.episode_id);
        if (row.verdict == "refute") refutation.push_back(row.episode_id);
        if (row.verdict == "available" || row.verdict == "retained") usable_experience = true;
    }
    auto conflicts = expected_conflicts(judgments);
    const bool conflict = !conflicts.empty();
    const bool insufficient = !conflict && support.empty() && refutation.empty() &&
                              !usable_experience;
    return ReEvidenceResult(replayed.query, std::move(judgments), std::move(support),
                            std::move(refutation), std::move(conflicts), conflict,
                            insufficient, conflict || insufficient);
}

}  // namespace swegca::vrs
