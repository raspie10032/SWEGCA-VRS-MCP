#include "world/recorded_claims.hpp"

#include "world/semantic_input_scope.hpp"
#include "world/semantic_source_context.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace swegca::world {
namespace {

[[nodiscard]] const JsonValue* find(const JsonValue::Object& object,
                                    const std::string_view key) noexcept {
    const auto found = object.find(key);
    return found == object.end() ? nullptr : &found->second;
}

[[nodiscard]] bool nonblank(const std::string_view value) noexcept {
    return std::ranges::any_of(value, [](const unsigned char c) {
        return c != ' ' && c != '\t' && c != '\n' && c != '\r' &&
               c != '\f' && c != '\v';
    });
}

[[nodiscard]] bool nonblank_string(const JsonValue* value) noexcept {
    const auto* text = value ? std::get_if<std::string>(&value->storage()) : nullptr;
    return text && nonblank(*text);
}

template<class T>
void append_unique(std::vector<T>& values, const T& value) {
    if (std::ranges::find(values, value) == values.end()) values.push_back(value);
}

[[nodiscard]] std::vector<std::string> string_array(const JsonValue* value,
                                                     bool& valid) {
    std::vector<std::string> result;
    valid = value && value->is_array() && !value->as_array().empty();
    if (!valid) return result;
    result.reserve(value->as_array().size());
    for (const auto& item : value->as_array()) {
        const auto* text = std::get_if<std::string>(&item.storage());
        if (!text || text->empty()) {
            valid = false;
            result.clear();
            return result;
        }
        result.push_back(*text);
    }
    return result;
}

[[nodiscard]] bool valid_evidence_kind(const JsonValue* value) noexcept {
    const auto* text = value ? std::get_if<std::string>(&value->storage()) : nullptr;
    if (!text) return false;
    return *text == "learned_prediction" || *text == "observed_evidence" ||
           *text == "deterministic_simulation" || *text == "user_claim" ||
           *text == "external_model_claim";
}

[[nodiscard]] bool scalar_claim_value(const JsonValue& value) noexcept {
    return std::holds_alternative<std::nullptr_t>(value.storage()) ||
           std::holds_alternative<bool>(value.storage()) ||
           std::holds_alternative<std::int64_t>(value.storage()) ||
           std::holds_alternative<JsonInteger>(value.storage()) ||
           std::holds_alternative<double>(value.storage()) ||
           std::holds_alternative<std::string>(value.storage());
}

[[nodiscard]] RecordedEncodedEvent convert_session_event(SessionEncodedEvent event) {
    std::vector<RecordedEncodedClaim> claims;
    claims.reserve(event.claims.size());
    for (auto& claim : event.claims) {
        std::vector<RecordedSourceContext> contexts;
        contexts.reserve(claim.semantic_anchor_resolution.input_context.size());
        for (auto& context : claim.semantic_anchor_resolution.input_context)
            contexts.emplace_back(std::move(context));
        RecordedSemanticAnchorResolution resolution{
            std::move(claim.semantic_anchor_resolution.referenced_anchors),
            std::move(claim.semantic_anchor_resolution.unresolved_anchors),
            claim.semantic_anchor_resolution.other_unresolved_anchor_count,
            std::move(claim.semantic_anchor_resolution.attributable_input_steps),
            claim.semantic_anchor_resolution.response_incomplete(),
            std::move(contexts),
            std::move(claim.semantic_anchor_resolution.attributable_document_key)};
        claims.push_back({
            {std::move(claim.address.episode_id), claim.address.step, claim.address.claim},
            JsonValue(std::move(claim.subject)), std::move(claim.predicate),
            std::move(claim.value), claim.reported_confidence,
            std::move(claim.semantic_unit), std::move(claim.semantic_key),
            std::move(claim.semantic_graph_addresses), std::move(claim.unresolved),
            std::move(resolution)});
    }
    return {std::move(event.episode_id), event.step,
            JsonValue(std::move(event.event_id)), JsonValue(std::move(event.event_type)),
            JsonValue(std::move(event.evidence_kind)), std::move(claims),
            std::move(event.declared_source), std::move(event.declared_evidence_refs),
            std::move(event.source_addresses), std::move(event.evidence_refs),
            std::move(event.revision), std::move(event.outcome),
            std::move(event.verification_state), std::move(event.unresolved),
            std::move(event.semantic_encoding), event.interpreted_input_steps()};
}

[[nodiscard]] RecordedEncodedEvent prepare_semantic_event(
    const SemanticSourceEpisode& episode, const std::size_t ordinal,
    const RecordedSourceLookup& source_lookup) {
    const auto& step = episode.steps.at(ordinal);
    const auto& record = step.observation;
    std::vector<RecordedEncodedClaim> claims;
    std::vector<std::string> unresolved;
    std::vector<std::size_t> input_steps;
    RecordedSemanticEncoding retained_encoding;
    try {
        if (!record.is_object()) throw std::invalid_argument("semantic_parent_lookup_unavailable");
        const auto* source_id_value = find(record.as_object(), "source_id");
        const auto* source_id = source_id_value
            ? std::get_if<std::string>(&source_id_value->storage()) : nullptr;
        if (!source_lookup || !source_id)
            throw std::invalid_argument("semantic_parent_lookup_unavailable");
        if (*source_id == episode.episode_id)
            throw std::invalid_argument("semantic_self_parent");
        const auto* parent = source_lookup(*source_id);
        if (!parent) throw std::invalid_argument("semantic_parent_unavailable");

        auto encoding = restore_semantic_encoding(record, *parent);
        if (std::ranges::find(step.evidence_refs, *source_id) == step.evidence_refs.end() ||
            episode.source_addresses != encoding.source_addresses)
            throw std::invalid_argument("semantic_parent_lineage_changed");

        for (const auto value : interpreted_input_steps(*parent, encoding))
            input_steps.push_back(static_cast<std::size_t>(value));
        const auto context_by_anchor = input_context_by_anchor(encoding);
        const auto local_anchors = attributable_text_anchors(
            *parent, encoding, &context_by_anchor);
        const auto graph = prepare_claim_graph_addresses(encoding, episode.episode_id);
        const std::unordered_set<std::string> unresolved_anchors(
            encoding.unresolved.begin(), encoding.unresolved.end());
        const std::unordered_set<std::size_t> partial(
            encoding.partial_response_units.begin(),
            encoding.partial_response_units.end());

        claims.reserve(encoding.units.size());
        for (std::size_t index = 0; index != encoding.units.size(); ++index) {
            const auto& unit = encoding.units[index];
            std::vector<std::string> references;
            for (const auto& anchor : unit.anchors) append_unique(references, anchor);
            for (const auto& qualifier : unit.qualifiers)
                for (const auto& anchor : qualifier.anchors)
                    append_unique(references, anchor);
            std::vector<std::string> affected;
            for (const auto& anchor : references)
                if (unresolved_anchors.contains(anchor)) affected.push_back(anchor);
            const bool response_incomplete = partial.contains(index);
            std::vector<std::size_t> local_steps;
            bool all_local = !references.empty();
            for (const auto& anchor : references) {
                const auto found = local_anchors.find(anchor);
                if (found == local_anchors.end()) { all_local = false; break; }
                local_steps.push_back(static_cast<std::size_t>(found->second));
            }
            if (!affected.empty() && !response_incomplete) all_local = false;
            if (all_local) {
                std::ranges::sort(local_steps);
                local_steps.erase(std::unique(local_steps.begin(), local_steps.end()),
                                  local_steps.end());
            } else local_steps.clear();

            std::vector<RecordedSourceContext> contexts;
            std::set<std::string, std::less<>> seen_contexts;
            for (const auto& anchor : references) {
                const auto found = context_by_anchor.find(anchor);
                if (found == context_by_anchor.end()) continue;
                for (const auto& row : found->second) {
                    const auto key = semantic_canonical_json(row.to_json());
                    if (seen_contexts.insert(key).second) contexts.emplace_back(row);
                }
            }
            RecordedSemanticAnchorResolution resolution{
                references, affected, unresolved_anchors.size() - affected.size(),
                std::move(local_steps), response_incomplete, std::move(contexts), {}};
            claims.push_back({
                {episode.episode_id, ordinal, index}, JsonValue(unit.subject),
                unit.predicate, unit.value, std::nullopt, unit,
                prepare_proposition_key(unit), graph.at(index), {},
                std::move(resolution)});
        }
        for (const auto& anchor : encoding.unresolved)
            unresolved.push_back("semantic_anchor_unresolved:" + anchor);
        retained_encoding = std::move(encoding);
    } catch (const std::exception&) {
        claims.clear();
        input_steps.clear();
        retained_encoding = std::monostate{};
        unresolved = {"semantic_source_binding_unresolved"};
    }
    return {episode.episode_id, ordinal, JsonValue(episode.episode_id),
            JsonValue("source_bound_semantic_proposal"),
            JsonValue("semantic_interpretation_proposal"), std::move(claims),
            JsonValue(nullptr), {}, episode.source_addresses, step.evidence_refs,
            episode.revision, step.outcome, episode.verification_state,
            std::move(unresolved), std::move(retained_encoding),
            std::move(input_steps)};
}

}  // namespace

