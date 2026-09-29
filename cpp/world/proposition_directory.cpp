#include "world/proposition_directory.hpp"

#include <algorithm>
#include <stdexcept>

namespace swegca::world {

PropositionDirectory PropositionDirectory::from_rows(
    const std::vector<std::pair<std::string, std::vector<JsonValue>>>& rows) {
    PropositionDirectory result;
    for (const auto& [identifier, observations] : rows) {
        std::vector<std::string> keys;
        for (const auto& observation : observations) {
            if (!observation.is_object()) continue;
            const auto found = observation.as_object().find("proposition_id");
            if (found == observation.as_object().end() ||
                !std::holds_alternative<std::string>(found->second.storage())) continue;
            const auto key = std::string(found->second.as_string());
            if (!key.empty() && std::ranges::find(keys, key) == keys.end())
                keys.push_back(key);
        }
        if (keys.empty()) continue;
        if (!result.by_episode.emplace(identifier, keys).second)
            throw std::invalid_argument("duplicate proposition source address");
        for (const auto& key : keys) result.by_proposition[key].push_back(identifier);
    }
    return result;
}

}  // namespace swegca::world
