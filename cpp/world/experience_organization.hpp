#pragma once

#include "world/memory_activation.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace swegca::world {

inline constexpr std::string_view experience_organization_source_sha256 =
    "08c6c4a35dbcef2aa81dda8d16ca5928c4835cbf0fa3211a78eaa01e53d45acd";
inline constexpr std::string_view organized_verification_state =
    "specialist_organized_pending_vrs";

struct SealedExperienceItem final {
    std::string source_item_id;
    JsonValue::Object observation;
    std::string attempted_judgment_or_action;
    std::string outcome;
    std::vector<std::string> evidence_refs;
    std::vector<std::string> source_addresses;
    std::string source_family;
    std::string task_family;
    std::int64_t observed_at_ns{};
    std::string revision;
    std::string verification_state;
    [[nodiscard]] JsonValue::Object canonical_payload() const;
};

struct ExperienceOrganizationRequest final {
    std::string base_snapshot_id;
    std::vector<SealedExperienceItem> items;
    std::string request_sha256;
    ExperienceOrganizationRequest(std::string base_snapshot_id,
        std::vector<SealedExperienceItem> items);
};

struct OrganizedExperienceItem final {
    std::string source_item_id;
    std::vector<std::string> cues;
    std::vector<std::string> relations;
    std::string judgment;
};

struct ExperienceOrganizationProposal final {
    std::string specialist_id;
    std::string request_sha256;
    std::string base_snapshot_id;
    std::vector<OrganizedExperienceItem> items;
    bool final_judgment_authorized{};
    bool semantic_authority_authorized{};
    bool action_authorized{};
    bool persistent_write_authorized{};
    bool world_write_authorized{};
    bool model_update_authorized{};
    bool distribution_authorized{};
    bool p3_authorized{};
    ExperienceOrganizationProposal(std::string specialist_id,
        std::string request_sha256, std::string base_snapshot_id,
        std::vector<OrganizedExperienceItem> items);
};

using ExperienceOrganizationSpecialist = std::function<ExperienceOrganizationProposal(
    const HotMemoryIndex&, const ExperienceOrganizationRequest&)>;

struct MainExperienceAssimilationReceipt final {
    std::string schema_version{"rozephine-main-experience-assimilation-receipt-v1"};
    std::string state_owner{"rozephine_main"};
    std::string request_sha256;
    std::string specialist_id;
    std::string base_snapshot_id;
    std::string next_snapshot_id;
    std::size_t retained_episode_count{};
    std::size_t added_episode_count{};
    std::size_t total_episode_count{};
    std::vector<std::string> source_families;
    std::vector<std::string> task_families;
    std::vector<std::string> added_outcomes;
    bool main_memory_commit{true};
    bool specialist_persistent_write_authorized{};
    bool final_judgment_authorized{};
    bool semantic_promotion_authorized{};
    bool action_authorized{};
    bool world_write_authorized{};
    bool model_update_authorized{};
    bool distribution_authorized{};
    bool p3_authorized{};
};

struct MainExperienceAssimilationResult final {
    std::shared_ptr<const HotMemoryIndex> snapshot;
    MainExperienceAssimilationReceipt receipt;
};

struct MainAnalogicalOutcomeJudgment final {
    std::string snapshot_id;
    std::string query;
    std::string status;
    std::optional<std::string> hypothesis;
    std::vector<std::string> selected_episode_ids;
    std::vector<std::string> rejected_episode_ids;
    std::map<std::string, std::string, std::less<>> rejection_reasons;
    std::vector<std::string> historical_outcomes;
    std::optional<std::string> current_task_family;
    std::optional<std::string> current_candidate_id;
    std::vector<std::string> current_task_episode_ids;
    std::vector<std::string> current_task_outcomes;
    std::optional<std::string> latest_current_task_episode_id;
    std::optional<std::string> latest_current_task_outcome;
    bool current_evidence_required{true};
    bool semantic_promotion_authority{};
    bool action_authority{};
    bool persistent_write_authority{};
    bool world_write_authority{};
    bool model_update_authority{};
    bool distribution_authority{};
    bool p3_authority{};
};

struct MainActionCandidate final {
    std::string candidate_id;
    std::vector<std::string> memory_cues;
};

