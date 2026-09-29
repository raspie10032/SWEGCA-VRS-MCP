#pragma once

#include "world/semantic_source_context.hpp"
#include "world/semantic_vrs_ingress.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace swegca::world {

inline constexpr std::string_view semantic_input_scope_source_sha256 =
    "ab7b0e1711ce368139dfc009b3706835d5e10a3d5691b9e2f93655e9204c07c9";
inline constexpr std::string_view semantic_document_schema =
    "rozephine-genshin-namuwiki-character-assimilation-v1";

[[nodiscard]] std::map<std::string, std::int64_t, std::less<>>
attributable_text_anchors(
    const SemanticSourceEpisode& source, const SemanticEncoding& encoding,
    const SemanticContextByAnchor* context_by_anchor = nullptr);

[[nodiscard]] std::vector<std::int64_t> interpreted_input_steps(
    const SemanticSourceEpisode& source, const SemanticEncoding& encoding);

}  // namespace swegca::world
