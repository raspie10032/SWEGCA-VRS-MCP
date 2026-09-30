#pragma once

#include "world/memory_activation.hpp"

#include <cstddef>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace swegca::world {

inline constexpr std::string_view ordinary_source_router_source_sha256 =
    "d34c207493b2c0999b757a88b4a3f8551ae9d4dbcc0ba925581d4d06c1f9899b";

class OrdinarySourceRouter final {
public:
    explicit OrdinarySourceRouter(
        std::vector<std::shared_ptr<const HotMemoryIndex>> sources,
        std::size_t maximum_entries = 16384,
        std::size_t maximum_estimated_bytes = 16U * 1024U * 1024U);

    [[nodiscard]] std::vector<std::shared_ptr<const HotMemoryIndex>>
        known_sources_for(std::string_view episode_id) const;
    [[nodiscard]] const MemoryEpisode& episode(std::string_view episode_id) const;

    [[nodiscard]] std::size_t estimated_bytes() const;
    [[nodiscard]] const std::vector<std::shared_ptr<const HotMemoryIndex>>& sources() const noexcept;

private:
    struct Entry final {
        std::vector<std::shared_ptr<const HotMemoryIndex>> routes;
        std::size_t charge{};
        std::list<std::string>::iterator lru;
    };

    std::vector<std::shared_ptr<const HotMemoryIndex>> sources_;
    std::vector<std::shared_ptr<const HotMemoryIndex>> known_;
    std::vector<std::shared_ptr<const HotMemoryIndex>> unknown_;
    std::size_t maximum_entries_{};
    std::size_t maximum_estimated_bytes_{};
    mutable std::size_t estimated_bytes_{};
    mutable std::mutex mutex_;
    mutable std::list<std::string> order_;
    mutable std::map<std::string, Entry, std::less<>> routes_;
};

}  // namespace swegca::world
