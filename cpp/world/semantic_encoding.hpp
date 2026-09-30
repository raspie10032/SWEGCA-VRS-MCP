#pragma once

#include "world/memory_activation.hpp"
#include "world/semantic_source_context.hpp"
#include "world/semantic_vrs_ingress.hpp"

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace swegca::world {

inline constexpr std::string_view semantic_encoding_source_sha256 =
    "70d10d453d17257cb41d98614172e46a7d79f416329c32ed54db43bd8e0cf7b6";
inline constexpr std::size_t semantic_response_max_bytes = 1'048'576;

extern const std::string semantic_encoding_prompt;

using SemanticDeliveredContent =
    std::variant<std::string, std::vector<std::byte>>;

struct SemanticDeliveredPart final {
    SemanticAnchor anchor;
    SemanticDeliveredContent content;
    std::string mime_type;
    friend bool operator==(const SemanticDeliveredPart&, const SemanticDeliveredPart&) = default;
};

struct SemanticEncodingInput final {
    std::string source_id;
    std::string source_revision;
    std::string model;
    std::vector<SemanticDeliveredPart> parts;
    std::string prompt{semantic_encoding_prompt};
    std::vector<SemanticSourceContext> source_context;
};

using SemanticProducerResult = std::variant<JsonValue, std::string>;
using SemanticProducer =
    std::function<SemanticProducerResult(const SemanticEncodingInput&)>;

[[nodiscard]] JsonValue parse_semantic_response(const SemanticProducerResult& body);
[[nodiscard]] std::vector<SemanticMeaningUnit> complete_semantic_unit_prefix(
    std::string_view text, const std::vector<SemanticAnchor>& anchors);
[[nodiscard]] MemoryEpisode semantic_encoding_episode(
    const SemanticEncoding& encoding);
[[nodiscard]] SemanticEncoding encode_semantic_offline(
    const SemanticSourceEpisode& source,
    std::vector<SemanticDeliveredPart> parts,
    std::string model, const SemanticProducer& producer);
[[nodiscard]] SemanticEncoding restore_semantic_encoding(
    const JsonValue& record, const SemanticSourceEpisode& source);

}  // namespace swegca::world
