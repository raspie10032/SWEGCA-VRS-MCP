#include "world/session_semantic_binding.hpp"

#include "transport/json.hpp"
#include "world/unicode_nfkc.hpp"

#include <algorithm>
#include <charconv>
#include <limits>
#include <memory_resource>
#include <set>
#include <stdexcept>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace swegca::world {
namespace {

[[noreturn]] void reject(const char* reason) { throw std::invalid_argument(reason); }

[[nodiscard]] bool text(const std::string_view value) {
    return !strip_unicode_whitespace(value).empty();
}

[[nodiscard]] JsonValue detached_json(const transport::Json& source) {
    switch (source.kind) {
    case transport::Json::Kind::null: return JsonValue(nullptr);
    case transport::Json::Kind::boolean: return JsonValue(source.scalar == "true");
    case transport::Json::Kind::string: return JsonValue(source.scalar);
    case transport::Json::Kind::number: {
        std::int64_t integer{};
        const auto parsed_integer = std::from_chars(
            source.scalar.data(), source.scalar.data() + source.scalar.size(), integer);
        if (parsed_integer.ec == std::errc{} &&
            parsed_integer.ptr == source.scalar.data() + source.scalar.size())
            return JsonValue(integer);
        double number{};
        const auto parsed_number = std::from_chars(
            source.scalar.data(), source.scalar.data() + source.scalar.size(),
            number, std::chars_format::general);
        if (parsed_number.ec != std::errc{} ||
            parsed_number.ptr != source.scalar.data() + source.scalar.size())
            reject("session_semantic_input_changed");
        return JsonValue(number);
    }
    case transport::Json::Kind::array: {
        JsonValue::Array values;
        values.reserve(source.values.size());
        for (const auto& value : source.values) values.push_back(detached_json(value));
        return JsonValue(std::move(values));
    }
    case transport::Json::Kind::object: {
        JsonValue::Object values;
        for (std::size_t index = 0; index != source.values.size(); ++index)
            values.emplace(source.keys[index], detached_json(source.values[index]));
        return JsonValue(std::move(values));
    }
    }
    reject("session_semantic_input_changed");
}

[[nodiscard]] JsonValue strings(const std::vector<std::string>& values) {
    JsonValue::Array result;
    result.reserve(values.size());
    for (const auto& value : values) result.emplace_back(value);
    return JsonValue(std::move(result));
}

void append_speech_entity_cues(const JsonValue& value, std::vector<std::string>& cues) {
    if (!value.is_object()) return;
    const auto& fields = value.as_object();
    const auto schema = fields.find("schema");
    if (schema == fields.end() ||
        !std::holds_alternative<std::string>(schema->second.storage()) ||
        schema->second.as_string() != "rozephine-speech-value-v1") return;
    const auto append = [&](const JsonValue& referent) {
        if (!referent.is_object()) return;
        const auto entity = referent.as_object().find("entity");
        if (entity != referent.as_object().end() &&
            std::holds_alternative<std::string>(entity->second.storage()))
            cues.emplace_back(entity->second.as_string());
    };
    if (const auto speaker = fields.find("speaker"); speaker != fields.end() &&
        !std::holds_alternative<std::nullptr_t>(speaker->second.storage())) append(speaker->second);
    for (const auto name : {"addressees", "topics"})
        if (const auto rows = fields.find(name); rows != fields.end() && rows->second.is_array())
            for (const auto& row : rows->second.as_array()) append(row);
}

[[nodiscard]] bool same_anchor(const SemanticAnchor& left,
                               const SemanticAnchor& right) {
    return left.identifier == right.identifier && left.step == right.step &&
        left.path == right.path && left.modality == right.modality &&
        left.role == right.role && left.char_range == right.char_range &&
        left.region == right.region && left.time_ns == right.time_ns;
}

[[nodiscard]] std::vector<std::string> references(const SemanticMeaningUnit& unit) {
    std::vector<std::string> result;
    std::set<std::string> seen;
    const auto insert = [&](const std::string& value) {
        if (seen.insert(value).second) result.push_back(value);
    };
    for (const auto& value : unit.anchors) insert(value);
    for (const auto& qualifier : unit.qualifiers)
        for (const auto& value : qualifier.anchors) insert(value);
    return result;
}

[[nodiscard]] SemanticSourceEpisode normalized_episode(
    const SemanticSourceEpisode& source) {
    (void)semantic_source_digest(source);
    auto result = source;
    std::vector<std::string> cues;
    std::set<std::string> seen;
    for (const auto& cue : source.cues) {
        auto normalized = unicode_casefold(collapse_unicode_whitespace(cue));
        if (!text(normalized)) reject("cue must not be empty");
        if (seen.insert(normalized).second) cues.push_back(std::move(normalized));
    }
    if (cues.empty()) reject("memory episode is incomplete");
    result.cues = std::move(cues);
    return result;
}

[[nodiscard]] std::size_t event_ordinal(const SemanticAnchor& anchor) {
    if (anchor.path.size() < 2 ||
        !std::holds_alternative<std::string>(anchor.path[0]) ||
        std::get<std::string>(anchor.path[0]) != "historical_records" ||
        !std::holds_alternative<std::int64_t>(anchor.path[1]) ||
        std::get<std::int64_t>(anchor.path[1]) < 0)
        reject("session semantic event anchor required");
    return static_cast<std::size_t>(std::get<std::int64_t>(anchor.path[1]));
}

void validate_binding(const BoundSessionSemantics& bound) {
    if (!text(bound.memory_snapshot_id) || bound.document_key.empty() ||
        !std::ranges::all_of(bound.document_key, [](const auto& value) { return text(value); }) ||
        !text(bound.derivative.episode_id))
        reject("session_semantic_graph_parent_required");

    std::unordered_set<std::string> parents;
    for (const auto& parent : bound.original_episodes)
        if (!text(parent.episode_id) || !parents.insert(parent.episode_id).second)
            reject("session_semantic_graph_parent_required");

    std::unordered_map<std::string, const SemanticAnchor*> anchors;
    for (const auto& anchor : bound.source_parts)
        if (!text(anchor.identifier) || !anchors.emplace(anchor.identifier, &anchor).second)
            reject("session_semantic_graph_parent_required");

    for (const auto& row : bound.units) {
        std::vector<std::string> references = row.unit.anchors;
        for (const auto& qualifier : row.unit.qualifiers)
            references.insert(references.end(), qualifier.anchors.begin(), qualifier.anchors.end());
        std::vector<std::string> expected;
        std::unordered_set<std::string> seen;
        for (const auto& identifier : references)
            if (seen.insert(identifier).second) expected.push_back(identifier);
        if (expected.size() != row.anchors.size())
            reject("session_semantic_graph_parent_required");
        for (std::size_t index = 0; index < expected.size(); ++index)
            if (!anchors.contains(expected[index]) ||
                row.anchors[index].identifier != expected[index] ||
                !same_anchor(row.anchors[index], *anchors.at(expected[index])))
                reject("session_semantic_graph_parent_required");
    }
    for (const auto& identifier : bound.unresolved)
        if (!anchors.contains(identifier)) reject("session_semantic_graph_parent_required");
}

}  // namespace

