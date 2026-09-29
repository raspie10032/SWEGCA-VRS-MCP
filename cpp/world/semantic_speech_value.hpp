#pragma once

#include "world/cognitive_state.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace swegca::world {

inline constexpr std::string_view semantic_speech_value_source_sha256 =
    "ee7f1de86045cf28e8e012dd93d377b883397fa607f08dffa0c8ce4de39453c5";
inline constexpr std::string_view semantic_speech_value_schema_id =
    "rozephine-speech-value-v1";

struct SpeechReferent final {
    std::string entity;
    std::vector<std::string> anchors;
    friend bool operator==(const SpeechReferent&, const SpeechReferent&) = default;
};

struct SpeechValue final {
    std::string kind;
    std::optional<SpeechReferent> speaker;
    std::vector<SpeechReferent> addressees;
    std::vector<SpeechReferent> topics;
    std::string content;
    std::vector<std::string> content_anchors;

    [[nodiscard]] std::vector<SpeechReferent> referents() const;
    [[nodiscard]] bool addresses_entity(std::string_view identifier) const;
    [[nodiscard]] std::tuple<std::string, std::optional<std::string>,
        std::vector<std::string>, std::vector<std::string>, std::string>
        comparison_key() const;
};

[[nodiscard]] SpeechValue parse_speech_value(
    const JsonValue& value, const std::vector<std::string>& unit_anchors);
[[nodiscard]] JsonValue speech_value_schema(const JsonValue& references);

}  // namespace swegca::world
