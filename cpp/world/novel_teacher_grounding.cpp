#include "world/novel_teacher_grounding.hpp"

#include "swegca_architecture/sha256.hpp"
#include "world/semantic_vrs_ingress.hpp"

#include <algorithm>
#include <cstddef>
#include <set>
#include <span>
#include <stdexcept>

namespace swegca::world {
namespace {

const std::set<std::string, std::less<>> task_families{
    "character-state", "event-order", "speaker-attribution", "causal-relation",
    "goal-and-motivation", "next-development"};
const std::set<std::string, std::less<>> teacher_verdicts{
    "supported", "refuted", "ambiguous"};
const std::set<std::string, std::less<>> confidence_levels{"low", "medium", "high"};
const std::set<std::string, std::less<>> teaching_arms{
    "full_system", "E2B_proposal_masked", "retrieval_disabled",
    "VRS_delta_frozen", "current_context_only"};

void require_text(const std::string_view value, const char* label) {
    if (value.empty()) throw std::invalid_argument(std::string(label) + " must be nonempty text");
}

bool digest_id(const std::string_view value) {
    return value.size() == 64 && std::ranges::all_of(value, [](const unsigned char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

bool novel_task_id(const std::string_view value) {
    return value.starts_with("novel-task:") && digest_id(value.substr(11));
}

std::string digest_text(const std::string_view value) {
    const auto bytes = architecture::Sha256::of(
        std::as_bytes(std::span<const char>(value.data(), value.size())));
    constexpr char digits[] = "0123456789abcdef";
    std::string result(bytes.size() * 2, '0');
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        const auto byte = std::to_integer<unsigned>(bytes[i]);
        result[i * 2] = digits[byte >> 4U];
        result[i * 2 + 1] = digits[byte & 15U];
    }
    return result;
}

std::size_t utf8_size(const std::string_view value) {
    return static_cast<std::size_t>(std::ranges::count_if(value, [](const unsigned char c) {
        return (c & 0xc0U) != 0x80U;
    }));
}

std::size_t utf8_byte_offset(const std::string_view value, const std::size_t characters) {
    if (characters == 0) return 0;
    std::size_t seen = 0;
    for (std::size_t offset = 0; offset < value.size(); ++offset) {
        if ((static_cast<unsigned char>(value[offset]) & 0xc0U) != 0x80U && ++seen > characters)
            return offset;
    }
    if (seen == characters) return value.size();
    throw std::invalid_argument("UTF-8 character coordinate changed");
}

JsonValue::Array strings(const std::vector<std::string>& values) {
    JsonValue::Array result;
    for (const auto& value : values) result.emplace_back(value);
    return result;
}

JsonValue::Object authority_false() {
    return {{"semantic", false}, {"world", false}, {"action", false},
        {"persistent_write", false}, {"model_update", false},
        {"distribution", false}, {"p3", false}};
}

const JsonValue& member(const JsonValue::Object& object, const std::string_view key) {
    const auto found = object.find(key);
    if (found == object.end()) throw std::invalid_argument("required JSON member missing");
    return found->second;
}

std::string text_member(const JsonValue::Object& object, const std::string_view key) {
    return std::string(member(object, key).as_string());
}

std::int64_t int_member(const JsonValue::Object& object, const std::string_view key) {
    const auto* value = std::get_if<std::int64_t>(&member(object, key).storage());
    if (!value) throw std::invalid_argument("integer JSON member changed");
    return *value;
}

bool bool_member(const JsonValue::Object& object, const std::string_view key) {
    const auto* value = std::get_if<bool>(&member(object, key).storage());
    if (!value) throw std::invalid_argument("boolean JSON member changed");
    return *value;
}

void push_unique(std::vector<std::string>& values, std::string value) {
    if (std::ranges::find(values, value) == values.end()) values.push_back(std::move(value));
}

std::vector<std::string> string_array(const JsonValue& value, const char* label) {
    if (!value.is_array()) throw std::invalid_argument(std::string(label) + " must be an array");
    std::vector<std::string> result;
    for (const auto& row : value.as_array()) {
        auto text = std::string(row.as_string());
        require_text(text, label);
        result.push_back(std::move(text));
    }
    if (result.empty()) throw std::invalid_argument(std::string(label) + " must be nonempty");
    return result;
}

}  // namespace

NovelContextRequest::NovelContextRequest(
    std::string task, std::string source, std::string revision,
    std::string family, std::string question_value, std::string context,
    std::string context_digest, const std::int64_t start_char,
    const std::int64_t end_char, const std::int64_t start_line,
    const std::int64_t end_line, std::string memory_id, std::string vrs_id,
    std::string pair_id)
    : task_id(std::move(task)), source_id(std::move(source)),
      source_revision(std::move(revision)), task_family(std::move(family)),
      question(std::move(question_value)), context_text(std::move(context)),
      context_sha256(std::move(context_digest)), context_start_char(start_char),
      context_end_char(end_char), context_start_line(start_line), context_end_line(end_line),
      memory_snapshot_id(std::move(memory_id)), vrs_snapshot_id(std::move(vrs_id)),
      full_current_pair_snapshot_id(std::move(pair_id)) {
    if (!novel_task_id(task_id)) throw std::invalid_argument("novel task ID changed");
    require_text(source_id, "source_id");
    if (!digest_id(source_revision)) throw std::invalid_argument("source_revision must be a SHA-256 digest");
    if (!task_families.contains(task_family)) throw std::invalid_argument("novel task family changed");
    require_text(question, "question"); require_text(context_text, "context_text");
    if (digest_text(context_text) != context_sha256)
        throw std::invalid_argument("novel context content hash changed");
    if (context_start_char < 0 || context_end_char <= context_start_char ||
        static_cast<std::size_t>(context_end_char - context_start_char) != utf8_size(context_text) ||
        context_start_line <= 0 || context_end_line < context_start_line)
        throw std::invalid_argument("novel context coordinate changed");
    if (!digest_id(memory_snapshot_id) || !digest_id(vrs_snapshot_id) ||
        !digest_id(full_current_pair_snapshot_id))
        throw std::invalid_argument("novel snapshot binding changed");
}

NovelContextRequest NovelContextRequest::from_public_payload(
    const JsonValue::Object& value, std::string memory_snapshot_id,
    std::string vrs_snapshot_id, std::string full_current_pair_snapshot_id) {
    static const std::set<std::string, std::less<>> allowed{
        "schema_version", "task_id", "source_id", "source_revision", "task_family",
        "question", "context_text", "context_sha256", "context_start_char",
        "context_end_char", "context_start_line", "context_end_line",
        "answer_bearing_continuation_exposed"};
    for (const auto& [key, unused] : value) {
        static_cast<void>(unused);
        if (!allowed.contains(key))
            throw std::invalid_argument("public novel request contains hidden-answer fields");
    }
    if (text_member(value, "schema_version") != "rozephine-p9a-public-novel-judgment-task-v1" ||
        bool_member(value, "answer_bearing_continuation_exposed"))
        throw std::invalid_argument("public novel request reveal boundary changed");
    return {text_member(value, "task_id"), text_member(value, "source_id"),
        text_member(value, "source_revision"), text_member(value, "task_family"),
        text_member(value, "question"), text_member(value, "context_text"),
        text_member(value, "context_sha256"), int_member(value, "context_start_char"),
        int_member(value, "context_end_char"), int_member(value, "context_start_line"),
        int_member(value, "context_end_line"), std::move(memory_snapshot_id),
        std::move(vrs_snapshot_id), std::move(full_current_pair_snapshot_id)};
}

JsonValue::Object NovelContextRequest::receipt() const {
    return {{"schema_version", "rozephine-p9a-novel-context-request-v1"},
        {"task_id", task_id}, {"source_id", source_id}, {"source_revision", source_revision},
        {"task_family", task_family}, {"question", question}, {"context_sha256", context_sha256},
        {"context_coordinates", JsonValue::Object{{"start_char", context_start_char},
            {"end_char", context_end_char}, {"start_line", context_start_line},
            {"end_line", context_end_line}}}, {"memory_snapshot_id", memory_snapshot_id},
        {"VRS_snapshot_id", vrs_snapshot_id},
        {"full_current_pair_snapshot_id", full_current_pair_snapshot_id},
        {"answer_bearing_continuation_exposed", false}, {"detached_read_only_snapshot", true},
        {"authority", authority_false()}};
}

NovelJudgmentProposal::NovelJudgmentProposal(
    std::string specialist, std::string task, std::string pair,
    std::string hypothesis_value, std::string rationale_value,
    std::string confidence_value, std::vector<std::string> evidence)
    : specialist_id(std::move(specialist)), task_id(std::move(task)),
      full_current_pair_snapshot_id(std::move(pair)), hypothesis(std::move(hypothesis_value)),
      rationale(std::move(rationale_value)), confidence(std::move(confidence_value)),
      evidence_refs(std::move(evidence)) {
    require_text(specialist_id, "specialist_id");
    if (!novel_task_id(task_id)) throw std::invalid_argument("novel proposal task ID changed");
    if (!digest_id(full_current_pair_snapshot_id))
        throw std::invalid_argument("proposal pair snapshot must be a SHA-256 digest");
    require_text(hypothesis, "hypothesis"); require_text(rationale, "rationale");
    if (!confidence_levels.contains(confidence)) throw std::invalid_argument("novel proposal confidence changed");
    if (evidence_refs.empty() ||
        std::ranges::any_of(evidence_refs, [](const auto& value) { return value.empty(); }))
        throw std::invalid_argument("novel proposal needs current evidence refs");
}

JsonValue::Object NovelJudgmentProposal::receipt() const {
    return {{"schema_version", "rozephine-p9a-novel-judgment-proposal-v1"},
        {"specialist_id", specialist_id}, {"task_id", task_id},
        {"full_current_pair_snapshot_id", full_current_pair_snapshot_id},
        {"hypothesis", hypothesis}, {"rationale", rationale}, {"confidence", confidence},
        {"evidence_refs", strings(evidence_refs)}, {"proposal_only", true},
        {"persistent_state_owned", false}, {"authority", authority_false()}};
}

SealedNovelJudgment::SealedNovelJudgment(
    std::string task, std::string source, std::string revision, std::string family,
    std::string arm_value, std::string pair, std::string hypothesis_value,
    std::string rationale_value, std::string confidence_value,
    std::vector<std::string> evidence, const std::int64_t sealed_time)
    : task_id(std::move(task)), source_id(std::move(source)), source_revision(std::move(revision)),
      task_family(std::move(family)), arm(std::move(arm_value)),
      full_current_pair_snapshot_id(std::move(pair)), hypothesis(std::move(hypothesis_value)),
      rationale(std::move(rationale_value)), confidence(std::move(confidence_value)),
      evidence_refs(std::move(evidence)), sealed_at_ns(sealed_time) {
    if (!novel_task_id(task_id)) throw std::invalid_argument("sealed novel task ID changed");
    require_text(source_id, "sealed source_id");
    if (!digest_id(source_revision) || !digest_id(full_current_pair_snapshot_id))
        throw std::invalid_argument("sealed novel digest binding changed");
    if (!task_families.contains(task_family)) throw std::invalid_argument("sealed novel task family changed");
    if (!teaching_arms.contains(arm)) throw std::invalid_argument("sealed novel causal arm changed");
    require_text(hypothesis, "sealed hypothesis"); require_text(rationale, "sealed rationale");
    if (!confidence_levels.contains(confidence) || sealed_at_ns <= 0 || evidence_refs.empty())
        throw std::invalid_argument("sealed novel judgment receipt changed");
    seal_id = semantic_json_digest(JsonValue::Object{
        {"schema_version", "rozephine-p9a-sealed-novel-judgment-v1"}, {"task_id", task_id},
        {"source_id", source_id}, {"source_revision", source_revision}, {"task_family", task_family},
        {"arm", arm}, {"full_current_pair_snapshot_id", full_current_pair_snapshot_id},
        {"hypothesis", hypothesis}, {"rationale", rationale}, {"confidence", confidence},
        {"evidence_refs", strings(evidence_refs)}, {"sealed_at_ns", sealed_at_ns},
        {"answer_bearing_continuation_exposed", false}});
}

JsonValue::Object SealedNovelJudgment::receipt() const {
    return {{"schema_version", "rozephine-p9a-sealed-novel-judgment-v1"}, {"seal_id", seal_id},
        {"task_id", task_id}, {"source_id", source_id}, {"source_revision", source_revision},
        {"task_family", task_family}, {"arm", arm},
        {"full_current_pair_snapshot_id", full_current_pair_snapshot_id},
        {"hypothesis", hypothesis}, {"rationale", rationale}, {"confidence", confidence},
        {"evidence_refs", strings(evidence_refs)}, {"sealed_at_ns", sealed_at_ns},
        {"answer_bearing_continuation_exposed", false}, {"main_owned_durable_judgment", true},
        {"authority", authority_false()}};
}

SealedNovelJudgment seal_novel_judgment(
    const NovelContextRequest& request, std::string hypothesis, std::string rationale,
    std::string confidence, std::vector<std::string> evidence_refs,
    const std::int64_t sealed_at_ns, std::string arm) {
    return {request.task_id, request.source_id, request.source_revision, request.task_family,
        std::move(arm), request.full_current_pair_snapshot_id, std::move(hypothesis),
        std::move(rationale), std::move(confidence), std::move(evidence_refs), sealed_at_ns};
}

NovelOutcomeEvidence::NovelOutcomeEvidence(
    std::string task, std::string seal, std::string source, std::string revision,
    std::string outcome, std::string outcome_digest, const std::int64_t start_char,
    const std::int64_t end_char, const std::int64_t start_line, const std::int64_t end_line,
    const std::int64_t revealed, const bool independent)
    : task_id(std::move(task)), seal_id(std::move(seal)), source_id(std::move(source)),
      source_revision(std::move(revision)), outcome_text(std::move(outcome)),
      outcome_sha256(std::move(outcome_digest)), outcome_start_char(start_char),
      outcome_end_char(end_char), outcome_start_line(start_line), outcome_end_line(end_line),
      revealed_at_ns(revealed), source_read_independent_of_specialist(independent) {
    if (!novel_task_id(task_id) || !digest_id(seal_id))
        throw std::invalid_argument("novel outcome binding changed");
    require_text(source_id, "outcome source_id");
    if (!digest_id(source_revision)) throw std::invalid_argument("outcome source revision changed");
    require_text(outcome_text, "outcome_text");
    if (digest_text(outcome_text) != outcome_sha256)
        throw std::invalid_argument("novel outcome text hash changed");
    if (outcome_start_char < 0 || outcome_end_char <= outcome_start_char ||
        static_cast<std::size_t>(outcome_end_char - outcome_start_char) != utf8_size(outcome_text) ||
        outcome_start_line <= 0 || outcome_end_line < outcome_start_line || revealed_at_ns <= 0 ||
        !source_read_independent_of_specialist)
        throw std::invalid_argument("novel outcome coordinate or source boundary changed");
}

void NovelOutcomeEvidence::validate_against_seal(const SealedNovelJudgment& sealed) const {
    if (task_id != sealed.task_id || seal_id != sealed.seal_id || source_id != sealed.source_id ||
        source_revision != sealed.source_revision || revealed_at_ns <= sealed.sealed_at_ns)
        throw std::invalid_argument("novel outcome was not independently revealed after seal");
}

TeacherEvidenceSpan::TeacherEvidenceSpan(
    const std::int64_t start, const std::int64_t end, std::string text_value,
    std::string digest)
    : start_offset(start), end_offset(end), text(std::move(text_value)),
      text_sha256(std::move(digest)) {
    require_text(text, "teacher evidence span");
    if (start_offset < 0 || end_offset <= start_offset ||
        static_cast<std::size_t>(end_offset - start_offset) != utf8_size(text) ||
        digest_text(text) != text_sha256)
        throw std::invalid_argument("teacher evidence span coordinate or hash changed");
}

TeacherEvidenceSpan TeacherEvidenceSpan::exact(
    const std::string_view outcome_text, const std::int64_t start,
    const std::int64_t end) {
    if (start < 0 || end <= start) throw std::invalid_argument("teacher evidence span coordinate changed");
    const auto byte_start = utf8_byte_offset(outcome_text, static_cast<std::size_t>(start));
    const auto byte_end = utf8_byte_offset(outcome_text, static_cast<std::size_t>(end));
    std::string text(outcome_text.substr(byte_start, byte_end - byte_start));
    return {start, end, text, digest_text(text)};
}

CodexTeacherCorrection::CodexTeacherCorrection(
    std::string task, std::string seal, std::string source, std::string revision,
    std::string outcome_digest, std::string verdict_value, std::string rationale_value,
    std::vector<TeacherEvidenceSpan> spans, std::optional<std::string> counter,
    std::string confidence_value, const std::int64_t issued, std::string teacher)
    : task_id(std::move(task)), seal_id(std::move(seal)), source_id(std::move(source)),
      source_revision(std::move(revision)), outcome_sha256(std::move(outcome_digest)),
      verdict(std::move(verdict_value)), rationale(std::move(rationale_value)),
      evidence_spans(std::move(spans)), counterevidence(std::move(counter)),
      confidence(std::move(confidence_value)), issued_at_ns(issued), teacher_id(std::move(teacher)) {
    if (!novel_task_id(task_id) || !digest_id(seal_id) || !digest_id(source_revision) ||
        !digest_id(outcome_sha256)) throw std::invalid_argument("Codex teacher binding changed");
    require_text(source_id, "teacher source_id");
    if (!teacher_verdicts.contains(verdict)) throw std::invalid_argument("Codex teacher verdict changed");
    require_text(rationale, "teacher rationale");
    if (verdict != "ambiguous" && evidence_spans.empty())
        throw std::invalid_argument("binary Codex correction needs exact source evidence");
    if (verdict == "refuted" && (!counterevidence || counterevidence->empty()))
        throw std::invalid_argument("refutation needs explicit counterevidence");
    if (!confidence_levels.contains(confidence) || issued_at_ns <= 0 || teacher_id != "codex")
        throw std::invalid_argument("Codex teacher confidence, time, or identity changed");
    correction_id = semantic_json_digest(main_evidence_receipt(false));
}

std::string CodexTeacherCorrection::experience_outcome() const {
    if (verdict == "supported") return "success";
    if (verdict == "refuted") return "failure";
    return "uncertain";
}

JsonValue::Object CodexTeacherCorrection::main_evidence_receipt(
    const bool include_correction_id) const {
    JsonValue::Array spans;
    for (const auto& row : evidence_spans) spans.emplace_back(JsonValue::Object{
        {"start_offset", row.start_offset}, {"end_offset", row.end_offset},
        {"text_sha256", row.text_sha256}});
    JsonValue::Object value{{"schema_version", "rozephine-p9a-codex-teacher-correction-v1"},
        {"teacher_id", teacher_id}, {"task_id", task_id}, {"seal_id", seal_id},
        {"source_id", source_id}, {"source_revision", source_revision},
        {"outcome_sha256", outcome_sha256}, {"verdict", verdict}, {"rationale", rationale},
        {"evidence_spans", std::move(spans)},
        {"counterevidence", counterevidence ? JsonValue(*counterevidence) : JsonValue(nullptr)},
        {"confidence", confidence}, {"issued_at_ns", issued_at_ns},
        {"direct_teacher_feedback", true}, {"teacher_feedback_is_final_semantic_authority", false},
        {"teacher_feedback_can_mutate_CognitiveState", false}, {"authority", authority_false()}};
    if (include_correction_id) value.emplace("correction_id", correction_id);
    return value;
}

CodexTeacherCorrection issue_codex_teacher_correction(
    const SealedNovelJudgment& sealed, const NovelOutcomeEvidence& outcome,
    std::string verdict, std::string rationale,
    std::vector<TeacherEvidenceSpan> evidence_spans,
    std::optional<std::string> counterevidence, std::string confidence,
    const std::int64_t issued_at_ns) {
    outcome.validate_against_seal(sealed);
    for (const auto& span : evidence_spans) {
        if (span.end_offset > static_cast<std::int64_t>(utf8_size(outcome.outcome_text)))
            throw std::invalid_argument("Codex teacher span is not exact revealed source text");
        const auto exact = TeacherEvidenceSpan::exact(
            outcome.outcome_text, span.start_offset, span.end_offset);
        if (exact.text != span.text)
            throw std::invalid_argument("Codex teacher span is not exact revealed source text");
    }
    if (issued_at_ns < outcome.revealed_at_ns)
        throw std::invalid_argument("Codex teacher feedback preceded source reveal");
    return {sealed.task_id, sealed.seal_id, sealed.source_id, sealed.source_revision,
        outcome.outcome_sha256, std::move(verdict), std::move(rationale),
        std::move(evidence_spans), std::move(counterevidence), std::move(confidence), issued_at_ns};
}

SealedExperienceItem form_novel_teaching_experience(
    const NovelContextRequest& request, const SealedNovelJudgment& sealed,
    const NovelOutcomeEvidence& outcome, const CodexTeacherCorrection& correction,
    const std::int64_t observed_at_ns) {
    outcome.validate_against_seal(sealed);
    if (request.task_id != sealed.task_id ||
        request.full_current_pair_snapshot_id != sealed.full_current_pair_snapshot_id ||
        correction.task_id != sealed.task_id || correction.seal_id != sealed.seal_id ||
        correction.outcome_sha256 != outcome.outcome_sha256 || observed_at_ns < correction.issued_at_ns)
        throw std::invalid_argument("novel teaching experience lineage changed");
    std::vector<std::string> evidence = sealed.evidence_refs;
    push_unique(evidence, "novel-context-sha256:" + request.context_sha256);
    push_unique(evidence, "novel-outcome-sha256:" + outcome.outcome_sha256);
    push_unique(evidence, "codex-teacher-correction:" + correction.correction_id);
    for (const auto& span : correction.evidence_spans)
        push_unique(evidence, "teacher-span-sha256:" + span.text_sha256);
    JsonValue::Object observation{
        {"task_family", request.task_family}, {"source_family", request.source_id},
        {"context_sha256", request.context_sha256},
        {"context_coordinates", JsonValue::Object{{"start_char", request.context_start_char},
            {"end_char", request.context_end_char}, {"start_line", request.context_start_line},
            {"end_line", request.context_end_line}}}, {"outcome_sha256", outcome.outcome_sha256},
        {"outcome_coordinates", JsonValue::Object{{"start_char", outcome.outcome_start_char},
            {"end_char", outcome.outcome_end_char}, {"start_line", outcome.outcome_start_line},
            {"end_line", outcome.outcome_end_line}}}, {"Codex_teacher_verdict", correction.verdict},
        {"Codex_teacher_correction_id", correction.correction_id},
        {"Codex_teacher_rationale", correction.rationale},
        {"Codex_teacher_counterevidence", correction.counterevidence
            ? JsonValue(*correction.counterevidence) : JsonValue(nullptr)},
        {"teacher_feedback_is_final_semantic_authority", false},
        {"teacher_feedback_can_mutate_CognitiveState", false}};
    return {"novel-teaching:" + correction.correction_id, std::move(observation),
        sealed.hypothesis, correction.experience_outcome(), std::move(evidence),
        {request.source_id, "char:" + std::to_string(request.context_start_char) + '-' +
            std::to_string(request.context_end_char),
            "char:" + std::to_string(outcome.outcome_start_char) + '-' +
            std::to_string(outcome.outcome_end_char)}, request.source_id,
        "private-novel:" + request.task_family, observed_at_ns, request.source_revision,
        "pending_main_SWEGCA_teacher_evidence_verification"};
}

std::pair<ExperienceOrganizationRequest, ExperienceOrganizationProposal>
organize_novel_teaching_experience(
    std::shared_ptr<const HotMemoryIndex> base, const SealedExperienceItem& sealed) {
    if (!base) throw std::invalid_argument("novel organizer base memory required");
    const auto& observation = sealed.observation;
    const auto verdict = text_member(observation, "Codex_teacher_verdict");
    const auto family = text_member(observation, "task_family");
    const auto rationale = text_member(observation, "Codex_teacher_rationale");
    const auto authority = std::get_if<bool>(&member(observation,
        "teacher_feedback_is_final_semantic_authority").storage());
    const auto mutation = std::get_if<bool>(&member(observation,
        "teacher_feedback_can_mutate_CognitiveState").storage());
    if (sealed.verification_state != "pending_main_SWEGCA_teacher_evidence_verification" ||
        !teacher_verdicts.contains(verdict) || !task_families.contains(family) || rationale.empty() ||
        !authority || *authority || !mutation || *mutation)
        throw std::invalid_argument("novel teacher evidence did not cross the main gate");
    std::vector<std::string> dialogic_cues, dialogic_relations;
    if (const auto found = observation.find("dialogic_teacher_contract_version");
        found != observation.end()) {
        if (found->second.as_string() != "rozephine-p9c-dialogic-v1")
            throw std::invalid_argument("dialogic novel experience contract changed");
        const auto distinction = text_member(observation, "teach_back_learned_distinction");
        const auto applicability = string_array(member(observation, "teach_back_applicability_conditions"),
            "teach-back applicability");
        const auto non_applicability = string_array(
            member(observation, "teach_back_non_applicability_conditions"),
            "teach-back non-applicability");
        const auto falsifier = text_member(observation, "teach_back_future_falsifier");
        if (int_member(observation, "dialogue_rows_count_as_new_experience") != 0 ||
            int_member(observation, "canonical_outcome_bearing_episode_count") != 1 ||
            bool_member(observation, "initial_teacher_assessment_assimilated_as_separate_experience") ||
            bool_member(observation, "specialist_identity_persisted_as_cognition_owner"))
            throw std::invalid_argument("dialogic novel experience contract changed");
        const bool self_corrected = bool_member(observation, "self_correction_supported");
        dialogic_cues = {"dialogic-novel-teaching",
            std::string("dialogic-self-correction:") + (self_corrected ? "true" : "false")};
        dialogic_relations.push_back("dialogic-learned-distinction:" + distinction);
        for (const auto& row : applicability)
            dialogic_relations.push_back("dialogic-applicability:" + row);
        for (const auto& row : non_applicability)
            dialogic_relations.push_back("dialogic-non-applicability:" + row);
        dialogic_relations.push_back("dialogic-future-falsifier:" + falsifier);
        dialogic_relations.push_back("dialogue-turns-and-initial-assessment-remain-one-source-episode");
    }
    ExperienceOrganizationRequest request(std::string(base->snapshot_id()), {sealed});
    AtomicMemoryActivationOwner owner(base);
    const auto specialist = [&](const HotMemoryIndex& snapshot,
                                const ExperienceOrganizationRequest& received) {
        if (&snapshot != base.get() || received.request_sha256 != request.request_sha256)
            throw std::runtime_error("novel organizer detached snapshot changed");
        std::vector<std::string> relations{"codex-teacher-verdict:" + verdict,
            "sealed-hypothesis-outcome:" + sealed.outcome,
            "teacher-feedback-remains-evidence-not-semantic-authority",
            "teacher-rationale:" + rationale};
        if (const auto found = observation.find("Codex_teacher_counterevidence");
            found != observation.end())
            if (const auto* value = std::get_if<std::string>(&found->second.storage()); value && !value->empty())
                relations.push_back("teacher-counterevidence:" + *value);
        relations.insert(relations.end(), dialogic_relations.begin(), dialogic_relations.end());
        std::vector<std::string> cues{"private-novel-teaching",
            "novel-task-family:" + family, "codex-teacher-verdict:" + verdict};
        cues.insert(cues.end(), dialogic_cues.begin(), dialogic_cues.end());
        OrganizedExperienceItem item{sealed.source_item_id, std::move(cues), std::move(relations),
            "main sealed hypothesis: " + sealed.attempted_judgment_or_action +
            "; Codex evidence verdict: " + verdict + "; correction: " + rationale};
        return ExperienceOrganizationProposal(std::string(novel_organizer_id),
            request.request_sha256, request.base_snapshot_id, {std::move(item)});
    };
    auto proposal = run_experience_organization_specialist(
        owner, request, std::string(novel_organizer_id), specialist);
    if (owner.snapshot().get() != base.get())
        throw std::runtime_error("novel organizer changed main-owned hot memory");
    return {std::move(request), std::move(proposal)};
}

StagedNovelTeachingExperience::StagedNovelTeachingExperience(
    std::string base_id, ExperienceOrganizationRequest request_value,
    ExperienceOrganizationProposal proposal_value, MemoryEpisode episode_value,
    std::shared_ptr<const HotMemoryIndex> memory)
    : base_pair_snapshot_id(std::move(base_id)), request(std::move(request_value)),
      proposal(std::move(proposal_value)), episode(std::move(episode_value)),
      staged_memory(std::move(memory)) {
    if (!digest_id(base_pair_snapshot_id) || !staged_memory ||
        request.base_snapshot_id == staged_memory->snapshot_id() ||
        !staged_memory->contains_episode(episode.episode_id))
        throw std::invalid_argument("novel staged-memory successor changed");
}

StagedNovelTeachingExperience stage_novel_teaching_experience(
    const FullCurrentMemoryVrsSnapshot& base_pair, const SealedExperienceItem& sealed) {
    auto [request, proposal] = organize_novel_teaching_experience(base_pair.memory, sealed);
    auto episodes = materialize_organized_experience_episodes(*base_pair.memory, request, proposal);
    if (episodes.size() != 1 || episodes.front().steps.front().outcome != sealed.outcome)
        throw std::runtime_error("novel organizer changed the main-sealed outcome");
    auto staged_memory = append_memory_activation_index(base_pair.memory, episodes);
    const auto* composite = dynamic_cast<const CompositeMemoryActivationIndex*>(staged_memory.get());
    bool shared = false;
    if (composite) {
        if (const auto* base_composite = dynamic_cast<const CompositeMemoryActivationIndex*>(base_pair.memory.get())) {
            shared = composite->sources.size() == base_composite->sources.size() + 1 &&
                std::equal(base_composite->sources.begin(), base_composite->sources.end(),
                    composite->sources.begin());
        } else shared = !composite->sources.empty() && composite->sources.front() == base_pair.memory;
    }
    if (!shared) throw std::runtime_error("novel memory successor lost structural sharing");
    return {base_pair.snapshot_id, std::move(request), std::move(proposal),
        episodes.front(), std::move(staged_memory)};
}

NovelVrsConvergenceEvidence::NovelVrsConvergenceEvidence(
    std::string snapshot, const bool contributed, const std::size_t removed,
    const double strength, std::optional<double> cutoff)
    : vrs_snapshot_id(std::move(snapshot)), all_required_outcomes_contributed(contributed),
      pre_convergence_connections_removed(removed),
      verified_experience_promotion_strength(strength),
      post_convergence_cutoff(cutoff) {
    if (!digest_id(vrs_snapshot_id) || !all_required_outcomes_contributed ||
        pre_convergence_connections_removed != 0 ||
        verified_experience_promotion_strength < 1.0 || post_convergence_cutoff)
        throw std::invalid_argument("novel VRS convergence evidence failed closed");
}

PublishedNovelTeachingExperience publish_staged_novel_teaching_experience(
    AtomicFullCurrentMemoryVrsOwner& owner, const FullCurrentMemoryVrsSnapshot& base_pair,
    const StagedNovelTeachingExperience& staged,
    const NovelVrsConvergenceEvidence& convergence) {
    if (staged.base_pair_snapshot_id != base_pair.snapshot_id)
        throw std::invalid_argument("novel staged experience belongs to another pair");
    bool rollback_rejected = false;
    try { FullCurrentMemoryVrsSnapshot invalid(staged.staged_memory, "invalid-vrs"); }
    catch (const std::invalid_argument&) { rollback_rejected = true; }
    auto held = owner.snapshot();
    if (held.snapshot_id != base_pair.snapshot_id || !rollback_rejected)
        throw std::runtime_error("novel partial-pair rollback boundary failed");
    FullCurrentMemoryVrsSnapshot replacement(staged.staged_memory, convergence.vrs_snapshot_id);
    owner.replace(held.snapshot_id, replacement);
    bool stale_rejected = false;
    try { owner.replace(held.snapshot_id, replacement); }
    catch (const std::invalid_argument&) { stale_rejected = true; }
    if (owner.snapshot().snapshot_id != replacement.snapshot_id || !stale_rejected)
        throw std::runtime_error("novel stale CAS overwrote persistent cognition");
    JsonValue::Object receipt{{"base_pair_snapshot_id", base_pair.snapshot_id},
        {"replacement_pair_snapshot_id", replacement.snapshot_id},
        {"memory_snapshot_id", std::string(replacement.memory->snapshot_id())},
        {"VRS_snapshot_id", replacement.vrs_snapshot_id},
        {"memory_and_VRS_replaced_by_one_main_owned_CAS", true},
        {"invalid_partial_pair_rollback_rejected", true}, {"stale_CAS_rejected", true},
        {"incremental_copy_on_write", true}, {"structural_sharing", true},
        {"full_corpus_rebuild", false}, {"lookup_requires_io", replacement.memory->lookup_requires_io()},
        {"added_episode_count", 1},
        {"verified_experience_promotion_strength", convergence.verified_experience_promotion_strength},
        {"authority", authority_false()}};
    return {std::move(replacement), std::move(receipt)};
}

MemoryActivationReceipt activate_novel_teaching_memory(
    const FullCurrentMemoryVrsSnapshot& pair, const NovelContextRequest& request,
    const std::set<std::string, std::less<>>& promoted_episode_ids) {
    if (request.memory_snapshot_id != pair.memory->snapshot_id() ||
        request.vrs_snapshot_id != pair.vrs_snapshot_id ||
        request.full_current_pair_snapshot_id != pair.snapshot_id)
        throw std::invalid_argument("novel memory activation snapshot binding changed");
    const auto task_cue = "task-family:private-novel:" + request.task_family;
    auto selection = select_runtime_cues(*pair.memory, request.question,
        {task_cue, "novel-task-family:" + request.task_family, "private-novel-teaching"},
        {task_cue});
    const auto context_ref = "novel-context-sha256:" + request.context_sha256;
    return activate_memory(*pair.memory, request.question, selection.selected_cues,
        [&](const ReplayedEpisode& episode) {
            const auto& observation = episode.steps.front().observation;
            const auto found = observation.find("task_family");
            const bool same_family = found != observation.end() &&
                found->second.as_string() == "private-novel:" + request.task_family;
            const bool promoted = same_family && promoted_episode_ids.contains(episode.episode_id);
            return CurrentEvidenceVerdict(episode.episode_id,
                "same-family-analogy:" + request.task_family,
                promoted ? "support" : "insufficient",
                promoted ? "same task family and current VRS promotion make this a relevant analogy, not historical truth"
                    : !same_family ? "experience remains recalled but belongs to another task family"
                                   : "experience remains recalled but is not promoted by this VRS projection",
                promoted ? std::vector<std::string>{context_ref} : std::vector<std::string>{});
        });
}

}  // namespace swegca::world
