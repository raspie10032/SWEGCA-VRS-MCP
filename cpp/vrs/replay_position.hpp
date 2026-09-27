#pragma once
#include "vrs/experience_block.hpp"

namespace swegca::vrs {
// Address of a completed Replay, not a truth judgment or a new observation.
struct ReplayPosition {
    architecture::DigestBytes source{},connection{};
    ExperienceLocation original{};
    std::uint64_t original_index=0;
    bool operator==(const ReplayPosition&) const = default;
};
}