struct MainCandidateReviewRow final {
    std::string candidate_id;
    RuntimeCueSelection cue_selection;
    MemoryActivationReceipt activation;
    MainAnalogicalOutcomeJudgment judgment;
    std::int64_t elapsed_ns{};
};

struct MainCandidateReviewBatch final {
    std::vector<MainCandidateReviewRow> rows;
    JsonValue::Object receipt;
};

class MainCandidateReviewer {
public:
    virtual ~MainCandidateReviewer() = default;
    [[nodiscard]] virtual MainCandidateReviewBatch review_candidates(
        const HotMemoryIndex& index, std::string_view task_id,
        std::string_view query, const std::vector<MainActionCandidate>& candidates,
        const std::optional<std::string>& current_task_family) const = 0;
};

struct MainActionCandidateSelection final {
    std::string snapshot_id;
    std::string task_id;
    std::string query;
    std::string status;
    std::optional<std::string> selected_candidate_id;
    std::vector<std::string> rejected_candidate_ids;
    std::map<std::string, RuntimeCueSelection, std::less<>> cue_selections;
    std::map<std::string, MemoryActivationReceipt, std::less<>> activations;
    std::map<std::string, MainAnalogicalOutcomeJudgment, std::less<>> candidate_judgments;
    std::map<std::string, std::int64_t, std::less<>> candidate_review_elapsed_ns;
    std::int64_t candidate_review_batch_elapsed_ns{};
    std::string candidate_review_backend{"cpu_full_receipt"};
    bool candidate_receipts_deferred{};
    JsonValue::Object resident_candidate_review_receipt;
    bool resident_flat_scheduler_used{};
    bool manager_persistent_state_retained{};
    bool worker_persistent_state_retained{};
    bool parallel_candidate_review{};
    std::size_t maximum_parallel_workers{1};
    std::string exploration_method;
    std::optional<std::string> current_task_family;
    std::vector<std::string> preferred_candidate_ids;
    bool prefer_least_recent_current_candidate{};
    bool current_evidence_required{true};
    bool action_authority{};
    bool semantic_promotion_authority{};
    bool persistent_write_authority{};
};

[[nodiscard]] std::string sealed_experience_episode_id(const SealedExperienceItem& item);
[[nodiscard]] ExperienceOrganizationProposal proposal_from_specialist_output(
    std::string specialist_id, const ExperienceOrganizationRequest& request,
    const JsonValue& payload);
[[nodiscard]] ExperienceOrganizationProposal run_experience_organization_specialist(
    const AtomicMemoryActivationOwner& owner,
    const ExperienceOrganizationRequest& request,
    std::string specialist_id,
    const ExperienceOrganizationSpecialist& specialist);
[[nodiscard]] MainAnalogicalOutcomeJudgment form_main_analogical_outcome_judgment(
    const HotMemoryIndex& index, const MemoryActivationReceipt& activation,
    std::optional<std::string> current_task_family = {},
    std::optional<std::string> current_candidate_id = {});
[[nodiscard]] MainActionCandidateSelection select_main_action_candidate(
    const HotMemoryIndex& index, std::string task_id, std::string query,
    const std::vector<MainActionCandidate>& candidates,
    std::size_t maximum_parallel_workers = 1,
    const MainCandidateReviewer* resident_candidate_reviewer = nullptr,
    std::optional<std::string> current_task_family = {},
    std::vector<std::string> preferred_candidate_ids = {},
    bool prefer_least_recent_current_candidate = false,
    std::map<std::string, std::string, std::less<>> prepared_exploration_ranks = {});
[[nodiscard]] MemoryEpisode ground_organized_experience_episode(
    const MemoryEpisode& episode);
[[nodiscard]] std::vector<MemoryEpisode> materialize_organized_experience_episodes(
    const HotMemoryIndex& base, const ExperienceOrganizationRequest& request,
    const ExperienceOrganizationProposal& proposal);
[[nodiscard]] MainExperienceAssimilationResult assimilate_organization_proposal(
    AtomicMemoryActivationOwner& owner, const ExperienceOrganizationRequest& request,
    const ExperienceOrganizationProposal& proposal);

}  // namespace swegca::world
