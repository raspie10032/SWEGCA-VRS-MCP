#include "swegca_architecture/sha256.hpp"
#include "world/prepared_session_cache.hpp"
#include "world/session_result_collection.hpp"
#include "world/session_content_encoding.hpp"
#include "world/session_semantic_binding.hpp"
#include "world/session_speech_ingress.hpp"
#include "world/session_speech_segments.hpp"

#include <cassert>
#include <cstddef>
#include <iostream>
#include <map>
#include <memory_resource>
#include <string>
#include <utility>
#include <vector>

using namespace swegca::world;

namespace {

std::string hex(const swegca::architecture::DigestBytes& digest) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result(digest.size() * 2, '0');
    for (std::size_t index = 0; index != digest.size(); ++index) {
        const auto value = std::to_integer<unsigned>(digest[index]);
        result[index * 2] = digits[value >> 4U];
        result[index * 2 + 1] = digits[value & 15U];
    }
    return result;
}

std::string sha(const std::string& value) {
    swegca::architecture::Sha256 digest;
    digest.update(value);
    return hex(digest.finish());
}

JsonValue occurrence(const std::string& digest, const std::int64_t line,
                     const std::int64_t offset, std::string kind,
                     JsonValue::Object metadata) {
    return JsonValue::Object{{"content_sha256", digest}, {"file_id", 1},
        {"line", line}, {"offset", offset}, {"bytes", 8},
        {"session", "session-1"}, {"turn", "turn-1"},
        {"kind", std::move(kind)}, {"item_metadata", std::move(metadata)}};
}

SemanticSourceEpisode source_episode(const std::string& document,
                                     const std::string& revision,
                                     const std::vector<std::string>& payload_digests) {
    JsonValue::Array occurrences{
        occurrence(payload_digests[0], 1, 0, "response_item:function_call",
                   {{"call_id", "call-1"}}),
        occurrence(payload_digests[1], 2, 16, "response_item:function_call_output",
                   {{"call_id", "call-1"}}),
        occurrence(payload_digests[2], 3, 32, "response_item:message", {})};
    JsonValue::Object provenance{
        {"character_offset", 0},
        {"duplicate_occurrences_are_not_new_events", true},
        {"semantics_and_actual_world_success_not_verified", true},
        {"occurrences", std::move(occurrences)},
        {"files", JsonValue::Object{{"1", JsonValue::Object{
            {"path", "/fixture/session.jsonl"}, {"sha256", std::string(64, '1')},
            {"byte_boundary", 4096}}}}}};
    JsonValue observation = JsonValue::Object{
        {"schema_version", std::string(authored_media_observation_schema)},
        {"source_type", "codex_session_record"},
        {"content", JsonValue::Object{
            {"source_document", JsonValue::Object{{"identity", "fixture-session"},
                {"sha256", revision}, {"fragments_are_independent_outcomes", false},
                {"license", "fixture"}}},
            {"historical_provenance", std::move(provenance)},
            {"variants", JsonValue::Array{JsonValue::Object{
                {"text", document}, {"source_address", "fixture:document"}}}}}}};
    SemanticMemoryStep step{"observation_attempt_outcome", std::move(observation), {},
        "recorded source", "pending", {"fixture:evidence"}};
    return {"episode-1", {"fixture-session"}, {std::move(step)},
            {"fixture:document"}, revision, "unverified"};
}

