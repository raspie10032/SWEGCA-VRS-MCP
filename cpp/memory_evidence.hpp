#pragma once

#include "memory_episode.hpp"
#include "memory_recall.hpp"

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace swegca::vrs {

struct ReplayedEpisode {
    std::string episode_id;
    std::vector<std::string> matched_cues;
    std::vector<MemoryStep> steps;
    std::vector<std::string> source_addresses;
    std::string verification_state;
    bool historical_truth_authorized = false;

    // SWEGCA: src/swegca_vrs2/engine/mosaic_memory_activation.py@7536139:350-364
    ReplayedEpisode(std::string episode_id, std::vector<std::string> matched_cues,
                    std::vector<MemoryStep> steps, std::vector<std::string> source_addresses,
                    std::string verification_state, bool historical_truth_authorized = false);
};

struct ReplayResult {
    std::string query;
    std::vector<ReplayedEpisode> episodes;
    bool action_authorized = false;
    bool persistent_write_authorized = false;

    // SWEGCA: src/swegca_vrs2/engine/mosaic_memory_activation.py@7536139:366-375
    ReplayResult(std::string query, std::vector<ReplayedEpisode> episodes,
                 bool action_authorized = false, bool persistent_write_authorized = false);
};

using OriginalReplayReader = std::function<MemoryEpisode(std::string_view)>;

// The accepted read path passes a one-original RecallResult and a pinned
// native-journal reader. Recall's full address list remains separate.
// SWEGCA: src/swegca_vrs2/engine/mosaic_memory_activation.py@7536139:378-398
// SWEGCA: user@2026-09-22:24-29
[[nodiscard]] ReplayResult replay_memory(const PublishedHotIndex& index,
                                         const RecallResult& selected,
                                         const OriginalReplayReader& read_original);

struct CurrentEvidenceVerdict {
    std::string episode_id;
    std::string proposition;
    std::string verdict;
    std::string rationale;
    std::vector<std::string> current_evidence_refs;
    std::vector<std::string> contradiction_refs;

    // SWEGCA: src/swegca_vrs2/engine/mosaic_memory_activation.py@7536139:401-429
    CurrentEvidenceVerdict(std::string episode_id, std::string proposition,
                           std::string verdict, std::string rationale,
                           std::vector<std::string> current_evidence_refs,
                           std::vector<std::string> contradiction_refs = {});
};

// SWEGCA: src/swegca_vrs2/engine/mosaic_memory_activation.py@7536139:432-458
[[nodiscard]] CurrentEvidenceVerdict current_experience_verdict(
    const ReplayedEpisode& episode, std::string memory_snapshot_id,
    std::string vrs_snapshot_id, std::optional<double> current_strength = std::nullopt,
    std::optional<std::string> proposition = std::nullopt);

struct ReEvidenceResult {
    std::string query;
    std::vector<CurrentEvidenceVerdict> judgments;
    std::vector<std::string> selected_support;
    std::vector<std::string> selected_refutation;
    std::vector<std::string> conflicting_propositions;
    bool unresolved_conflict = false;
    bool insufficient_evidence = false;
    bool should_abstain = false;
    bool action_authorized = false;
    bool persistent_write_authorized = false;
    bool semantic_promotion_authorized = false;

    // SWEGCA: src/swegca_vrs2/engine/mosaic_memory_activation.py@7536139:461-516
    ReEvidenceResult(std::string query, std::vector<CurrentEvidenceVerdict> judgments,
                     std::vector<std::string> selected_support,
                     std::vector<std::string> selected_refutation,
                     std::vector<std::string> conflicting_propositions,
                     bool unresolved_conflict, bool insufficient_evidence,
                     bool should_abstain, bool action_authorized = false,
                     bool persistent_write_authorized = false,
                     bool semantic_promotion_authorized = false);
};

using EvidenceJudge = std::function<CurrentEvidenceVerdict(const ReplayedEpisode&)>;

// SWEGCA: src/swegca_vrs2/engine/mosaic_memory_activation.py@7536139:518-555
[[nodiscard]] ReEvidenceResult re_evidence_memory(const ReplayResult& replayed,
                                                  const EvidenceJudge& judge);

}  // namespace swegca::vrs