bool is_recorded_event(const JsonValue& observation) noexcept {
    if (!observation.is_object()) return false;
    const auto& object = observation.as_object();
    const auto* schema = find(object, "schema");
    if (schema && std::holds_alternative<std::string>(schema->storage())) {
        const auto value = schema->as_string();
        if (value == semantic_encoding_schema || value == session_semantic_input_schema)
            return true;
    }
    constexpr std::string_view required[]{
        "event_id", "event_type", "source", "claims", "evidence_kind"};
    return std::ranges::all_of(required, [&](const auto key) {
        return object.contains(key);
    });
}

std::optional<RecordedEncodedEvent> prepare_recorded_event(
    const SemanticSourceEpisode& episode, const std::size_t ordinal,
    const RecordedSourceLookup& source_lookup,
    const PreparedSessionView* source_memory,
    const std::vector<SemanticSourceEpisode>& source_episodes) {
    if (ordinal >= episode.steps.size())
        throw std::out_of_range("recorded event ordinal outside episode");
    const auto& step = episode.steps[ordinal];
    const auto& observation = step.observation;
    if (!observation.is_object()) return std::nullopt;
    const auto& object = observation.as_object();
    const auto* schema_value = find(object, "schema");
    const auto* schema = schema_value
        ? std::get_if<std::string>(&schema_value->storage()) : nullptr;
    if (schema && *schema == session_semantic_input_schema)
        return convert_session_event(prepare_session_recorded_event(
            episode, ordinal, source_memory, source_episodes));
    if (schema && *schema == semantic_encoding_schema)
        return prepare_semantic_event(episode, ordinal, source_lookup);
    if (!is_recorded_event(observation)) return std::nullopt;

    static const std::set<std::string, std::less<>> event_fields{
        "event_id", "event_type", "source", "claims", "evidence_refs",
        "evidence_kind", "metadata"};
    std::vector<std::string> unresolved;
    std::vector<RecordedEncodedClaim> claims;
    if (object.size() != event_fields.size() ||
        !std::ranges::all_of(event_fields, [&](const auto& key) {
            return object.contains(key);
        }))
        append_unique(unresolved, std::string("recorded_event_fields_unresolved"));

    const auto* metadata = find(object, "metadata");
    if (!metadata || !metadata->is_object() || !metadata->as_object().empty())
        append_unique(unresolved, std::string("recorded_event_metadata_unresolved"));

    const auto* source = find(object, "source");
    bool source_valid = source && source->is_object() && source->as_object().size() == 3;
    if (source_valid) {
        static constexpr std::string_view fields[]{"representation", "adapter", "source_ref"};
        source_valid = std::ranges::all_of(fields, [&](const auto key) {
            return nonblank_string(find(source->as_object(), key));
        });
    }
    if (!source_valid)
        append_unique(unresolved, std::string("recorded_event_source_unresolved"));

    bool refs_valid = false;
    auto refs = string_array(find(object, "evidence_refs"), refs_valid);
    if (!refs_valid) {
        append_unique(unresolved, std::string("recorded_event_refs_unresolved"));
        refs.clear();
    }
    if (!valid_evidence_kind(find(object, "evidence_kind")))
        append_unique(unresolved, std::string("recorded_event_kind_unresolved"));
    if (!nonblank_string(find(object, "event_id")) ||
        !nonblank_string(find(object, "event_type")))
        append_unique(unresolved, std::string("recorded_event_identity_unresolved"));

    const auto* rows = find(object, "claims");
    if (!rows || !rows->is_array() || rows->as_array().empty()) {
        append_unique(unresolved, std::string("recorded_event_claims_unresolved"));
    } else {
        static const std::set<std::string, std::less<>> claim_fields{
            "subject", "predicate", "value", "confidence"};
        for (std::size_t index = 0; index != rows->as_array().size(); ++index) {
            const auto& row_value = rows->as_array()[index];
            if (!row_value.is_object() || row_value.as_object().size() != claim_fields.size() ||
                !std::ranges::all_of(claim_fields, [&](const auto& key) {
                    return row_value.is_object() && row_value.as_object().contains(key);
                })) {
                append_unique(unresolved, std::string("recorded_claim_fields_unresolved"));
                continue;
            }
            const auto& row = row_value.as_object();
            const auto* predicate = find(row, "predicate");
            const auto* confidence = find(row, "confidence");
            const auto* confidence_double = confidence
                ? std::get_if<double>(&confidence->storage()) : nullptr;
            const auto* confidence_integer = confidence
                ? std::get_if<std::int64_t>(&confidence->storage()) : nullptr;
            const double score = confidence_double ? *confidence_double
                : confidence_integer ? static_cast<double>(*confidence_integer)
                : -1.0;
            if (!nonblank_string(predicate) || !std::isfinite(score) ||
                score < 0.0 || score > 1.0) {
                append_unique(unresolved, std::string("recorded_claim_shape_unresolved"));
                continue;
            }
            const auto& subject = row.at("subject");
            if (!nonblank_string(&subject))
                append_unique(unresolved, std::string("recorded_claim_subject_unresolved"));
            const auto& value = row.at("value");
            std::vector<std::string> value_problems;
            if (!scalar_claim_value(value))
                value_problems.push_back("recorded_claim_value_semantics_unresolved");
            else if (const auto* number = std::get_if<double>(&value.storage());
                     number && !std::isfinite(*number))
                value_problems.push_back("recorded_claim_nonfinite_value");
            for (const auto& problem : value_problems) append_unique(unresolved, problem);
            claims.push_back({{episode.episode_id, ordinal, index}, subject,
                std::string(predicate->as_string()), value, score, std::nullopt,
                std::nullopt, std::monostate{}, std::move(value_problems), std::nullopt});
        }
    }

    const auto missing = JsonValue(nullptr);
    return RecordedEncodedEvent{
        episode.episode_id, ordinal,
        find(object, "event_id") ? *find(object, "event_id") : missing,
        find(object, "event_type") ? *find(object, "event_type") : missing,
        find(object, "evidence_kind") ? *find(object, "evidence_kind") : missing,
        std::move(claims), source ? *source : missing, std::move(refs),
        episode.source_addresses, step.evidence_refs, episode.revision,
        step.outcome, episode.verification_state, std::move(unresolved),
        std::monostate{}, {}};
}

}  // namespace swegca::world