std::shared_ptr<const BoundSessionSemantics> bind_session_semantics(
    const PreparedSessionView& view,
    const std::vector<SemanticSourceEpisode>& source_episodes,
    const SessionSpeechInterpretation& supplied_interpretation,
    const std::string_view expected_memory_snapshot_id) {
    if (view.memory_snapshot_id != expected_memory_snapshot_id)
        reject("session_semantic_memory_generation_changed");
    const auto key = supplied_interpretation.source.document_key;
    auto entry = view.get(key, expected_memory_snapshot_id);
    if (!entry)
        reject("session_semantic_document_not_prepared");
    const auto interpretation = restore_session_speech_interpretation(
        session_speech_receipt(supplied_interpretation), *entry);
    if (!text(interpretation.model))
        reject("session_semantic_model_identity_required");
    const auto indexed = view.directory.fragments(key, expected_memory_snapshot_id);
    std::set<std::tuple<std::string, std::size_t, std::size_t>> indexed_members;
    for (const auto& row : indexed)
        indexed_members.emplace(row.episode_id, row.step, row.character_offset);
    std::set<std::tuple<std::string, std::size_t, std::size_t>> fragment_members;
    for (const auto& row : entry->document.fragments)
        fragment_members.emplace(row.episode_id, row.step, row.character_offset);
    if (indexed_members != fragment_members)
        reject("session_semantic_document_membership_changed");

    std::map<std::string, std::size_t, std::less<>> original_indices;
    std::vector<SemanticSourceEpisode> parent_episodes;
    for (const auto& fragment : entry->document.fragments) {
        const auto source = std::ranges::find_if(source_episodes, [&](const auto& episode) {
            return episode.episode_id == fragment.episode_id;
        });
        if (source == source_episodes.end() || source->revision != fragment.revision ||
            source->source_addresses != fragment.source_addresses ||
            fragment.step >= source->steps.size() ||
            source->steps[fragment.step].outcome != fragment.outcome)
            reject("session_semantic_parent_revision_changed");
        if (!original_indices.contains(source->episode_id)) {
            original_indices.emplace(source->episode_id, parent_episodes.size());
            parent_episodes.push_back(normalized_episode(*source));
        }
    }
    const auto receipt = session_speech_receipt(interpretation);
    const JsonValue payload = JsonValue::Object{
        {"schema", std::string(session_semantic_input_schema)},
        {"interpretation", receipt}, {"new_observation_count", 0},
        {"independent_evidence_count", 0}, {"grants_authority", false}};
    const auto payload_bytes = semantic_canonical_json(payload);
    const auto digest = semantic_json_digest(payload);
    std::vector<std::string> parents;
    std::vector<std::string> addresses;
    std::set<std::string> address_seen;
    for (const auto& source : parent_episodes) {
        parents.push_back(source.episode_id);
        for (const auto& address : source.source_addresses)
            if (address_seen.insert(address).second) addresses.push_back(address);
    }
    std::vector<std::string> cues{key.first};
    cues.insert(cues.end(), parents.begin(), parents.end());
    for (const auto& unit : interpretation.units) {
        cues.push_back(unit.subject);
        cues.push_back(unit.predicate);
    }
    for (const auto& unit : interpretation.units) {
        if (unit.value_kind == "entity" &&
            std::holds_alternative<std::string>(unit.value.storage()))
            cues.push_back(std::get<std::string>(unit.value.storage()));
    }
    for (const auto& unit : interpretation.units)
        append_speech_entity_cues(unit.value, cues);
    std::vector<std::string> unique_cues;
    std::set<std::string> cue_seen;
    for (const auto& cue : cues) {
        auto normalized = unicode_casefold(collapse_unicode_whitespace(cue));
        if (!text(normalized)) reject("memory episode is incomplete");
        if (cue_seen.insert(normalized).second)
            unique_cues.push_back(std::move(normalized));
    }
    std::vector<std::string> evidence = parents;
    evidence.insert(evidence.end(), addresses.begin(), addresses.end());
    SemanticMemoryStep step{"session_semantic_proposal", payload, parents,
        "Multi-fragment source interpretation, not an independent observation",
        "pending", evidence};
    SemanticSourceEpisode derivative{"session-semantic:" + digest,
        std::move(unique_cues), {std::move(step)}, addresses, digest,
        "derived_semantic_unverified_proposal"};

    std::map<std::string, SemanticAnchor, std::less<>> parts;
    std::vector<SemanticAnchor> source_parts;
    for (const auto& part : interpretation.source.request.parts) {
        parts.emplace(part.anchor.identifier, part.anchor);
        source_parts.push_back(part.anchor);
    }
    std::map<std::string, std::vector<const SessionSourceContext*>, std::less<>> contexts;
    for (const auto& context : interpretation.source.request.source_context)
        for (const auto& identifier : context.anchor_ids)
            contexts[identifier].push_back(&context);
    const std::set<std::string> unresolved_set(
        interpretation.unresolved.begin(), interpretation.unresolved.end());
    std::vector<SessionSemanticUnitBinding> units;
    for (const auto& unit : interpretation.units) {
        const auto refs = references(unit);
        std::vector<SemanticAnchor> anchors;
        std::vector<std::size_t> ordinals;
        std::vector<SessionSourceContext> input_context;
        std::set<const SessionSourceContext*> context_seen;
        std::vector<std::string> unresolved_anchors;
        for (const auto& identifier : refs) {
            const auto found = parts.find(identifier);
            if (found == parts.end()) reject("session semantic anchor unavailable");
            anchors.push_back(found->second);
            const auto ordinal = event_ordinal(found->second);
            if (std::ranges::find(ordinals, ordinal) == ordinals.end()) ordinals.push_back(ordinal);
            if (const auto rows = contexts.find(identifier); rows != contexts.end())
                for (const auto& context : rows->second) {
                    if (context_seen.insert(context).second) input_context.push_back(*context);
                }
            if (unresolved_set.contains(identifier)) unresolved_anchors.push_back(identifier);
        }
        std::vector<const SessionEvent*> events;
        std::vector<const BoundSessionOccurrence*> occurrences;
        for (const auto ordinal : ordinals) {
            const auto& event = entry->document.archive->event(
                ordinal, entry->document.declared_sha256);
            events.push_back(&event);
            if (const auto rows = entry->occurrence_index.by_event.find(ordinal);
                rows != entry->occurrence_index.by_event.end())
                for (const auto index : rows->second)
                    occurrences.push_back(&entry->occurrence_index.occurrences.at(index));
        }
        units.push_back({unit, std::move(anchors), std::move(events),
            std::move(occurrences), std::move(input_context),
            std::move(unresolved_anchors)});
    }
    return std::make_shared<const BoundSessionSemantics>(BoundSessionSemantics{
        std::string(expected_memory_snapshot_id), {key.first, key.second}, receipt,
        std::move(parent_episodes), std::move(derivative), std::move(source_parts),
        std::move(units), interpretation.unresolved, std::move(entry), interpretation,
        payload_bytes, digest});
}

