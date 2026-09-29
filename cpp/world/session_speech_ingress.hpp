#pragma once

#include "world/prepared_session_cache.hpp"
#include "world/semantic_vrs_ingress.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace swegca::world {

inline constexpr std::string_view session_speech_ingress_source_sha256 =
    "da22579fa2f713d753291dd07499c173f7afde0523dfbf0a6d56b55e1abceffb";
inline constexpr std::string_view session_speech_interpretation_schema =
    "rozephine-session-speech-interpretation-v1";

struct SessionDeliveredPart final {
    SemanticAnchor anchor;
    std::string content;
    std::string media_type;
};

struct SessionSourceContext final {
    std::int64_t step{};
    std::vector<SemanticPathElement> path;
    std::vector<SemanticPathElement> excluded_fields;
    std::vector<std::string> anchor_ids;
    std::string value_json;
};

struct SessionSpeechRequest final {
    std::string source_id;
    std::string source_revision;
    std::string model;
    std::vector<SessionDeliveredPart> parts;
    std::string prompt;
    std::vector<SessionSourceContext> source_context;
};

struct SessionParentFragment final {
    std::string episode_id;
    std::string revision;
    std::size_t step{};
    std::size_t variant{};
    std::size_t character_offset{};
    std::vector<std::string> source_addresses;
    std::string outcome;
    friend bool operator==(const SessionParentFragment&,
                           const SessionParentFragment&) = default;
};

struct SessionSpeechInput final {
    SessionDocumentKey document_key;
    std::vector<std::size_t> event_ordinals;
    std::vector<SessionParentFragment> parent_fragments;
    SessionSpeechRequest request;

    [[nodiscard]] JsonValue binding() const;
};

struct SessionSpeechInterpretation final {
    SessionSpeechInput source;
    std::string model;
    std::vector<SemanticMeaningUnit> units;
    std::vector<std::string> unresolved;
    std::optional<JsonValue> annotations;
    std::optional<JsonValue> segments;

    [[nodiscard]] constexpr bool whole_document_understood() const noexcept { return false; }
    [[nodiscard]] constexpr bool semantic_authority() const noexcept { return false; }
    [[nodiscard]] constexpr bool persistent_write_authority() const noexcept { return false; }
    [[nodiscard]] constexpr bool native_admission_complete() const noexcept { return false; }
    [[nodiscard]] JsonValue receipt() const;
};

struct SessionSpeechAnnotation final {
    std::string anchor;
    std::string kind;
    JsonValue speaker;
    JsonValue::Array addressees;
    JsonValue::Array topics;
    std::vector<std::string> anchors;
    std::vector<SemanticQualifier> qualifiers;
};

[[nodiscard]] SessionSpeechInput prepare_session_speech_input(
    const PreparedSessionEntry& entry,
    const std::vector<std::size_t>& event_ordinals,
    std::string model);

// Accepts an already parsed offline proposal. It performs no model call, retry,
// native commit, or source search.
[[nodiscard]] SessionSpeechInterpretation interpret_session_speech(
    SessionSpeechInput prepared,
    std::vector<SemanticMeaningUnit> units,
    std::vector<std::string> unresolved);

// Validate one raw producer response before any typed fields can be dropped.
// The producer remains outside the core; this boundary performs no model call.
[[nodiscard]] SessionSpeechInterpretation interpret_session_speech_response(
    SessionSpeechInput prepared, std::string_view response_json);

[[nodiscard]] SessionSpeechInterpretation interpret_session_speech_annotations(
    SessionSpeechInput prepared,
    std::vector<SessionSpeechAnnotation> annotations,
    std::vector<std::string> unresolved);

[[nodiscard]] SessionSpeechInterpretation interpret_session_speech_annotations_response(
    SessionSpeechInput prepared, std::string_view response_json);

[[nodiscard]] JsonValue session_speech_receipt(
    const SessionSpeechInterpretation& interpretation);

// Rebind a portable receipt to the current prepared source. This performs no
// model call and rejects any source, interpretation, or construction drift.
[[nodiscard]] SessionSpeechInterpretation restore_session_speech_interpretation(
    const JsonValue& record, const PreparedSessionEntry& entry);

}  // namespace swegca::world
