#pragma once

#include "swegca_architecture/evidence_kernel.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

// VRS nano-kernel: the target gate predicate. Its output is a
// write-authorization failure mask, not a verdict, so it belongs to the
// synapse (VRS), not the core verifier (codex 16:55); it reads the core's
// EvidenceStatus.
//
// Kernel rules (user@2026-09-23 nano-core directive; design board nano-core
// boundary): no allocation, lock, exception, I/O, string, virtual call or
// global mutable state; fixed-width inputs and outputs; every kernel checks
// its own inputs and fails closed, so no authority result depends on a
// comment about what the shell validated (codex KJ1-KJ3).
//
// Arithmetic contract (codex KJ7). Bit-identical results are claimed only
// when all of these hold, and are not yet verified on any GPU/NPU:
//   - IEEE-754 binary32 (arbiter) and binary64 (evidence), round to nearest
//     even, subnormals preserved (no flush-to-zero / denormals-are-zero);
//   - no floating-point contraction or reassociation (-ffp-contract=off,
//     no -ffast-math);
//   - only +, -, *, /, sqrt, fabs, fmin, fmax, all exactly specified by
//     IEEE-754; no other libm call inside a kernel;
//   - reductions run in the sequential order written here.
// The Wilson z value comes from the shell once per rule set (a libm call),
// so every executor must receive the same rule bits rather than recompute z.
//
//
// Rules: ARCHITECTURE_SPEC.md@5901a5a §4.5 (gate, Bind).
namespace swegca::architecture {

struct GatePolicy;
namespace kernel {
class GateRules;
}  // namespace kernel
[[nodiscard]] kernel::GateRules make_gate_rules(const GatePolicy& policy);

namespace kernel {

// Boolean gate conditions, one bit each, so the item and batch paths share
// one representation. `condition_regime_change_suspected` set means FAIL.
enum GateCondition : std::uint16_t {
    condition_evidence_current = 1u << 0,
    condition_accumulator_revision_current = 1u << 1,
    condition_runtime_context_safe = 1u << 2,
    condition_definitions_complete = 1u << 3,
    condition_counterfactual_support = 1u << 4,
    condition_intervention_support = 1u << 5,
    condition_regime_change_suspected = 1u << 6,
    condition_slot_gate = 1u << 7,
    condition_device_gate = 1u << 8,
    condition_capacity_safe = 1u << 9,
};

// Everything the target predicate reads. `decision_binding` is the digest
// Main computed for the decision; `proposal_binding` is the same function
// over the proposal (Main-owned EvidenceDecision shell).
struct GateInput {
    EvidenceStatus status = EvidenceStatus::abstain;
    double causal_lower_bound = 0;
    std::uint32_t source_diversity = 0;
    std::uint32_t context_diversity = 0;
    std::uint16_t conditions = 0;
    std::uint64_t address_count = 0;
    Digest decision_binding{};
    Digest proposal_binding{};
};

// One bit per failed condition, so a receipt names every failure at once.
enum GateFailure : std::uint32_t {
    gate_evidence_not_accepted = 1u << 0,
    gate_evidence_expired = 1u << 1,
    gate_accumulator_revision_stale = 1u << 2,
    gate_runtime_context_unsafe = 1u << 3,
    gate_causal_lower_bound = 1u << 4,
    gate_source_diversity = 1u << 5,
    gate_context_diversity = 1u << 6,
    gate_definitions_incomplete = 1u << 7,
    gate_counterfactual_support = 1u << 8,
    gate_intervention_support = 1u << 9,
    gate_regime_change_suspected = 1u << 10,
    gate_slot = 1u << 11,
    gate_device = 1u << 12,
    gate_capacity_strategy = 1u << 13,
    gate_addresses_empty = 1u << 14,   // E001
    gate_binding_mismatch = 1u << 15,  // E002
    gate_rules_invalid = 1u << 16,
    gate_input_invalid = 1u << 17,
};

// Validated gate thresholds; only `make_gate_rules` constructs one.
class GateRules final {
private:
    GateRules() = default;
    friend GateRules swegca::architecture::make_gate_rules(const GatePolicy&);
    friend std::uint32_t authorize_target(const GateRules& rules, const GateInput& in) noexcept;

