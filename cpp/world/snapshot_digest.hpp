#pragma once

#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace swegca::world {

struct MemoryEpisode;

inline constexpr std::string_view snapshot_digest_source_sha256 =
    "cf59ddf581b5d98eaca3a28730a9b9984796d319fe9071cce31d7018ffe523d1";

[[nodiscard]] std::string snapshot_digest(
    const std::map<std::string, MemoryEpisode, std::less<>>& episodes,
    const std::map<std::string, std::vector<std::string>, std::less<>>& postings);

}  // namespace swegca::world