const SessionSemanticUnitBinding& BoundSessionSemantics::unit(
    const std::size_t ordinal,
    const std::string_view expected_memory_snapshot_id) const {
    if (expected_memory_snapshot_id != memory_snapshot_id)
        reject("session_semantic_memory_generation_changed");
    if (ordinal >= units.size()) reject("session_semantic_unit_index_required");
    return units[ordinal];
}

std::shared_ptr<const BoundSessionSemantics> restore_session_semantics(
    const JsonValue& payload,
    const PreparedSessionView& view,
    const std::vector<SemanticSourceEpisode>& source_episodes,
    const std::string_view expected_memory_snapshot_id) {
    try {
        if (view.memory_snapshot_id != expected_memory_snapshot_id)
            reject("session_semantic_memory_generation_changed");
        if (!payload.is_object() ||
            payload.at("schema").as_string() != session_semantic_input_schema)
            reject("session_semantic_bytes_required");
        const auto& receipt = payload.at("interpretation");
        const auto& document_key = receipt.at("source").at("document_key").as_array();
        if (document_key.size() != 2)
            reject("session_speech_receipt_malformed");
        const SessionDocumentKey key{std::string(document_key[0].as_string()),
                                     std::string(document_key[1].as_string())};
        auto entry = view.get(key, expected_memory_snapshot_id);
        if (!entry) reject("session_semantic_document_not_prepared");
        const auto interpretation = restore_session_speech_interpretation(receipt, *entry);
        auto bound = bind_session_semantics(
            view, source_episodes, interpretation, expected_memory_snapshot_id);
        if (bound->payload != semantic_canonical_json(payload))
            reject("session_semantic_input_changed");
        return bound;
    } catch (const std::out_of_range&) {
        reject("session_semantic_input_changed");
    } catch (const std::bad_variant_access&) {
        reject("session_semantic_input_changed");
    }
}

