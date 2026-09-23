#include "swegca_architecture/gate_rules.hpp"

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

// SWEGCA: src/swegca/mosaic_bounded_world_write.py@5901a5a:35-55
kernel::GateRules make_gate_rules(const GatePolicy& p) {
    constexpr const char* name = "gate_policy";
    if (!unit(p.minimum_causal_lower_bound)) invalid(name, "minimum_causal_lower_bound");
    if (p.minimum_source_diversity == 0 || p.minimum_context_diversity == 0)
        invalid(name, "diversity_minima");
    kernel::GateRules rules;
    rules.minimum_causal_lower_bound_ = p.minimum_causal_lower_bound;
    rules.minimum_source_diversity_ = p.minimum_source_diversity;
    rules.minimum_context_diversity_ = p.minimum_context_diversity;
    return rules;
}

}  // namespace swegca::architecture
