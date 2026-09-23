#pragma once


#include "swegca_vrs/gate_kernel.hpp"
#include "swegca_vrs/identity_types.hpp"

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

// The identity of a gate policy. The evidence gate binds it into the
// commit capability's operation and the guarded writer recomputes it from
// its own configuration, so a write is authorized only under the one
// configuration the user's writer judges and previews with.
// SWEGCA: src/tinylm_slicer/mosaic_bounded_world_write.py@3bddcb7:379-412
[[nodiscard]] Digest256 gate_policy_digest(const GatePolicy& policy);

}  // namespace swegca::vrs