std::shared_ptr<const BoundSessionSemantics> restore_session_semantics_bytes(
    const std::string_view payload_bytes,
    const PreparedSessionView& view,
    const std::vector<SemanticSourceEpisode>& source_episodes,
    const std::string_view expected_memory_snapshot_id) {
    try {
        std::pmr::monotonic_buffer_resource memory;
        const auto parsed = transport::parse_json(payload_bytes, memory, 1000);
        auto payload = detached_json(parsed);
        if (semantic_canonical_json(payload) != payload_bytes)
            reject("session_semantic_input_changed");
        return restore_session_semantics(
            payload, view, source_episodes, expected_memory_snapshot_id);
    } catch (const std::length_error&) {
        reject("session_semantic_input_changed");
    }
}

SessionSemanticGraphDelta::SessionSemanticGraphDelta(
    std::shared_ptr<const TermAddressIndex> address_index_value,
    std::string memory_snapshot_id_value,
    std::string vrs_snapshot_id_value,
    const std::size_t edge_start_value,
    std::shared_ptr<const BoundSessionSemantics> binding_value,
    std::vector<std::string> appended_terms_value,
    std::vector<EventSignalEdge> edge_rows_value,
    std::vector<SessionSemanticEdgeRole> edge_roles_value,
    std::vector<std::vector<SessionSemanticEdgeRole>> unit_graph_addresses_value)
    : address_index(std::move(address_index_value)),
      memory_snapshot_id(std::move(memory_snapshot_id_value)),
      vrs_snapshot_id(std::move(vrs_snapshot_id_value)),
      edge_start(edge_start_value), binding(std::move(binding_value)),
      appended_terms(std::move(appended_terms_value)),
      edge_rows(std::move(edge_rows_value)), edge_roles(std::move(edge_roles_value)),
      unit_graph_addresses(std::move(unit_graph_addresses_value)) {
    if (!address_index || !binding || edge_rows.size() != edge_roles.size() ||
        unit_graph_addresses.size() != binding->units.size())
        reject("session_semantic_graph_parent_required");
}

