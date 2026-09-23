#pragma once

#include "swegca_vrs/core_imports.hpp"

#include "swegca_vrs/gate_kernel.hpp"

#include <cstdint>

// Shell side of the nano-core: validates Main's policy once and produces the
// prevalidated rule values the kernels read. Validation may throw; kernels
// never do. Rules: ARCHITECTURE_SPEC.md@5901a5a §4.5.
namespace swegca::vrs {

struct GatePolicy {
    double minimum_causal_lower_bound = 0.55;
    std::uint32_t minimum_source_diversity = 2;
    std::uint32_t minimum_context_diversity = 4;
};

// Throws `std::invalid_argument("<policy>_invalid:<field>")`.
[[nodiscard]] kernel::GateRules make_gate_rules(const GatePolicy& policy);

}  // namespace swegca::vrs
