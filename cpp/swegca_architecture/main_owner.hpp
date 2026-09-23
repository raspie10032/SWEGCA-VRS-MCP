#pragma once

#include "swegca_architecture/authority_roles.hpp"
#include "swegca_architecture/cognitive_state.hpp"

#include <cstddef>
#include <span>
#include <vector>

namespace swegca::architecture {

// Startup input only. Main copies tensor bytes through its Account during
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
    StructuredWorldGraph world_graph;
    std::vector<ExperienceAddress> evidence_references;
    GoalState goals;
    ValueState values;
    SelfState self;
};

}  // namespace swegca::architecture