std::vector<SessionSemanticGraphReceipt> SessionSemanticGraphDelta::receipts() const {
    return {{std::string(session_semantic_graph_schema), binding->interpretation_receipt,
             binding->derivative.episode_id, edge_start, edge_rows.size(), edge_roles}};
}

void SessionSemanticGraphDelta::require_parent(
    const std::shared_ptr<const TermAddressIndex>& expected_address_index,
    const std::string_view expected_memory_snapshot_id,
    const std::string_view expected_vrs_snapshot_id,
    const std::size_t edge_count) const {
    if (expected_address_index.get() != address_index.get() ||
        expected_memory_snapshot_id != memory_snapshot_id ||
        expected_vrs_snapshot_id != vrs_snapshot_id || edge_count != edge_start)
        reject("session_semantic_graph_parent_changed");
}

SessionSemanticGraphDelta prepare_session_semantic_delta(
    std::shared_ptr<const BoundSessionSemantics> bound,
    std::shared_ptr<const TermAddressIndex> address_index,
    const std::size_t edge_count,
    std::string memory_snapshot_id,
    std::string vrs_snapshot_id) {
    if (!bound || !address_index || bound->memory_snapshot_id != memory_snapshot_id ||
        !text(memory_snapshot_id) || !text(vrs_snapshot_id))
        reject("session_semantic_graph_parent_required");
    validate_binding(*bound);

    for (const auto& parent : bound->original_episodes)
        if (!address_index->lookup(parent.episode_id))
            reject("session_semantic_graph_parent_missing");
    if (address_index->lookup(bound->derivative.episode_id))
        reject("session_semantic_derivative_already_in_graph");

    if (address_index->size() > std::numeric_limits<std::uint32_t>::max())
        reject("session semantic node capacity exceeded");
    std::unordered_map<std::string, std::uint32_t> additions;
    std::vector<std::string> appended_terms;
    std::vector<EventSignalEdge> edge_rows;
    std::vector<SessionSemanticEdgeRole> edge_roles;
    std::set<std::tuple<std::string, std::string, std::string>> seen;

    const auto node = [&](const std::string& value) -> std::uint32_t {
        if (const auto existing = address_index->lookup(value)) {
            if (*existing > std::numeric_limits<std::uint32_t>::max())
                reject("session semantic node capacity exceeded");
            return static_cast<std::uint32_t>(*existing);
        }
        if (const auto existing = additions.find(value); existing != additions.end())
            return existing->second;
        if (appended_terms.size() > std::numeric_limits<std::uint32_t>::max() -
                address_index->size())
            reject("session semantic node capacity exceeded");
        const auto result = static_cast<std::uint32_t>(
            address_index->size() + appended_terms.size());
        additions.emplace(value, result);
        appended_terms.push_back(value);
        return result;
    };

    const auto link = [&](const std::string& left, const std::string& right,
                          const std::string& kind) {
        SessionSemanticEdgeRole role{kind, left, right};
        if (seen.emplace(kind, left, right).second) {
            edge_rows.push_back({node(left), node(right), 1, .75F});
            edge_roles.push_back(role);
        }
        return role;
    };

    const auto scope = strings(bound->document_key);
    const auto document = semantic_scoped_address("session-document", scope, scope);
    for (const auto& parent : bound->original_episodes)
        link(parent.episode_id, document, "document_reconstruction_parent");
    const auto derivation = link(document, bound->derivative.episode_id, "source_derivation");

    std::unordered_map<std::string, std::string> anchors;
    anchors.reserve(bound->source_parts.size());
    for (const auto& anchor : bound->source_parts) {
        const auto address = semantic_anchor_address(scope, anchor);
        anchors.emplace(anchor.identifier, address);
        link(address, document, "anchor_document_location");
    }

    std::vector<std::vector<SessionSemanticEdgeRole>> unit_graph_addresses;
    unit_graph_addresses.reserve(bound->units.size());
    for (const auto& row : bound->units) {
        const auto address = semantic_unit_address(scope, row.unit);
        std::vector<SessionSemanticEdgeRole> links{derivation};
        links.push_back(link(bound->derivative.episode_id, address, "encoding_unit"));
        for (const auto& anchor : row.anchors) {
            const auto& anchor_address = anchors.at(anchor.identifier);
            links.push_back(link(address, anchor_address, "unit_anchor_proposal"));
            links.push_back({"anchor_document_location", anchor_address, document});
        }
        unit_graph_addresses.push_back(std::move(links));
    }
    for (const auto& identifier : bound->unresolved)
        link(bound->derivative.episode_id, anchors.at(identifier), "unresolved_anchor");

    return SessionSemanticGraphDelta(
        std::move(address_index), std::move(memory_snapshot_id),
        std::move(vrs_snapshot_id), edge_count, std::move(bound),
        std::move(appended_terms), std::move(edge_rows), std::move(edge_roles),
        std::move(unit_graph_addresses));
}

}  // namespace swegca::world
