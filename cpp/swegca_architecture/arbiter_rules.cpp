#include "swegca_architecture/arbiter_rules.hpp"

#include <cmath>
#include <stdexcept>
#include <string>

namespace swegca::architecture {
namespace {

// SWEGCA: src/swegca/mosaic_evidence_accumulator.py@5901a5a:69-95
[[noreturn]] void invalid(const char* policy, const char* field) {
    throw std::invalid_argument(std::string(policy) + "_invalid:" + field);
}

// SWEGCA: src/swegca/mosaic_bounded_world_write.py@5901a5a:42-55
bool unit(double value) { return std::isfinite(value) && value >= 0 && value <= 1; }

}  // namespace

// Keep the original positive finite policy values in binary64. Conversion to
// a state scalar occurs only after the clip comparison establishes it fits.
// SWEGCA: src/swegca/mosaic_synapse_arbiter.py@5901a5a:222-236
kernel::ArbiterRules make_arbiter_rules(const ArbiterPolicy& p) {
    constexpr const char* name = "arbiter_policy";
    if (!(std::isfinite(p.maximum_slot_delta) && p.maximum_slot_delta > 0))
        invalid(name, "maximum_slot_delta");
    if (!(std::isfinite(p.maximum_world_delta) && p.maximum_world_delta > 0))
        invalid(name, "maximum_world_delta");
    if (!unit(p.minimum_weight)) invalid(name, "minimum_weight");
    kernel::ArbiterRules rules;
    rules.maximum_slot_delta_ = p.maximum_slot_delta;
    rules.maximum_world_delta_ = p.maximum_world_delta;
    rules.minimum_weight_ = p.minimum_weight;
    return rules;
}

}  // namespace swegca::architecture
