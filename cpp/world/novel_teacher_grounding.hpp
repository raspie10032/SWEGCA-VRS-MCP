#pragma once

#include "world/experience_organization.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace swegca::world {

inline constexpr std::string_view novel_teacher_grounding_source_sha256 =
    "4a0b935c2fd8190261c13fe8673c567247e5a4420bb75558b86509b5b778d10a";
inline constexpr std::string_view novel_organizer_id =
    "deterministic-main-owned-novel-teaching-organizer-v1";

struct NovelContextRequest final {
    std::string task_id;
    std::string source_id;
    std::string source_revision;
    std::string task_family;
    std::string question;
    std::string context_text;
    std::string context_sha256;
    std::int64_t context_start_char{};
    std::int64_t context_end_char{};
    std::int64_t context_start_line{};
    std::int64_t context_end_line{};
    std::string memory_snapshot_id;
    std::string vrs_snapshot_id;
    std::string full_current_pair_snapshot_id;

    NovelContextRequest(std::string task_id, std::string source_id,
        std::string source_revision, std::string task_family, std::string question,
        std::string context_text, std::string context_sha256,
        std::int64_t context_start_char, std::int64_t context_end_char,
        std::int64_t context_start_line, std::int64_t context_end_line,
        std::string memory_snapshot_id, std::string vrs_snapshot_id,
        std::string full_current_pair_snapshot_id);
    [[nodiscard]] static NovelContextRequest from_public_payload(
        const JsonValue::Object& value, std::string memory_snapshot_id,
        std::string vrs_snapshot_id, std::string full_current_pair_snapshot_id);
    [[nodiscard]] JsonValue::Object receipt() const;
};

struct NovelJudgmentProposal final {
    std::string specialist_id;
    std::string task_id;
    std::string full_current_pair_snapshot_id;
    std::string hypothesis;
    std::string rationale;
    std::string confidence;
    std::vector<std::string> evidence_refs;

    NovelJudgmentProposal(std::string specialist_id, std::string task_id,
        std::string full_current_pair_snapshot_id, std::string hypothesis,
        std::string rationale, std::string confidence,
        std::vector<std::string> evidence_refs);
    [[nodiscard]] JsonValue::Object receipt() const;
};

struct SealedNovelJudgment final {
    std::string task_id;
    std::string source_id;
    std::string source_revision;
    std::string task_family;
    std::string arm;
    std::string full_current_pair_snapshot_id;
    std::string hypothesis;
    std::string rationale;
    std::string confidence;
    std::vector<std::string> evidence_refs;
    std::int64_t sealed_at_ns{};
    std::string seal_id;

    SealedNovelJudgment(std::string task_id, std::string source_id,
        std::string source_revision, std::string task_family, std::string arm,
        std::string full_current_pair_snapshot_id, std::string hypothesis,
        std::string rationale, std::string confidence,
        std::vector<std::string> evidence_refs, std::int64_t sealed_at_ns);
    [[nodiscard]] JsonValue::Object receipt() const;
};

[[nodiscard]] SealedNovelJudgment seal_novel_judgment(
    const NovelContextRequest& request, std::string hypothesis,
    std::string rationale, std::string confidence,
    std::vector<std::string> evidence_refs, std::int64_t sealed_at_ns,
    std::string arm = "full_system");

struct NovelOutcomeEvidence final {
    std::string task_id;
    std::string seal_id;
    std::string source_id;
    std::string source_revision;
    std::string outcome_text;
    std::string outcome_sha256;
    std::int64_t outcome_start_char{};
    std::int64_t outcome_end_char{};
    std::int64_t outcome_start_line{};
    std::int64_t outcome_end_line{};
    std::int64_t revealed_at_ns{};
    bool source_read_independent_of_specialist{true};

    NovelOutcomeEvidence(std::string task_id, std::string seal_id,
        std::string source_id, std::string source_revision, std::string outcome_text,
        std::string outcome_sha256, std::int64_t outcome_start_char,
        std::int64_t outcome_end_char, std::int64_t outcome_start_line,
        std::int64_t outcome_end_line, std::int64_t revealed_at_ns,
        bool source_read_independent_of_specialist = true);
    void validate_against_seal(const SealedNovelJudgment& sealed) const;
};

struct TeacherEvidenceSpan final {
    std::int64_t start_offset{};
    std::int64_t end_offset{};
    std::string text;
    std::string text_sha256;

