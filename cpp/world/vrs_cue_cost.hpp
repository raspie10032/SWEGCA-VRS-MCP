#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace swegca::world {

inline constexpr std::string_view vrs_cue_cost_source_sha256 =
    "dbaa521baa675be1386360530c7bf450efd57676a60c6e48a82bce69313b7d8c";

using CuePostingDirectory =
    std::map<std::string, std::vector<std::uint32_t>, std::less<>>;
using CueNormalizer = std::function<std::string(std::string_view)>;
using PostingLookup = std::function<std::vector<std::uint32_t>(
    std::string_view, const std::vector<std::uint32_t>&)>;

struct CueCostMetrics final {
    std::map<std::string, std::uint64_t, std::less<>> counts;
    std::map<std::string, std::uint64_t, std::less<>> times_ns;
};

[[nodiscard]] std::set<std::uint32_t> profiled_nodes(
    const std::vector<std::string>& cues,
    const CuePostingDirectory& directory,
    const CueNormalizer& normalize,
    const PostingLookup& posting_lookup,
    CueCostMetrics& metrics);

}  // namespace swegca::world
