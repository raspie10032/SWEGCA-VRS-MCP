#pragma once

#include "swegca_architecture/arbiter_kernel.hpp"

#include <cstdint>

// Shell side of the nano-core: validates Main's policy once and produces the
// prevalidated rule values the kernels read. Validation may throw; kernels
// never do. Rules: ARCHITECTURE_SPEC.md@5901a5a §4.6.
namespace swegca::architecture {

struct ArbiterPolicy {
    double maximum_slot_delta = 0.02;
    double maximum_world_delta = 0.5;
    double minimum_weight = 0.5;
};

// Throws `std::invalid_argument("<policy>_invalid:<field>")`.
[[nodiscard]] kernel::ArbiterRules make_arbiter_rules(const ArbiterPolicy& policy);

}  // namespace swegca::architecture