    TeacherEvidenceSpan(std::int64_t start_offset, std::int64_t end_offset,
        std::string text, std::string text_sha256);
    [[nodiscard]] static TeacherEvidenceSpan exact(
        std::string_view outcome_text, std::int64_t start_offset,
        std::int64_t end_offset);
};

struct CodexTeacherCorrection final {
    std::string task_id;
    std::string seal_id;
    std::string source_id;
    std::string source_revision;
    std::string outcome_sha256;
    std::string verdict;
    std::string rationale;
    std::vector<TeacherEvidenceSpan> evidence_spans;
    std::optional<std::string> counterevidence;
    std::string confidence;
    std::int64_t issued_at_ns{};
    std::string teacher_id{"codex"};
    std::string correction_id;

    CodexTeacherCorrection(std::string task_id, std::string seal_id,
        std::string source_id, std::string source_revision,
        std::string outcome_sha256, std::string verdict, std::string rationale,
        std::vector<TeacherEvidenceSpan> evidence_spans,
        std::optional<std::string> counterevidence, std::string confidence,
        std::int64_t issued_at_ns, std::string teacher_id = "codex");
    [[nodiscard]] std::string experience_outcome() const;
    [[nodiscard]] JsonValue::Object main_evidence_receipt(
        bool include_correction_id = true) const;
};

[[nodiscard]] CodexTeacherCorrection issue_codex_teacher_correction(
    const SealedNovelJudgment& sealed, const NovelOutcomeEvidence& outcome,
    std::string verdict, std::string rationale,
    std::vector<TeacherEvidenceSpan> evidence_spans,
    std::optional<std::string> counterevidence, std::string confidence,
    std::int64_t issued_at_ns);
[[nodiscard]] SealedExperienceItem form_novel_teaching_experience(
    const NovelContextRequest& request, const SealedNovelJudgment& sealed,
    const NovelOutcomeEvidence& outcome, const CodexTeacherCorrection& correction,
    std::int64_t observed_at_ns);
[[nodiscard]] std::pair<ExperienceOrganizationRequest, ExperienceOrganizationProposal>
organize_novel_teaching_experience(
    std::shared_ptr<const HotMemoryIndex> base, const SealedExperienceItem& sealed);

struct StagedNovelTeachingExperience final {
    std::string base_pair_snapshot_id;
    ExperienceOrganizationRequest request;
    ExperienceOrganizationProposal proposal;
    MemoryEpisode episode;
    std::shared_ptr<const HotMemoryIndex> staged_memory;

    StagedNovelTeachingExperience(std::string base_pair_snapshot_id,
        ExperienceOrganizationRequest request,
        ExperienceOrganizationProposal proposal, MemoryEpisode episode,
        std::shared_ptr<const HotMemoryIndex> staged_memory);
};

[[nodiscard]] StagedNovelTeachingExperience stage_novel_teaching_experience(
    const FullCurrentMemoryVrsSnapshot& base_pair,
    const SealedExperienceItem& sealed);

struct NovelVrsConvergenceEvidence final {
    std::string vrs_snapshot_id;
    bool all_required_outcomes_contributed{};
    std::size_t pre_convergence_connections_removed{};
    double verified_experience_promotion_strength{};
    std::optional<double> post_convergence_cutoff;

    NovelVrsConvergenceEvidence(std::string vrs_snapshot_id,
        bool all_required_outcomes_contributed,
        std::size_t pre_convergence_connections_removed,
        double verified_experience_promotion_strength,
        std::optional<double> post_convergence_cutoff = {});
};

struct PublishedNovelTeachingExperience final {
    FullCurrentMemoryVrsSnapshot snapshot;
    JsonValue::Object receipt;
};

[[nodiscard]] PublishedNovelTeachingExperience publish_staged_novel_teaching_experience(
    AtomicFullCurrentMemoryVrsOwner& owner,
    const FullCurrentMemoryVrsSnapshot& base_pair,
    const StagedNovelTeachingExperience& staged,
    const NovelVrsConvergenceEvidence& convergence);
[[nodiscard]] MemoryActivationReceipt activate_novel_teaching_memory(
    const FullCurrentMemoryVrsSnapshot& pair,
    const NovelContextRequest& request,
    const std::set<std::string, std::less<>>& promoted_episode_ids);

}  // namespace swegca::world
