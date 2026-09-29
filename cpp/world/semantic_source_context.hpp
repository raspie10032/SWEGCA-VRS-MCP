#pragma once

#include "world/semantic_vrs_ingress.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace swegca::world {

inline constexpr std::string_view semantic_source_context_source_sha256 =
    "77886dfbb91645c53cfbaf6acca24090e615c68117e9efad5cb202a976dc6d0f";
inline constexpr std::string_view semantic_source_context_role =
    "reported_authored_metadata_not_independently_interpreted";
inline constexpr std::string_view authored_media_observation_schema =
    "rozephine-authored-media-observation-v1";

inline constexpr std::string_view semantic_source_context_limits[] = {
    "language_labels_may_be_wrong",
    "record_identifiers_do_not_identify_speakers",
    "grouping_does_not_prove_causality_or_translation_equivalence",
    "audio_alignment_not_verified_by_text_interpretation",
    "uninterpreted_metadata_may_contain_additional_conditions",
};

struct SemanticSourceContext final {
    std::int64_t step{};
    std::vector<SemanticPathElement> path;
    std::vector<std::string> excluded_fields;
    std::vector<std::string> anchor_ids;
    std::string value_json;

    [[nodiscard]] JsonValue to_json() const;
    friend bool operator==(const SemanticSourceContext&,
                           const SemanticSourceContext&) = default;
};

using SemanticContextByAnchor =
    std::map<std::string, std::vector<SemanticSourceContext>, std::less<>>;

[[nodiscard]] SemanticContextByAnchor input_context_by_anchor(
    const SemanticEncoding& encoding);
[[nodiscard]] std::vector<SemanticSourceContext> authored_source_context(
    const SemanticSourceEpisode& source,
    const std::vector<SemanticAnchor>& anchors);
[[nodiscard]] std::vector<SemanticSourceContext> restore_source_context(
    const JsonValue::Array& records,
    const SemanticSourceEpisode& source,
    const std::vector<SemanticAnchor>& anchors);

}  // namespace swegca::world
