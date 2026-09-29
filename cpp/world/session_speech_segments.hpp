#pragma once

#include "world/session_speech_ingress.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace swegca::world {

inline constexpr std::string_view session_speech_segments_source_sha256 =
    "dd0f5fb65a445b76bc4ca0f4c40c89790d89dbe080dd9afdb3427d0098181784";

struct SessionSegmentAnnotation final {
    std::string anchor;
    std::string quote;
    std::size_t occurrence{};
    std::string kind;
    JsonValue speaker;
    JsonValue::Array addressees;
    JsonValue::Array topics;
    std::vector<std::string> anchors;
    std::vector<SemanticQualifier> qualifiers;
};

[[nodiscard]] SessionSpeechInterpretation from_segment_annotations(
    SessionSpeechInput prepared,
    std::vector<SessionSegmentAnnotation> segments,
    std::vector<std::string> unresolved);

}  // namespace swegca::world
