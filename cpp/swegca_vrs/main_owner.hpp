#pragma once


#include "swegca_vrs/authority_roles.hpp"
#include "swegca_vrs/cognitive_state.hpp"

#include <cstddef>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace swegca::vrs {

// Startup input only. Main copies text, tensor and payload bytes through its
// Account during construction; all views and spans must remain valid for that call.
// No field is a capability or an accepted evidence decision.
// Rule: reconstruction board §3A, §4 CognitiveState, §10.1.
struct MainInitialState final {
    struct TensorInput final {
        ScalarType scalar_type;
        TensorShape3 shape;
        std::span<const std::byte> canonical_bytes;
        // When present, Main reads bounded chunks from this borrowed reader.
        // canonical_bytes must then be empty. The reader lives through Main's
        // constructor call; neither input becomes part of CognitiveState.
        const TensorByteReader* reader = nullptr;
    };

    std::string_view owner;
    std::span<const RoleDefinitionInput> roles;
    TensorInput semantic;
    TensorInput executive;
    TensorInput scratch;
    std::span<const WorldEntityInput> entities;
    std::span<const WorldRelationInput> relations;
    std::span<const std::string_view> evidence_references;
    std::span<const std::byte> goals;
    std::span<const std::byte> values;
    std::span<const std::byte> self;
    // Canonical AutonomyControl bytes, or none for the author's absent keys.
    // An explicit step-0 control is allowed. Main decodes and copies them.
    std::optional<std::span<const std::byte>> autonomy;
};

}  // namespace swegca::vrs
