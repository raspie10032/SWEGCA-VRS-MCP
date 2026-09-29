#pragma once

#include "world/cognitive_state.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <string_view>

namespace swegca::world {

inline constexpr std::string_view stage_timing_source_sha256 =
    "ea385ac024377d4f3b9f211f2b570b7f8395151d6004f57f2eabfd0d1bd18190";

class StageTiming final {
public:
    using Clock = std::function<std::uint64_t()>;

    explicit StageTiming(std::string scope, Clock clock = {});
    void checkpoint(std::string name);
    [[nodiscard]] JsonValue receipt() const;

private:
    std::string scope_;
    Clock clock_;
    std::uint64_t started_{};
    std::uint64_t last_{};
    std::map<std::string, std::uint64_t, std::less<>> stages_;
};

}  // namespace swegca::world