void test_complete_session_archive_pipeline() {
    const std::vector<std::string> payloads{
        R"({"type":"function_call","name":"functions.exec_command","arguments":"{\"cmd\":\"true\"}"})",
        R"({"type":"function_call_output","output":{"exit_code":0}})",
        R"({"type":"message","role":"user","big":123456789012345678901234567890,"tiny":1e-400,"content":[{"type":"text","text":"Alpha\n요청","text_elements":[{"condition":"quoted only"}]},{"type":"image","url":"retained://opaque"}]})"};
    std::vector<std::string> payload_digests;
    for (std::size_t index = 0; index != payloads.size(); ++index) {
        auto canonical = payloads[index];
        if (index == 2) canonical.replace(canonical.find("1e-400"), 6, "0.0");
        payload_digests.push_back(sha(canonical));
    }
    std::string document = "{\"historical_records\":[";
    for (std::size_t index = 0; index != payloads.size(); ++index) {
        if (index) document += ',';
        document += "{\"content_sha256\":\"" + payload_digests[index] +
                    "\",\"payload\":" + payloads[index] + "}";
    }
    document += "],\"commands_are_inert\":true,\"claims_unverified\":true}";
    const auto revision = sha(document);
    const std::vector<SemanticSourceEpisode> episodes{
        source_episode(document, revision, payload_digests)};

    const auto [directory, changed] = SessionDocumentDirectory{}.append_with_keys(episodes);
    assert((changed == std::vector<SessionDocumentKey>{{"fixture-session", revision}}));
    const auto view = directory.bind("memory-1");
    const auto key = changed.front();
    const auto cache = prepare_session_cache_document(
        episodes, view, "memory-1", PreparedSessionCache{}, key);
    const auto prepared = cache.bind("memory-1", view);
    const auto rebound = cache.bind("memory-2", directory.bind("memory-2"));
    assert(rebound.get(key, "memory-2"));
    const auto entry = prepared.get(key, "memory-1");
    assert(entry && entry->status() == "prepared");
    assert(entry->document.archive->events.size() == 3);
    assert(entry->occurrence_index.occurrences.size() == 3);

    const SessionCallKey call_key{"session-1", "turn-1", "function", "call-1"};
    const auto joined = prepared.call_join(call_key, "memory-1");
    assert(joined.status == "linked_by_scoped_call_id");
    assert(joined.calls.size() == 1 && joined.results.size() == 1);

    SessionSelectedSteps selected{
        {{"episode-1", 0}, {revision, JsonValue::Object{{"verdict", "available"}},
                            "selected"}}};
    const auto collected = collect_session_results(
        prepared, selected, true, true, true);
    assert(collected.results.size() == 1);
    assert(collected.results.front().meaning->interpretation == "recorded_normal_exit");
    assert(collected.results.front().calls.size() == 1);
    assert(collected.messages.size() == 1);

    auto speech = prepare_session_speech_input(*entry, {2}, "fixture-model");
    std::pmr::monotonic_buffer_resource context_memory;
    const auto source_context = swegca::transport::parse_json(
        speech.request.source_context.front().value_json, context_memory, 1000);
    const auto* block_context = source_context.find("block_context");
    const auto* original_fields = source_context.find("original_message_fields");
    assert(original_fields && original_fields->find("big") && original_fields->find("tiny"));
    assert(original_fields->find("big")->scalar == "123456789012345678901234567890");
    assert(original_fields->find("tiny")->scalar == "0.0");
    assert(block_context && block_context->kind == swegca::transport::Json::Kind::array);
    assert(block_context->values.size() == 2);
    const auto* first_value = block_context->values.front().find("value");
    const auto* last_value = block_context->values.back().find("value");
    assert(first_value && first_value->find("text") == nullptr);
    assert(first_value->find("text_elements") != nullptr);
    assert(last_value && last_value->find("url") != nullptr);
    const auto* occurrence_context = source_context.find("occurrences");
    assert(occurrence_context &&
           occurrence_context->kind == swegca::transport::Json::Kind::array &&
           occurrence_context->values.size() == 1);
    const auto& occurrence_row = occurrence_context->values.front();
    const auto* source_position = occurrence_row.find("source_position");
    const auto* source_claim = occurrence_row.find("source_claim");
    const auto* metadata = occurrence_row.find("metadata");
    assert(source_position &&
           source_position->kind == swegca::transport::Json::Kind::array &&
           source_position->values.size() == 6);
    assert(source_claim && source_claim->kind == swegca::transport::Json::Kind::array);
    assert(metadata && metadata->find("content_sha256") && metadata->find("item_metadata"));
    const auto* unrepresented = source_context.find("unrepresented_block_indices");
    assert(unrepresented && unrepresented->kind == swegca::transport::Json::Kind::array &&
           unrepresented->values.size() == 1 && unrepresented->values.front().scalar == "1");
    const auto anchor = speech.request.parts.front().anchor.identifier;
    auto raw_input = prepare_session_speech_input(*entry, {2}, "fixture-model");
    const auto raw_response = std::string{"{\"units\":[{\"subject\":\"subject\","}
        + "\"predicate\":\"said\",\"value\":\"Alpha\",\"value_kind\":\"literal\","
          "\"polarity\":\"affirmed\",\"basis\":\"reported\",\"anchors\":[\"" +
        anchor + "\"],\"qualifiers\":[]}],\"unresolved\":[]}";
    const auto raw_interpretation = interpret_session_speech_response(
        std::move(raw_input), raw_response);
    assert(raw_interpretation.units.size() == 1);
    bool extra_proposal_field_rejected = false;
    try {
        auto extra_input = prepare_session_speech_input(*entry, {2}, "fixture-model");
        (void)interpret_session_speech_response(
            std::move(extra_input),
            raw_response.substr(0, raw_response.size() - 1) + ",\"summary\":\"x\"}");
    } catch (const std::invalid_argument& error) {
        extra_proposal_field_rejected =
            std::string_view(error.what()) == "semantic_units_required_not_summary";
    }
    assert(extra_proposal_field_rejected);
    bool oversized_integer_rejected = false;
    try {
        auto oversized_input = prepare_session_speech_input(*entry, {2}, "fixture-model");
        auto oversized_response = raw_response;
        const auto value = oversized_response.find("\"Alpha\"");
        oversized_response.replace(value, 7, std::string(4301, '9'));
        (void)interpret_session_speech_response(
            std::move(oversized_input), oversized_response);
    } catch (const std::invalid_argument& error) {
        oversized_integer_rejected =
            std::string_view(error.what()) == "invalid_json_object";
    }
    assert(oversized_integer_rejected);
    auto interpretation = interpret_session_speech_annotations(
        std::move(speech), {{anchor, "request", JsonValue(nullptr), {}, {}, {anchor}, {}}}, {});
    assert(interpretation.units.size() == 1);
    const auto receipt = session_speech_receipt(interpretation);
    assert(receipt.at("schema").as_string() == session_speech_interpretation_schema);
    const auto restored_interpretation =
        restore_session_speech_interpretation(receipt, *entry);
    assert(session_speech_receipt(restored_interpretation) == receipt);

    auto segmented_input = prepare_session_speech_input(*entry, {2}, "fixture-model");
    const auto segmented = from_segment_annotations(std::move(segmented_input),
        {{anchor, "Alpha", 0, "request", JsonValue(nullptr),
          JsonValue::Array{JsonValue::Object{{"entity", "reader"},
                                             {"anchors", JsonValue::Array{anchor}}}},
          JsonValue::Array{JsonValue::Object{{"entity", "Alpha"},
                                             {"anchors", JsonValue::Array{anchor}}}},
          {anchor}, {{"condition", "reported", {anchor}}}}},
        {anchor});
    const auto& segment_row = segmented.segments->at("segments").as_array().front();
    assert(segment_row.at("addressees").as_array().size() == 1);
    assert(segment_row.at("topics").as_array().size() == 1);
    assert(segment_row.at("qualifiers").as_array().size() == 1);
    assert(segmented.units.front().qualifiers.size() == 1);
    const auto segmented_receipt = session_speech_receipt(segmented);
    assert(session_speech_receipt(
        restore_session_speech_interpretation(segmented_receipt, *entry)) ==
        segmented_receipt);

    const auto bound = bind_session_semantics(
        prepared, episodes, interpretation, "memory-1");
    assert(bound->original_episodes.size() == 1);
    assert(bound->derivative.episode_id.starts_with("session-semantic:"));
    assert(bound->units.size() == 1);
    assert(bound->units.front().events.size() == 1);
    assert(bound->units.front().occurrences.size() == 1);
    assert(!bound->grants_authority());
    assert(bound->entry.get() == entry.get());
    assert(bound->interpretation.receipt() == receipt);
    assert(bound->payload == semantic_canonical_json(
        bound->derivative.steps.front().observation));
    assert(bound->sha256 == bound->derivative.revision);
    bool stale_generation_rejected = false;
    try {
        (void)bind_session_semantics(prepared, episodes, interpretation, "memory-2");
    } catch (const std::invalid_argument& error) {
        stale_generation_rejected =
            std::string_view(error.what()) == "session_semantic_memory_generation_changed";
    }
    assert(stale_generation_rejected);

    const auto restored_bound = restore_session_semantics(
        bound->derivative.steps.front().observation, prepared, episodes, "memory-1");
    assert(restored_bound->derivative.episode_id == bound->derivative.episode_id);
    assert(restored_bound->units.size() == bound->units.size());
    assert(&restored_bound->unit(0, "memory-1") == &restored_bound->units.front());
    const auto restored_bytes = restore_session_semantics_bytes(
        bound->payload, prepared, episodes, "memory-1");
    assert(restored_bytes->sha256 == bound->sha256);
    bool noncanonical_bytes_rejected = false;
    try {
        (void)restore_session_semantics_bytes(
            bound->payload + " ", prepared, episodes, "memory-1");
    } catch (const std::invalid_argument& error) {
        noncanonical_bytes_rejected =
            std::string_view(error.what()) == "session_semantic_input_changed";
    }
    assert(noncanonical_bytes_rejected);
    const auto encoded = prepare_session_recorded_event(
        bound->derivative, 0, prepared, episodes);
    assert(encoded.semantic_encoding);
    assert(encoded.claims.size() == 1);
    assert(encoded.claims.front().semantic_anchor_resolution.attributable_document_key ==
           bound->document_key);
    assert(encoded.unresolved.empty());
    assert(encoded.semantic_encoding->outcomes() == std::vector<std::string>{"pending"});
    assert(encoded.semantic_encoding->model() == "fixture-model");
    const auto unavailable = prepare_session_recorded_event(
        bound->derivative, 0, static_cast<const PreparedSessionView*>(nullptr), episodes);
    assert(!unavailable.semantic_encoding && unavailable.claims.empty());
    assert(unavailable.unresolved ==
           std::vector<std::string>{"semantic_source_binding_unresolved"});
}

}  // namespace

int main() {
    test_complete_session_archive_pipeline();
    std::cout << "PASS source-bound session archive C++ pipeline\n";
}
