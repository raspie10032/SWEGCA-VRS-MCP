#pragma once

#include "world/cognitive_state.hpp"

#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace swegca::world {

inline constexpr std::string_view proposition_directory_source_sha256 =
    "5e0db492f2309757a4fd66b4124cec6791fe7c67aa173bcbc220dd19fdb1d2c7";

struct PropositionDirectory final {
    std::map<std::string, std::vector<std::string>, std::less<>> by_episode;
    std::map<std::string, std::vector<std::string>, std::less<>> by_proposition;

    [[nodiscard]] static PropositionDirectory from_rows(
        const std::vector<std::pair<std::string, std::vector<JsonValue>>>& rows);
};

}  // namespace swegca::world
