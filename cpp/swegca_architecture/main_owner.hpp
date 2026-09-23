#pragma once

#include "swegca_architecture/authority_roles.hpp"
#include "swegca_architecture/cognitive_state.hpp"

#include <cstddef>
#include <span>
#include <vector>

namespace swegca::architecture {

// Startup input only. Main copies tensor and payload bytes through its Account during
// construction; input spans must remain valid for that constructor call.
// No field is a capability or an accepted evidence decision.
// Rule: reconstruction board §3A, §4 CognitiveState, §10.1.
struct MainInitialState final {
    struct TensorInput final {
        ScalarType scalar_type;
        TensorShape3 shape;
        std::span<const std::byte> canonical_bytes;
    };

    OwnerId owner;
    RoleRegistry roles;
    TensorInput semantic;
    TensorInput executive;
    TensorInput scratch;
    std::span<const WorldEntityInput> entities;
    std::span<const WorldRelationInput> relations;
    std::vector<ExperienceAddress> evidence_references;
    std::span<const std::byte> goals;
    std::span<const std::byte> values;
    std::span<const std::byte> self;
};

}  // namespace swegca::architecture
