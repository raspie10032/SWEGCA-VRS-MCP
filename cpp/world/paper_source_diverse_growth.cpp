#include "world/paper_source_diverse_growth.hpp"

#include "world/semantic_vrs_ingress.hpp"

#include <stdexcept>

namespace swegca::world {
namespace {

JsonValue::Object authority_false() {
    return {{"semantic", false}, {"world", false}, {"action", false},
        {"persistent_write", false}, {"model_update", false},
        {"distribution", false}, {"p3", false}};
}

std::string digest(JsonValue::Object value) {
    return semantic_json_digest(JsonValue(std::move(value)));
}

}  // namespace

JsonValue::Object PrefixVisualJudgment::receipt() const {
    return {{"schema_version", "rozephine-paper-prefix-visual-judgment-v1"},
        {"source_id", source_id}, {"source_revision_receipt", source_revision_receipt},
        {"prefix_evidence_receipt_sha256", prefix_evidence_receipt_sha256},
        {"predicted_operational_label", predicted_operational_label},
        {"predicted_outcome", predicted_outcome}, {"threshold", threshold},
        {"later_interval_opened", false}, {"proposal_only", true},
        {"authority", authority_false()}, {"receipt_sha256", receipt_sha256}};
}

PrefixVisualJudgment prefix_judgment(
    const PrefixTemporalEvidence& evidence, const double threshold) {
    if (!(threshold > 0.0 && threshold < 1.0))
        throw std::invalid_argument("prefix stability threshold must be within (0, 1)");
    const bool stable = evidence.median_successive_cosine >= threshold;
    JsonValue::Object wire{{"schema_version", "rozephine-paper-prefix-visual-judgment-v1"},
        {"source_id", evidence.source_id},
        {"source_revision_receipt", evidence.source_revision_receipt},
        {"prefix_evidence_receipt_sha256", evidence.receipt_sha256},
        {"predicted_operational_label", stable ? "stable" : "changing"},
        {"predicted_outcome", stable ? "success" : "failure"},
        {"threshold", threshold}, {"later_interval_opened", false},
        {"proposal_only", true}, {"authority", authority_false()}};
    return {evidence.source_id, evidence.source_revision_receipt,
        evidence.receipt_sha256, stable ? "stable" : "changing",
        stable ? "success" : "failure", threshold, digest(std::move(wire))};
}

MemoryEpisode build_source_diverse_outcome_episode(
    const PrefixTemporalEvidence& evidence, const PrefixVisualJudgment& prefix,
    const LaterVisualStabilityOutcome& outcome, std::string source_family,
    std::string source_item_id, std::string prefix_proposals_sha256,
    std::string later_proposals_sha256) {
    if ((outcome.outcome != "success" && outcome.outcome != "failure") ||
        (outcome.operational_label != "stable" && outcome.operational_label != "changing"))
        throw std::invalid_argument("source-diverse episode requires a resolved visual outcome");
    if (prefix.source_id != evidence.source_id || outcome.source_id != evidence.source_id ||
        prefix.source_revision_receipt != evidence.source_revision_receipt ||
        outcome.source_revision_receipt != evidence.source_revision_receipt ||
        source_family.empty() || source_item_id.empty())
        throw std::invalid_argument("source-diverse prefix/later lineage changed");
    if (evidence.profile_cues.empty())
        throw std::invalid_argument("source-diverse profile cue is missing");
    const auto& profile = evidence.profile_cues.front();
    const auto actual = "actual-relation:" + profile +
        "->temporal-event:later-visual-stability|actual-class:" + outcome.operational_label;
    const auto episode_id = "experience:" + digest(JsonValue::Object{
        {"source_id", evidence.source_id},
        {"source_revision_receipt", evidence.source_revision_receipt},
        {"prefix_judgment_receipt_sha256", prefix.receipt_sha256},
        {"outcome_receipt_sha256", outcome.receipt_sha256},
        {"task_family", temporal_task_family}});
    std::vector<std::string> cues{"task-family:" + std::string(temporal_task_family),
        "outcome:" + outcome.outcome};
    cues.insert(cues.end(), evidence.profile_cues.begin(), evidence.profile_cues.end());
    cues.insert(cues.end(), evidence.content_cues.begin(), evidence.content_cues.end());
    JsonValue::Object observation{
        {"schema_version", "rozephine-paper-source-diverse-outcome-v1"},
        {"source_item_id", source_item_id}, {"source_id", evidence.source_id},
        {"source_revision_receipt", evidence.source_revision_receipt},
        {"source_family", source_family}, {"task_family", temporal_task_family},
        {"prefix_evidence_receipt_sha256", evidence.receipt_sha256},
        {"prefix_judgment_receipt_sha256", prefix.receipt_sha256},
        {"later_outcome_receipt_sha256", outcome.receipt_sha256},
        {"prefix_judgment_matched_later_outcome",
            prefix.predicted_operational_label == outcome.operational_label},
        {"operational_outcome", outcome.operational_label},
        {"one_original_source_equals_one_experience", true},
        {"generated_frames_count_as_new_experience", false}};
    MemoryStep step("observation_attempt_outcome", std::move(observation),
        {profile + "->worker-outcome:" + outcome.outcome, actual,
         "requests-evidence:disjoint-later-visual-stability"},
        "The sealed prefix-only prediction was " + prefix.predicted_operational_label +
            "; the disjoint later interval was " + outcome.operational_label + '.',
        outcome.outcome,
        {"sha256:" + prefix_proposals_sha256, "sha256:" + later_proposals_sha256,
         "sha256:" + prefix.receipt_sha256, "sha256:" + outcome.receipt_sha256});
    return MemoryEpisode(episode_id, std::move(cues), {std::move(step)},
        {evidence.source_id, "source-revision:" + evidence.source_revision_receipt,
         "prefix-evidence:" + evidence.receipt_sha256},
        "source-revision:" + evidence.source_revision_receipt,
        "main_sealed_pending_vrs");
}

}  // namespace swegca::world
