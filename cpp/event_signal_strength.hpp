#pragma once

#include "event_vrs_inputs.hpp"
#include "vrs_state_update.hpp"

#include <cstdint>
#include <map>
#include <set>
#include <string_view>

namespace swegca::vrs {

struct BoundEventStrengthUpdates {
    std::map<std::uint32_t, float> strengths;
    std::set<std::uint32_t> seed_nodes;
};

// Bind one detached Re-evidence proposal to immutable edges. A null receipt
// contributes no strength change. The proposal is checked, never reapplied.
// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_signal.py@7536139:53-97
[[nodiscard]] BoundEventStrengthUpdates bind_event_strength_updates(
    const ValidatedEventVrsInputs& inputs,
    const VRSStateUpdateReceipt* receipt,
    std::string_view connection_namespace = "vrs-edge:",
    std::string_view strength_storage_dtype = "<f4");

}  // namespace swegca::vrs
