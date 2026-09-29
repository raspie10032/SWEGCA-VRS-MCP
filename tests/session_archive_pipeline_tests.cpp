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
        R"({"type":"message","role":"user","content":[{"type":"text","text":"Alpha 요청","text_elements":[{"condition":"quoted only"}]},{"type":"image","url":"retained://opaque"}]})"};
    std::vector<std::string> payload_digests;
    for (const auto& payload : payloads) payload_digests.push_back(sha(payload));
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
    const PreparedSessionView prepared("memory-1", cache, view);
    const auto entry = prepared.get(key, "memory-1");
    assert(entry && entry->status() == "prepared");
    assert(entry->document.archive->events.size() == 3);
    assert(entry->occurrence_index.occurrences.size() == 3);

    const SessionCallKey call_key{"session-1", "turn-1", "function", "call-1"};
    const auto joined = prepared.call_join(call_key, "memory-1");
    assert(joined.status == "linked_by_scoped_call_id");
    assert(joined.calls.size() == 1 && joined.results.size() == 1);

    std::map<std::pair<std::string, std::size_t>, SessionSelectedStep> selected{
        {{"episode-1", 0}, {revision, JsonValue::Object{{"verdict", "available"}},
                            "selected"}}};
    const auto collected = collect_session_results(
        prepared, selected, true, true, true);
    assert(collected.results.size() == 1);
    assert(collected.results.front().meaning->interpretation == "recorded_normal_exit");
    assert(collected.results.front().calls.size() == 1);
    assert(collected.messages.size() == 1);

    auto speech = prepare_session_speech_input(*entry, {2}, "fixture-model");
    const auto& source_context = speech.request.source_context.front().value;
    const auto& block_context = source_context.at("block_context").as_array();
    assert(block_context.size() == 2);
    assert(!block_context.front().at("value").as_object().contains("text"));
    assert(block_context.front().at("value").as_object().contains("text_elements"));
    assert(block_context.back().at("value").as_object().contains("url"));
    assert((source_context.at("unrepresented_block_indices").as_array() ==
            JsonValue::Array{JsonValue(1)}));
    const auto anchor = speech.request.parts.front().anchor.identifier;
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
          JsonValue::Array{JsonValue::Object{{"entity", "reader"}}},
          JsonValue::Array{JsonValue::Object{{"entity", "Alpha"}}}, {anchor}, {}}},
        {anchor});
    const auto& segment_row = segmented.segments->at("segments").as_array().front();
    assert(segment_row.at("addressees").as_array().size() == 1);
    assert(segment_row.at("topics").as_array().size() == 1);
    const auto segmented_receipt = session_speech_receipt(segmented);
    assert(session_speech_receipt(
        restore_session_speech_interpretation(segmented_receipt, *entry)) ==
        segmented_receipt);

    const auto bound = bind_session_semantics(prepared, episodes, entry, interpretation);
    assert(bound->original_episodes.size() == 1);
    assert(bound->derivative.episode_id.starts_with("session-semantic:"));
    assert(bound->units.size() == 1);
    assert(bound->units.front().events.size() == 1);
    assert(bound->units.front().occurrences.size() == 1);
    assert(!bound->grants_authority());
    assert(bound->entry.get() == entry.get());

    const auto restored_bound = restore_session_semantics(
        bound->derivative.steps.front().observation, prepared, episodes);
    assert(restored_bound->derivative.episode_id == bound->derivative.episode_id);
    assert(restored_bound->units.size() == bound->units.size());
    assert(&restored_bound->unit(0, "memory-1") == &restored_bound->units.front());
    const auto encoded = prepare_session_recorded_event(
        bound->derivative, 0, prepared, episodes);
    assert(encoded.semantic_encoding);
    assert(encoded.claims.size() == 1);
    assert(encoded.claims.front().semantic_anchor_resolution.attributable_document_key ==
           bound->document_key);
    assert(encoded.unresolved.empty());
    assert(encoded.semantic_encoding->outcomes() == std::vector<std::string>{"pending"});
    assert(encoded.semantic_encoding->model() == "fixture-model");
}

}  // namespace

int main() {
    test_complete_session_archive_pipeline();
    std::cout << "PASS source-bound session archive C++ pipeline\n";
}
