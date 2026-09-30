#pragma once

#include "world/cognitive_state.hpp"
#include "world/proposition_directory.hpp"
#include "world/semantic_family_directory.hpp"

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace swegca::world {

inline constexpr std::string_view memory_activation_source_sha256 =
    "0fedd008564cb6afb52064b8dfe6e351ea7515006fb9d984aa4d4b31578ec792";

inline const std::set<std::string, std::less<>> memory_outcomes{
    "success", "failure", "negative", "uncertain", "conflict", "pending"};
inline const std::set<std::string, std::less<>> evidence_verdicts{
    "support", "refute", "insufficient", "conflict", "available", "retained"};

struct MemoryStep final {
    std::string phase;
    JsonValue::Object observation;
    std::vector<std::string> relations;
    std::string judgment;
    std::string outcome;
    std::vector<std::string> evidence_refs;

    MemoryStep(std::string phase, JsonValue::Object observation,
               std::vector<std::string> relations, std::string judgment,
               std::string outcome, std::vector<std::string> evidence_refs);
    friend bool operator==(const MemoryStep&, const MemoryStep&) = default;
};

struct MemoryEpisode final {
    std::string episode_id;
    std::vector<std::string> cues;
    std::vector<MemoryStep> steps;
    std::vector<std::string> source_addresses;
    std::string revision;
    std::string verification_state;

    MemoryEpisode(std::string episode_id, std::vector<std::string> cues,
                  std::vector<MemoryStep> steps,
                  std::vector<std::string> source_addresses,
                  std::string revision, std::string verification_state);
    friend bool operator==(const MemoryEpisode&, const MemoryEpisode&) = default;
};

class HotMemoryIndex {
public:
    virtual ~HotMemoryIndex() = default;
    [[nodiscard]] virtual std::string_view snapshot_id() const noexcept = 0;
    [[nodiscard]] virtual bool lookup_requires_io() const noexcept { return false; }
    [[nodiscard]] virtual std::size_t episode_count() const noexcept = 0;
    [[nodiscard]] virtual const std::map<std::string, std::size_t, std::less<>>&
        outcome_counts() const noexcept = 0;
    [[nodiscard]] virtual const MemoryEpisode& episode(std::string_view episode_id) const = 0;
    [[nodiscard]] virtual bool contains_episode(std::string_view episode_id) const;
    [[nodiscard]] virtual std::vector<std::string> episode_ids_for_cue(
        std::string_view cue) const = 0;
    [[nodiscard]] virtual std::vector<std::string> iter_episode_ids() const = 0;
    [[nodiscard]] virtual std::vector<SemanticFamilyDirectory>
        semantic_family_directories() const { return {}; }
};

class MemoryActivationIndex final : public HotMemoryIndex {
public:
    MemoryActivationIndex(
        std::string snapshot_id,
        std::map<std::string, MemoryEpisode, std::less<>> episodes_by_id,
        std::map<std::string, std::vector<std::string>, std::less<>> postings_by_cue,
        std::map<std::string, std::optional<bool>, std::less<>> coverage = {},
        bool lookup_requires_io = false);

    [[nodiscard]] std::string_view snapshot_id() const noexcept override;
    [[nodiscard]] bool lookup_requires_io() const noexcept override;
    [[nodiscard]] std::size_t episode_count() const noexcept override;
    [[nodiscard]] const std::map<std::string, std::size_t, std::less<>>&
        outcome_counts() const noexcept override;
    [[nodiscard]] const MemoryEpisode& episode(std::string_view episode_id) const override;
    [[nodiscard]] bool contains_episode(std::string_view episode_id) const override;
    [[nodiscard]] std::vector<std::string> episode_ids_for_cue(
        std::string_view cue) const override;
    [[nodiscard]] std::vector<std::string> iter_episode_ids() const override;

    const std::map<std::string, MemoryEpisode, std::less<>> episodes_by_id;
    const std::map<std::string, std::vector<std::string>, std::less<>> postings_by_cue;
    const PropositionDirectory proposition_directory;

private:
    std::string snapshot_id_;
    bool lookup_requires_io_{};
    std::map<std::string, std::size_t, std::less<>> outcome_counts_;
};

class CompositeMemoryActivationIndex final : public HotMemoryIndex {
public:
    explicit CompositeMemoryActivationIndex(
        std::vector<std::shared_ptr<const HotMemoryIndex>> sources);

    [[nodiscard]] std::string_view snapshot_id() const noexcept override;
    [[nodiscard]] std::size_t episode_count() const noexcept override;
    [[nodiscard]] const std::map<std::string, std::size_t, std::less<>>&
        outcome_counts() const noexcept override;
    [[nodiscard]] const MemoryEpisode& episode(std::string_view episode_id) const override;
    [[nodiscard]] bool contains_episode(std::string_view episode_id) const override;
    [[nodiscard]] std::vector<std::string> episode_ids_for_cue(
        std::string_view cue) const override;
    [[nodiscard]] std::vector<std::string> iter_episode_ids() const override;
    [[nodiscard]] std::vector<SemanticFamilyDirectory>
        semantic_family_directories() const override;

    const std::vector<std::shared_ptr<const HotMemoryIndex>> sources;

private:
    std::string snapshot_id_;
    std::map<std::string, std::size_t, std::less<>> outcome_counts_;
    std::map<std::string, std::shared_ptr<const HotMemoryIndex>, std::less<>> routed_;
    std::map<std::string, std::vector<std::string>, std::less<>> postings_;
    std::vector<std::shared_ptr<const HotMemoryIndex>> unrouted_;
};

[[nodiscard]] std::shared_ptr<const MemoryActivationIndex> build_memory_activation_index(
    const std::vector<MemoryEpisode>& episodes,
    const std::vector<std::string>& required_outcomes = {});
[[nodiscard]] std::shared_ptr<const HotMemoryIndex> append_memory_activation_index(
    std::shared_ptr<const HotMemoryIndex> base,
    const std::vector<MemoryEpisode>& episodes,
    const std::vector<std::string>& required_outcomes = {});

class AtomicMemoryActivationOwner final {
public:
    explicit AtomicMemoryActivationOwner(std::shared_ptr<const HotMemoryIndex> initial);
    [[nodiscard]] std::shared_ptr<const HotMemoryIndex> snapshot() const;
    [[nodiscard]] std::string replace(
        std::string_view expected_snapshot_id,
        std::shared_ptr<const HotMemoryIndex> replacement);
private:
    mutable std::mutex mutex_;
    std::shared_ptr<const HotMemoryIndex> current_;
};

struct FullCurrentMemoryVrsSnapshot final {
    std::shared_ptr<const HotMemoryIndex> memory;
    std::string vrs_snapshot_id;
    std::string snapshot_id;

    FullCurrentMemoryVrsSnapshot(std::shared_ptr<const HotMemoryIndex> memory,
                                 std::string vrs_snapshot_id);
};

class AtomicFullCurrentMemoryVrsOwner final {
public:
    explicit AtomicFullCurrentMemoryVrsOwner(FullCurrentMemoryVrsSnapshot initial);
    [[nodiscard]] FullCurrentMemoryVrsSnapshot snapshot() const;
    [[nodiscard]] std::string replace(
        std::string_view expected_snapshot_id,
        FullCurrentMemoryVrsSnapshot replacement);
private:
    mutable std::mutex mutex_;
    FullCurrentMemoryVrsSnapshot current_;
};

struct DejaVuSignal final {
    std::string snapshot_id;
    std::string query;
    std::vector<std::string> current_cues;
    std::vector<std::string> matched_cues;
    double recognition_strength{};
    std::size_t candidate_count{};
    bool memory_identifiers_exposed{};
    bool action_authorized{};

    DejaVuSignal(std::string snapshot_id, std::string query,
                 std::vector<std::string> current_cues,
                 std::vector<std::string> matched_cues,
                 double recognition_strength, std::size_t candidate_count,
                 bool memory_identifiers_exposed = false,
                 bool action_authorized = false);
    [[nodiscard]] bool triggered() const noexcept { return candidate_count > 0; }
};

struct RuntimeCueSelection final {
    std::string snapshot_id;
    std::string query;
    std::map<std::string, std::size_t, std::less<>> candidate_counts;
    std::vector<std::string> selected_cues;
    std::vector<std::string> rejected_cues;
    std::string selection_method{"minimum_nonempty_hot_fanout_then_query_order"};
    bool codex_or_evaluator_allowlist_used{};

    RuntimeCueSelection(std::string snapshot_id, std::string query,
        std::map<std::string, std::size_t, std::less<>> candidate_counts,
        std::vector<std::string> selected_cues,
        std::vector<std::string> rejected_cues,
        std::string selection_method = "minimum_nonempty_hot_fanout_then_query_order",
        bool codex_or_evaluator_allowlist_used = false);
};

[[nodiscard]] RuntimeCueSelection select_runtime_cues(
    const HotMemoryIndex& index, std::string query,
    const std::vector<std::string>& candidate_cues,
    const std::vector<std::string>& preferred_current_evidence_cues = {});
[[nodiscard]] DejaVuSignal detect_deja_vu(
    const HotMemoryIndex& index, std::string query,
    const std::vector<std::string>& current_cues);

struct RecallCandidate final {
    std::string episode_id;
    std::vector<std::string> matched_cues;
    double cue_overlap{};
    std::string revision;
    std::string verification_state;
    std::vector<std::string> historical_outcomes;
};

struct RecallResult final {
    std::string query;
    std::vector<RecallCandidate> candidates;
    std::string snapshot_id;
    bool codex_per_item_allowlist_used{};
    bool action_authorized{};
    bool persistent_write_authorized{};
    std::vector<std::pair<std::string, std::string>> source_dependencies;

    RecallResult(std::string query, std::vector<RecallCandidate> candidates,
        std::string snapshot_id, bool codex_per_item_allowlist_used = false,
        bool action_authorized = false, bool persistent_write_authorized = false,
        std::vector<std::pair<std::string, std::string>> source_dependencies = {});
};

[[nodiscard]] RecallResult recall_memory(
    const HotMemoryIndex& index, const DejaVuSignal& signal,
    const std::vector<std::string>& navigation_cues = {});

struct ReplayedEpisode final {
    std::string episode_id;
    std::vector<std::string> matched_cues;
    std::vector<MemoryStep> steps;
    std::vector<std::string> source_addresses;
    std::string verification_state;
    bool historical_truth_authorized{};

    ReplayedEpisode(std::string episode_id, std::vector<std::string> matched_cues,
        std::vector<MemoryStep> steps, std::vector<std::string> source_addresses,
        std::string verification_state, bool historical_truth_authorized = false);
};

struct ReplayResult final {
    std::string query;
    std::vector<ReplayedEpisode> episodes;
    bool action_authorized{};
    bool persistent_write_authorized{};

    ReplayResult(std::string query, std::vector<ReplayedEpisode> episodes,
                 bool action_authorized = false,
                 bool persistent_write_authorized = false);
};

[[nodiscard]] ReplayResult replay_memory(
    const HotMemoryIndex& index, const RecallResult& recalled);

struct CurrentEvidenceVerdict final {
    std::string episode_id;
    std::string proposition;
    std::string verdict;
    std::string rationale;
    std::vector<std::string> current_evidence_refs;
    std::vector<std::string> contradiction_refs;

    CurrentEvidenceVerdict(std::string episode_id, std::string proposition,
        std::string verdict, std::string rationale,
        std::vector<std::string> current_evidence_refs,
        std::vector<std::string> contradiction_refs = {});
};

[[nodiscard]] CurrentEvidenceVerdict current_experience_verdict(
    const ReplayedEpisode& episode, std::string memory_snapshot_id,
    std::string vrs_snapshot_id, std::optional<double> current_strength = std::nullopt,
    std::optional<std::string> proposition = std::nullopt);

struct ReEvidenceResult final {
    std::string query;
    std::vector<CurrentEvidenceVerdict> judgments;
    std::vector<std::string> selected_support;
    std::vector<std::string> selected_refutation;
    std::vector<std::string> conflicting_propositions;
    bool unresolved_conflict{};
    bool insufficient_evidence{};
    bool should_abstain{};
    bool action_authorized{};
    bool persistent_write_authorized{};
    bool semantic_promotion_authorized{};

    ReEvidenceResult(std::string query,
        std::vector<CurrentEvidenceVerdict> judgments,
        std::vector<std::string> selected_support,
        std::vector<std::string> selected_refutation,
        std::vector<std::string> conflicting_propositions,
        bool unresolved_conflict, bool insufficient_evidence, bool should_abstain,
        bool action_authorized = false, bool persistent_write_authorized = false,
        bool semantic_promotion_authorized = false);
};

using EvidenceJudge = std::function<CurrentEvidenceVerdict(const ReplayedEpisode&)>;
[[nodiscard]] ReEvidenceResult re_evidence_memory(
    const ReplayResult& replayed, const EvidenceJudge& judge);

struct MemoryActivationReceipt final {
    std::string schema_version;
    std::string snapshot_id;
    DejaVuSignal deja_vu;
    RecallResult recall;
    ReplayResult replay;
    ReEvidenceResult re_evidence;
    std::vector<std::string> stage_order{"deja_vu", "recall", "replay", "re_evidence"};
    bool action_authorized{};
    bool persistent_write_authorized{};

    MemoryActivationReceipt(std::string schema_version, std::string snapshot_id,
        DejaVuSignal deja_vu, RecallResult recall, ReplayResult replay,
        ReEvidenceResult re_evidence,
        std::vector<std::string> stage_order =
            {"deja_vu", "recall", "replay", "re_evidence"},
        bool action_authorized = false, bool persistent_write_authorized = false);
};

[[nodiscard]] MemoryActivationReceipt activate_memory(
    const HotMemoryIndex& index, std::string query,
    const std::vector<std::string>& current_cues, const EvidenceJudge& judge);

}  // namespace swegca::world
