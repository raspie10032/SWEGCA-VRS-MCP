#include "world/existing_semantic_input.hpp"

#include "world/semantic_encoding.hpp"
#include "world/semantic_vrs_ingress.hpp"

#include <algorithm>
#include <set>
#include <stdexcept>

namespace swegca::world {
namespace {

JsonValue::Array strings(const std::vector<std::string>& values) {
    JsonValue::Array result;
    for (const auto& value : values) result.emplace_back(value);
    return result;
}

JsonValue::Array proposals(const std::vector<SemanticEncoding>& values) {
    JsonValue::Array result;
    for (const auto& value : values) result.push_back(semantic_encoding_receipt(value));
    return result;
}

JsonValue::Array failures(const std::vector<SemanticPreparationFailure>& values) {
    JsonValue::Array result;
    for (const auto& value : values) result.emplace_back(value.receipt());
    return result;
}

const JsonValue& required(const JsonValue::Object& row, const std::string_view key) {
    const auto found = row.find(key);
    if (found == row.end()) throw std::invalid_argument("existing_semantic_input_receipt_changed");
    return found->second;
}

std::vector<std::string> text_array(const JsonValue& value) {
    if (!value.is_array()) throw std::invalid_argument("existing_semantic_input_receipt_changed");
    std::vector<std::string> result;
    for (const auto& item : value.as_array()) {
        const auto* text = std::get_if<std::string>(&item.storage());
        if (!text || text->empty())
            throw std::invalid_argument("existing_semantic_input_receipt_changed");
        result.push_back(*text);
    }
    return result;
}

}  // namespace

SemanticSourceEpisode semantic_source_episode(const MemoryEpisode& episode) {
    std::vector<SemanticMemoryStep> steps;
    for (const auto& step : episode.steps)
        steps.push_back({step.phase, JsonValue(step.observation), step.relations,
            step.judgment, step.outcome, step.evidence_refs});
    return {episode.episode_id, episode.cues, std::move(steps), episode.source_addresses,
        episode.revision, episode.verification_state};
}

ExistingSemanticInput bind_existing_semantics(
    const HotMemoryIndex& memory, const PreparedSemanticBatch& incoming) {
    if (incoming.proposals.empty())
        throw std::invalid_argument("prepared_existing_semantic_batch_required");
    std::vector<SemanticSourceEpisode> sources;
    sources.reserve(incoming.source_ids.size());
    for (const auto& identifier : incoming.source_ids)
        sources.push_back(semantic_source_episode(memory.episode(identifier)));
    auto proposed = incoming.for_wave(sources);
    std::map<std::string, const SemanticSourceEpisode*, std::less<>> by_id;
    for (const auto& source : sources)
        if (!by_id.emplace(source.episode_id, &source).second)
            throw std::invalid_argument("prepared_existing_semantic_source_binding_changed");
    std::vector<SemanticEncoding> validated;
    for (const auto& proposal : proposed)
        validated.push_back(restore_semantic_encoding(
            semantic_encoding_receipt(proposal), *by_id.at(proposal.source_id)));
    if (incoming.elapsed_ns < 0)
        throw std::invalid_argument("prepared_existing_semantic_source_binding_changed");
    for (const auto& failure : incoming.failures) {
        const auto found = by_id.find(failure.source_id);
        if (found == by_id.end() || failure.source_revision != found->second->revision ||
            SemanticPreparationFailure::from_receipt(failure.receipt()).receipt() != failure.receipt())
            throw std::invalid_argument("prepared_existing_semantic_source_binding_changed");
    }
    for (const auto& proposal : validated) {
        const auto derivative = semantic_encoding_episode(proposal);
        if (memory.contains_episode(derivative.episode_id))
            throw std::invalid_argument("semantic_derivative_already_accumulated");
    }
    PreparedSemanticBatch batch{incoming.source_ids, std::move(validated),
        incoming.failures, incoming.elapsed_ns};
    JsonValue payload(JsonValue::Object{
        {"schema", "rozephine-existing-semantic-input-v1"},
        {"source_ids", strings(batch.source_ids)}, {"proposals", proposals(batch.proposals)},
        {"failures", failures(batch.failures)},
        {"added_distinct_source_episode_count", std::int64_t{0}},
        {"new_observation_count", std::int64_t{0}},
        {"independent_evidence_count", std::int64_t{0}}, {"grants_authority", false}});
    auto canonical = semantic_canonical_json(payload);
    auto sha256 = semantic_json_digest(payload);
    return {std::move(sources), std::move(batch), std::move(payload), canonical,
        std::move(sha256)};
}

ExistingSemanticInput restore_existing_semantic_input(
    const JsonValue& payload, const HotMemoryIndex& memory) {
    if (!payload.is_object())
        throw std::invalid_argument("existing_semantic_input_receipt_changed");
    const auto& row = payload.as_object();
    if (row.size() != 8 || required(row, "schema").as_string() !=
            "rozephine-existing-semantic-input-v1")
        throw std::invalid_argument("existing_semantic_input_receipt_changed");
    auto ids = text_array(required(row, "source_ids"));
    std::map<std::string, SemanticSourceEpisode, std::less<>> sources;
    for (const auto& identifier : ids)
        sources.emplace(identifier, semantic_source_episode(memory.episode(identifier)));
    if (!required(row, "proposals").is_array() || !required(row, "failures").is_array())
        throw std::invalid_argument("existing_semantic_input_receipt_changed");
    std::vector<SemanticEncoding> encodings;
    for (const auto& value : required(row, "proposals").as_array()) {
        const auto& object = value.as_object();
        const auto id = object.at("source_id").as_string();
        encodings.push_back(restore_semantic_encoding(value, sources.at(std::string(id))));
    }
    std::vector<SemanticPreparationFailure> failure_rows;
    for (const auto& value : required(row, "failures").as_array())
        failure_rows.push_back(SemanticPreparationFailure::from_receipt(value.as_object()));
    auto bound = bind_existing_semantics(memory,
        PreparedSemanticBatch{ids, std::move(encodings), std::move(failure_rows), 0});
    if (bound.payload != payload)
        throw std::invalid_argument("existing_semantic_input_receipt_changed");
    return bound;
}

}  // namespace swegca::world