    double minimum_causal_lower_bound_ = 0;
    std::uint32_t minimum_source_diversity_ = 0;
    std::uint32_t minimum_context_diversity_ = 0;
};

// Authorize_target = Authorize_impl and Nonempty(addresses) and Bind(...).
// Returns 0 exactly when authorized; invalid rules or input never return 0.
// An all-zero decision binding never binds.
// Rule: ARCHITECTURE_SPEC.md@5901a5a:154-162 (E001/E002 closed here).
// SWEGCA: src/swegca/mosaic_bounded_world_write.py@5901a5a:286-314
[[nodiscard]] inline std::uint32_t authorize_target(const GateRules& rules,
                                                    const GateInput& in) noexcept {
    std::uint32_t failed = 0;
    const auto fail_if = [&failed](bool condition, std::uint32_t bit) noexcept {
        failed |= condition ? bit : 0u;
    };
    fail_if(!finite_unit(rules.minimum_causal_lower_bound_) ||
                rules.minimum_source_diversity_ == 0 || rules.minimum_context_diversity_ == 0,
            gate_rules_invalid);
    fail_if(!finite_unit(in.causal_lower_bound) || (in.conditions & ~std::uint16_t{0x3ff}) != 0,
            gate_input_invalid);
    const auto has = [&in](std::uint16_t bit) noexcept { return (in.conditions & bit) != 0; };
    fail_if(in.status != EvidenceStatus::accept, gate_evidence_not_accepted);
    fail_if(!has(condition_evidence_current), gate_evidence_expired);
    fail_if(!has(condition_accumulator_revision_current), gate_accumulator_revision_stale);
    fail_if(!has(condition_runtime_context_safe), gate_runtime_context_unsafe);
    fail_if(!(in.causal_lower_bound >= rules.minimum_causal_lower_bound_),
            gate_causal_lower_bound);
    fail_if(in.source_diversity < rules.minimum_source_diversity_, gate_source_diversity);
    fail_if(in.context_diversity < rules.minimum_context_diversity_, gate_context_diversity);
    fail_if(!has(condition_definitions_complete), gate_definitions_incomplete);
    fail_if(!has(condition_counterfactual_support), gate_counterfactual_support);
    fail_if(!has(condition_intervention_support), gate_intervention_support);
    fail_if(has(condition_regime_change_suspected), gate_regime_change_suspected);
    fail_if(!has(condition_slot_gate), gate_slot);
    fail_if(!has(condition_device_gate), gate_device);
    fail_if(!has(condition_capacity_safe), gate_capacity_strategy);
    fail_if(in.address_count == 0, gate_addresses_empty);
    std::byte any{0};
    std::byte differ{0};
    for (std::size_t at = 0; at < in.decision_binding.size(); ++at) {
        any |= in.decision_binding[at];
        differ |= in.decision_binding[at] ^ in.proposal_binding[at];
    }
    fail_if(any == std::byte{0} || differ != std::byte{0}, gate_binding_mismatch);
    return failed;
}

// Structure-of-arrays gate batch (codex KJ5).
struct GateColumns {
    std::size_t count = 0;
    std::span<const EvidenceStatus> status;
    std::span<const double> causal_lower_bound;
    std::span<const std::uint32_t> source_diversity;
    std::span<const std::uint32_t> context_diversity;
    std::span<const std::uint16_t> conditions;
    std::span<const std::uint64_t> address_count;
    std::span<const Digest> decision_binding;
    std::span<const Digest> proposal_binding;
};

// Judges items [first, last); returns false (and writes nothing) when a column
// does not fit.
// SWEGCA: src/swegca/mosaic_bounded_world_write.py@5901a5a:286-314
[[nodiscard]] inline bool authorize_target_batch(const GateRules& rules, const GateColumns& in,
                                                 std::span<std::uint32_t> failures,
                                                 std::size_t first, std::size_t last) noexcept {
    const std::size_t n = in.count;
    if (first > last || last > n || in.status.size() != n || in.causal_lower_bound.size() != n ||
        in.source_diversity.size() != n || in.context_diversity.size() != n ||
        in.conditions.size() != n || in.address_count.size() != n ||
        in.decision_binding.size() != n || in.proposal_binding.size() != n ||
        failures.size() != n)
        return false;
    for (std::size_t item = first; item < last; ++item)
        failures[item] = authorize_target(
            rules, GateInput{in.status[item], in.causal_lower_bound[item],
                             in.source_diversity[item], in.context_diversity[item],
                             in.conditions[item], in.address_count[item],
                             in.decision_binding[item], in.proposal_binding[item]});
    return true;
}

}  // namespace kernel
}  // namespace swegca::architecture
