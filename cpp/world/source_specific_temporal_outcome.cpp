#include "world/source_specific_temporal_outcome.hpp"

#include "world/semantic_vrs_ingress.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>

namespace swegca::world {
namespace {

constexpr std::string_view query =
    "Will the disjoint later interval preserve a stable visual state?";
constexpr std::string_view profile_prefix = "phase7-temporal-profile-v2";
constexpr std::string_view content_prefix = "phase7-current-proposal-v2";
constexpr std::string_view empty_sha256 =
    "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";

void require_text(const std::string_view value, const char* label) {
    if (value.empty()) throw std::invalid_argument(std::string(label) + " must be nonempty text");
}

std::pair<std::string, std::string> identity(
    const std::vector<TemporalProposal>& rows) {
    if (rows.empty()) throw std::invalid_argument("temporal proposals are required");
    std::set<std::string, std::less<>> sources, revisions;
    for (const auto& row : rows) {
        require_text(row.source_id, "proposal source ID");
        require_text(row.source_revision_receipt, "proposal source revision");
        sources.insert(row.source_id);
        revisions.insert(row.source_revision_receipt);
    }
    if (sources.size() != 1 || revisions.size() != 1)
        throw std::invalid_argument("proposal source lineage changed within one interval");
    return {*sources.begin(), *revisions.begin()};
}

std::vector<double> cosines(const std::vector<TemporalProposal>& rows) {
    std::vector<double> result;
    for (const auto& row : rows) {
        if (row.worker_kind != "visual_embedding" || row.status != "observed" ||
            !row.successive_embedding_cosine) continue;
        const auto value = *row.successive_embedding_cosine;
        if (!std::isfinite(value) || value < -1.0 || value > 1.0)
            throw std::invalid_argument("visual successive cosine is outside [-1, 1]");
        result.push_back(value);
    }
    return result;
}

double median(std::vector<double> values) {
    std::ranges::sort(values);
    const auto middle = values.size() / 2;
    if (values.size() % 2) return values[middle];
    return (values[middle - 1] + values[middle]) / 2.0;
}

int profile_bin(const double value, const double width) {
    if (value < 0.0 || value > 1.0)
        throw std::invalid_argument("profile cosine must be within [0, 1]");
    return std::min(static_cast<int>(value / width), static_cast<int>(1.0 / width) - 1);
}

JsonValue::Array strings(const std::vector<std::string>& values) {
    JsonValue::Array result;
    for (const auto& value : values) result.emplace_back(value);
    return result;
}

JsonValue::Array numbers(const std::vector<double>& values) {
    JsonValue::Array result;
    for (const auto value : values) result.emplace_back(value);
    return result;
}

JsonValue::Object authority_false() {
    return {{"semantic", false}, {"world", false}, {"action", false},
        {"persistent_write", false}, {"model_update", false},
        {"distribution", false}, {"p3", false}};
}

std::string digest(JsonValue::Object value) {
    return semantic_json_digest(JsonValue(std::move(value)));
}

}  // namespace

std::vector<std::string> PrefixTemporalEvidence::candidate_cues() const {
    auto result = profile_cues;
    result.insert(result.end(), content_cues.begin(), content_cues.end());
    return result;
}

JsonValue::Object PrefixTemporalEvidence::receipt() const {
    return {{"schema_version", "rozephine-source-specific-prefix-temporal-evidence-v2"},
        {"source_id", source_id}, {"source_revision_receipt", source_revision_receipt},
        {"median_successive_cosine", median_successive_cosine},
        {"minimum_successive_cosine", minimum_successive_cosine},
        {"maximum_successive_cosine", maximum_successive_cosine},
        {"ocr_observed", ocr_observed},
        {"source_dimensions", JsonValue::Array{
            JsonValue(static_cast<std::int64_t>(source_dimensions.first)),
            JsonValue(static_cast<std::int64_t>(source_dimensions.second))}},
        {"profile_cues", strings(profile_cues)}, {"content_cues", strings(content_cues)},
        {"receipt_sha256", receipt_sha256}};
}

JsonValue::Object LaterVisualStabilityOutcome::receipt() const {
    return {{"schema_version", "rozephine-later-visual-stability-outcome-v2"},
        {"source_id", source_id}, {"source_revision_receipt", source_revision_receipt},
        {"outcome", outcome}, {"operational_label", operational_label},
        {"stability_threshold", stability_threshold},
        {"successive_cosines", numbers(successive_cosines)},
        {"median_successive_cosine", median_successive_cosine
            ? JsonValue(*median_successive_cosine) : JsonValue(nullptr)},
        {"outcome_selected_by", "main_preregistered_later_visual_stability_rule"},
        {"perception_availability_is_success", false},
        {"specialist_could_choose_rewrite_or_veto", false},
        {"authority", authority_false()}, {"receipt_sha256", receipt_sha256}};
}

PrefixTemporalEvidence extract_prefix_temporal_evidence(
    const std::vector<TemporalProposal>& proposals) {
    const auto [source, revision] = identity(proposals);
    auto values = cosines(proposals);
    const auto visual = std::ranges::find_if(proposals, [](const auto& row) {
        return row.worker_kind == "visual_embedding" && row.status == "observed";
    });
    if (visual == proposals.end() || values.empty())
        throw std::invalid_argument("prefix requires observed visual temporal evidence");
    if (!visual->source_dimensions || !visual->source_dimensions->first ||
        !visual->source_dimensions->second)
        throw std::invalid_argument("prefix source dimensions changed");
    const auto middle = median(values);
    const auto minimum = *std::ranges::min_element(values);
    const auto maximum = *std::ranges::max_element(values);
    const bool ocr = std::ranges::any_of(proposals, [](const auto& row) {
        return row.worker_kind == "roi_ocr" && row.status == "observed" &&
            row.text && !row.text->empty();
    });
    const auto [width, height] = *visual->source_dimensions;
    const std::string resolution = width * height >= 900000
        ? "at-least-900kp" : "below-900kp";
    const auto m10 = profile_bin(middle, 0.1), n10 = profile_bin(minimum, 0.1);
    const auto m20 = profile_bin(middle, 0.2), n20 = profile_bin(minimum, 0.2);
    const auto x20 = profile_bin(maximum, 0.2);
    const auto ocr_number = ocr ? "1" : "0";
    std::vector<std::string> profiles{
        std::string(profile_prefix) + ":m10=" + std::to_string(m10) + ":n10=" +
            std::to_string(n10) + ":x20=" + std::to_string(x20) + ":ocr=" +
            ocr_number + ":res=" + resolution,
        std::string(profile_prefix) + ":m20=" + std::to_string(m20) + ":n20=" +
            std::to_string(n20) + ":x20=" + std::to_string(x20) + ":ocr=" + ocr_number,
        std::string(profile_prefix) + ":m20=" + std::to_string(m20) + ":n20=" +
            std::to_string(n20) + ":ocr=" + ocr_number,
        std::string(profile_prefix) + ":m20=" + std::to_string(m20)};
    std::vector<std::string> contents;
    for (const auto& row : proposals) {
        if (row.status != "observed" || row.content_or_embedding_sha256.empty() ||
            row.content_or_embedding_sha256 == empty_sha256) continue;
        const auto cue = std::string(content_prefix) + ':' + row.worker_kind + ':' +
            row.content_or_embedding_sha256;
        if (std::ranges::find(contents, cue) == contents.end()) contents.push_back(cue);
    }
    if (contents.empty())
        throw std::invalid_argument("prefix lacks nonempty proposal content identity");
    JsonValue::Object canonical{{"source_id", source}, {"source_revision_receipt", revision},
        {"median_successive_cosine", middle}, {"minimum_successive_cosine", minimum},
        {"maximum_successive_cosine", maximum}, {"ocr_observed", ocr},
        {"source_dimensions", JsonValue::Array{
            JsonValue(static_cast<std::int64_t>(width)),
            JsonValue(static_cast<std::int64_t>(height))}},
        {"profile_cues", strings(profiles)}, {"content_cues", strings(contents)}};
    return {source, revision, middle, minimum, maximum, ocr, {width, height},
        std::move(profiles), std::move(contents), digest(std::move(canonical))};
}

LaterVisualStabilityOutcome evaluate_later_visual_stability(
    const std::vector<TemporalProposal>& proposals, const double threshold) {
    const auto [source, revision] = identity(proposals);
    if (!(threshold > 0.0 && threshold < 1.0))
        throw std::invalid_argument("stability threshold must be within (0, 1)");
    auto values = cosines(proposals);
    std::optional<double> middle;
    std::string outcome = "negative";
    std::string operational = "insufficient_visual_temporal_evidence";
    if (!values.empty()) {
        middle = median(values);
        const bool stable = *middle >= threshold;
        outcome = stable ? "success" : "failure";
        operational = stable ? "stable" : "changing";
    }
    JsonValue::Object wire{{"schema_version", "rozephine-later-visual-stability-outcome-v2"},
        {"source_id", source}, {"source_revision_receipt", revision}, {"outcome", outcome},
        {"operational_label", operational}, {"stability_threshold", threshold},
        {"successive_cosines", numbers(values)},
        {"median_successive_cosine", middle ? JsonValue(*middle) : JsonValue(nullptr)},
        {"outcome_selected_by", "main_preregistered_later_visual_stability_rule"},
        {"perception_availability_is_success", false},
        {"specialist_could_choose_rewrite_or_veto", false}, {"authority", authority_false()}};
    return {source, revision, outcome, operational, threshold, std::move(values), middle,
        digest(std::move(wire))};
}

MemoryEpisode development_projection_episode(
    const PrefixTemporalEvidence& evidence, const LaterVisualStabilityOutcome& outcome,
    std::string prefix_proposals_sha256, std::string later_proposals_sha256) {
    if (outcome.source_id != evidence.source_id ||
        outcome.source_revision_receipt != evidence.source_revision_receipt)
        throw std::invalid_argument("development prefix/later lineage changed");
    if (outcome.outcome != "success" && outcome.outcome != "failure")
        throw std::invalid_argument("development projection requires a resolved binary outcome");
    const auto episode_id = "experience-projection:" + digest(JsonValue::Object{
        {"source_revision_receipt", evidence.source_revision_receipt},
        {"prefix_proposals_sha256", prefix_proposals_sha256},
        {"later_proposals_sha256", later_proposals_sha256},
        {"outcome_receipt_sha256", outcome.receipt_sha256},
        {"task_family", temporal_task_family}});
    std::vector<std::string> cues{"task-family:" + std::string(temporal_task_family),
        "outcome:" + outcome.outcome};
    cues.insert(cues.end(), evidence.profile_cues.begin(), evidence.profile_cues.end());
    cues.insert(cues.end(), evidence.content_cues.begin(), evidence.content_cues.end());
    JsonValue::Object observation{{"task_family", temporal_task_family},
        {"candidate_id", evidence.receipt_sha256}, {"observed_at_ns", std::int64_t{0}},
        {"source_id", evidence.source_id},
        {"source_revision_receipt", evidence.source_revision_receipt},
        {"prefix_temporal_evidence_receipt_sha256", evidence.receipt_sha256},
        {"operational_outcome", outcome.operational_label},
        {"derived_projection_not_new_source_experience", true}};
    MemoryStep step("observation_attempt_outcome", std::move(observation),
        {"task-family:" + std::string(temporal_task_family) + "->outcome:" + outcome.outcome,
         "requests-evidence:disjoint-later-visual-stability"},
        "proposal: an already-observed development prefix profile was followed by the main-sealed " +
            outcome.outcome + " stability outcome", outcome.outcome,
        {"sha256:" + prefix_proposals_sha256, "sha256:" + later_proposals_sha256,
         "sha256:" + outcome.receipt_sha256});
    return MemoryEpisode(episode_id, std::move(cues), {std::move(step)},
        {evidence.source_id, "source-revision:" + evidence.source_revision_receipt,
         "prefix-evidence:" + evidence.receipt_sha256},
        "source-revision:" + evidence.source_revision_receipt,
        "specialist_organized_pending_vrs");
}

TemporalJudgmentResult judge_later_visual_stability(
    const HotMemoryIndex& memory, const PrefixTemporalEvidence& evidence) {
    auto selection = select_runtime_cues(memory, std::string(query), evidence.candidate_cues(),
        evidence.profile_cues);
    auto activation = activate_memory(memory, std::string(query), selection.selected_cues,
        [&](const ReplayedEpisode& episode) {
            return CurrentEvidenceVerdict(episode.episode_id,
                "prefix-temporal-evidence:" + evidence.receipt_sha256, "insufficient",
                "the disjoint later visual-stability outcome remains sealed", {});
        });
    TemporalAnalogicalJudgment judgment;
    judgment.snapshot_id = std::string(memory.snapshot_id());
    judgment.query = activation.recall.query;
    for (const auto& candidate : activation.recall.candidates) {
        const auto& episode = memory.episode(candidate.episode_id);
        const bool direct = episode.verification_state == "specialist_organized_pending_vrs" &&
            episode.steps.size() == 1 && episode.steps.front().phase == "observation_attempt_outcome" &&
            (episode.steps.front().outcome == "success" || episode.steps.front().outcome == "failure");
        if (direct) {
            judgment.selected_episode_ids.push_back(episode.episode_id);
            judgment.historical_outcomes.push_back(episode.steps.front().outcome);
        } else {
            judgment.rejected_episode_ids.push_back(episode.episode_id);
            judgment.rejection_reasons.emplace(episode.episode_id,
                "not a direct organized binary-outcome analogy");
        }
    }
    std::set<std::string, std::less<>> distinct(
        judgment.historical_outcomes.begin(), judgment.historical_outcomes.end());
    if (distinct.size() == 1) {
        judgment.status = "non_authoritative_hypothesis";
        judgment.hypothesis = *distinct.begin();
    } else if (!distinct.empty()) {
        judgment.status = "abstain_conflicting_direct_analogies";
    } else {
        judgment.status = "abstain_no_direct_analogy";
    }
    return {std::move(selection), std::move(activation), std::move(judgment)};
}

}  // namespace swegca::world
