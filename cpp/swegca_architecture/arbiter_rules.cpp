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

// Limits are bounded by `kernel::max_delta_limit`, so the float conversion is
// exact in range and every arbiter sum stays finite (codex KJ4).
// SWEGCA: src/swegca/mosaic_synapse_arbiter.py@5901a5a:222-236
kernel::ArbiterRules make_arbiter_rules(const ArbiterPolicy& p) {
    constexpr const char* name = "arbiter_policy";
    const auto bounded = [](double value) {
        return std::isfinite(value) && value > 0 && value <= kernel::max_delta_limit;
    };
    if (!bounded(p.maximum_slot_delta)) invalid(name, "maximum_slot_delta");
    if (!bounded(p.maximum_world_delta)) invalid(name, "maximum_world_delta");
    if (!unit(p.minimum_weight)) invalid(name, "minimum_weight");
    kernel::ArbiterRules rules;
    rules.maximum_slot_delta_ = static_cast<float>(p.maximum_slot_delta);
    rules.maximum_world_delta_ = static_cast<float>(p.maximum_world_delta);
    rules.minimum_weight_ = static_cast<float>(p.minimum_weight);
    if (!(std::isfinite(rules.maximum_slot_delta_) && rules.maximum_slot_delta_ > 0 &&
          std::isfinite(rules.maximum_world_delta_) && rules.maximum_world_delta_ > 0 &&
          std::isfinite(rules.minimum_weight_)))
        invalid(name, "float_conversion");
    return rules;
}

}  // namespace swegca::architecture
