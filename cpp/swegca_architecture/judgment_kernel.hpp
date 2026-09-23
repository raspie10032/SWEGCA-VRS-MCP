#pragma once

#include "swegca_architecture/digest_bytes.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <type_traits>

// SWEGCA nano-core: pure judgment kernels for the evidence decision, the
// target gate predicate and the Single-World arbiter.
// Kernel rules (user@2026-09-23 nano-core directive; design board nano-core
// boundary): no allocation, lock, exception, I/O, string, virtual call or
// global mutable state; fixed-width inputs and outputs; every kernel checks
// its own inputs and fails closed, so no authority result depends on a
// comment about what the shell validated (codex KJ1-KJ3).
//
// Arithmetic contract (codex KJ7). Bit-identical results are claimed only
// when all of these hold, and are not yet verified on any GPU/NPU:
//   - IEEE-754 binary32 or binary64 (arbiter), binary64 (evidence), round to nearest
//     even, subnormals preserved (no flush-to-zero / denormals-are-zero);
//   - no floating-point contraction or reassociation (-ffp-contract=off,
//     no -ffast-math);
//   - only +, -, *, /, sqrt, fabs, fmin, fmax, all exactly specified by
//     IEEE-754; no other libm call inside a kernel;
//   - reductions run in the sequential order written here.
// The Wilson z value comes from the shell once per rule set (a libm call),
// so every executor must receive the same rule bits rather than recompute z.
//
// Rules: ARCHITECTURE_SPEC.md@5901a5a §4.4 (decision), §4.5 (gate, Bind),
// §4.6 (arbitration).
namespace swegca::architecture {

struct EvidencePolicy;
struct GatePolicy;
struct ArbiterPolicy;
namespace kernel {
class EvidenceRules;
class GateRules;
class ArbiterRules;
}  // namespace kernel
[[nodiscard]] kernel::EvidenceRules make_evidence_rules(const EvidencePolicy& policy);
[[nodiscard]] kernel::GateRules make_gate_rules(const GatePolicy& policy);
[[nodiscard]] kernel::ArbiterRules make_arbiter_rules(const ArbiterPolicy& policy);

namespace kernel {

// The shared digest byte type; its header is exception- and allocation-free (codex KJ6).
using Digest = DigestBytes;

inline constexpr std::size_t max_axes = 8;
inline constexpr double max_delta_limit = 1.0e6;  // keeps every sum finite

enum class EvidenceStatus : std::uint8_t { abstain = 0, accept = 1, reject = 2 };

enum class EvidenceReason : std::uint8_t {
    minimum_effective_samples = 1,
    source_diversity = 2,
    axis_source_diversity = 3,
    context_diversity = 4,
    regime_change_suspected = 5,
    causal_lower_bound = 6,
    upper_bound_below_threshold = 7,
    uncertain = 8,
    invalid_input = 9,
};

// Fixed-size evidence tally of one claim, maintained by the Main-owned shell.
struct EvidenceTally {
    std::array<double, max_axes> axis_support{};
    std::array<double, max_axes> axis_refute{};
    std::array<std::uint32_t, max_axes> axis_source_diversity{};
    std::uint32_t source_diversity = 0;
    std::uint32_t context_diversity = 0;
    std::uint32_t recent_count = 0;
    double recent_sum = 0;
    std::uint64_t revision = 0;
};

struct EvidenceJudgment {
    EvidenceStatus status = EvidenceStatus::abstain;
    EvidenceReason reason = EvidenceReason::invalid_input;
    double posterior_mean = 0;
    double causal_lower_bound = 0;
    double overall_upper_bound = 0;
    double effective_sample_size = 0;
    double regime_change_score = 0;
    std::uint32_t source_diversity = 0;
    std::uint32_t context_diversity = 0;
    std::uint64_t revision = 0;
};

// Structure-of-arrays view of `count` claims. Axis columns are axis-major:
// axis_support[axis * count + item].
struct EvidenceColumns {
    std::size_t count = 0;
    std::span<const double> axis_support;
    std::span<const double> axis_refute;
    std::span<const std::uint32_t> axis_source_diversity;
    std::span<const std::uint32_t> source_diversity;
    std::span<const std::uint32_t> context_diversity;
    std::span<const std::uint32_t> recent_count;
    std::span<const double> recent_sum;
    std::span<const std::uint64_t> revision;
};

struct EvidenceJudgmentColumns {
    std::span<EvidenceStatus> status;
    std::span<EvidenceReason> reason;
    std::span<double> posterior_mean;
    std::span<double> causal_lower_bound;
    std::span<double> overall_upper_bound;
    std::span<double> effective_sample_size;
    std::span<double> regime_change_score;
};

// Validated decision rules. Only `make_evidence_rules` constructs one; the
// fields are private so a valid value cannot be edited into an invalid one.
class EvidenceRules final {
private:
    EvidenceRules() = default;
    friend EvidenceRules swegca::architecture::make_evidence_rules(const EvidencePolicy&);
    friend bool rules_valid(const EvidenceRules& rules) noexcept;
    friend EvidenceJudgment judge_evidence(const EvidenceRules& rules,
                                           const EvidenceTally& tally) noexcept;
    friend bool judge_evidence_batch(const EvidenceRules& rules, const EvidenceColumns& in,
                                     const EvidenceJudgmentColumns& out, std::size_t first,
                                     std::size_t last) noexcept;

    double z_ = 0;
    double z_squared_ = 0;
    double threshold_ = 0;  // chance rate + accept margin
    double prior_alpha_ = 0;
    double prior_beta_ = 0;
    double minimum_effective_samples_per_axis_ = 0;
    std::uint32_t minimum_source_diversity_ = 0;
    std::uint32_t minimum_axis_source_diversity_ = 0;
    std::uint32_t minimum_context_diversity_ = 0;
    std::uint32_t minimum_recent_samples_ = 0;
    std::uint32_t recent_window_ = 0;
    double regime_change_threshold_ = 0;
    std::uint32_t axis_count_ = 0;
};

// SWEGCA: src/swegca/mosaic_evidence_accumulator.py@5901a5a:69-95
[[nodiscard]] inline bool finite_unit(double value) noexcept {
    return std::isfinite(value) && value >= 0 && value <= 1;
}

// SWEGCA: src/swegca/mosaic_evidence_accumulator.py@5901a5a:69-95
[[nodiscard]] inline bool rules_valid(const EvidenceRules& r) noexcept {
    return std::isfinite(r.z_) && r.z_ > 0 && r.z_squared_ == r.z_ * r.z_ &&
           finite_unit(r.threshold_) && std::isfinite(r.prior_alpha_) && r.prior_alpha_ > 0 &&
           std::isfinite(r.prior_beta_) && r.prior_beta_ > 0 &&
           std::isfinite(r.minimum_effective_samples_per_axis_) &&
           r.minimum_effective_samples_per_axis_ > 0 && r.minimum_source_diversity_ > 0 &&
           r.minimum_axis_source_diversity_ > 0 && r.minimum_context_diversity_ > 0 &&
           r.minimum_recent_samples_ > 0 && r.recent_window_ >= r.minimum_recent_samples_ &&
           finite_unit(r.regime_change_threshold_) && r.axis_count_ > 0 &&
           r.axis_count_ <= max_axes;
}

// SWEGCA: src/swegca/mosaic_evidence_accumulator.py@5901a5a:98-131
[[nodiscard]] inline bool finite_count(double value) noexcept {
    return std::isfinite(value) && value >= 0;
}

struct Interval {
    double lower = 0;
    double upper = 1;
};

// Wilson score interval; no samples means the whole unit interval.
// SWEGCA: src/swegca/mosaic_evidence_accumulator.py@5901a5a:261-283
[[nodiscard]] inline Interval wilson_interval(double supports, double refutes, double z,
                                              double z_squared) noexcept {
    const double samples = supports + refutes;
    if (!(samples > 0)) return {0.0, 1.0};
    const double rate = supports / samples;
    const double denominator = 1 + z_squared / samples;
    const double center = (rate + z_squared / (2 * samples)) / denominator;
    const double radius =
        z * std::sqrt(rate * (1 - rate) / samples + z_squared / (4 * samples * samples)) /
        denominator;
    return {std::fmax(0.0, center - radius), std::fmin(1.0, center + radius)};
}

// Invalid rules or tally abstain with `invalid_input`. Otherwise: abstain on
// too few samples, diversity or a measured regime change; accept only when
// the weakest axis lower bound clears the threshold; reject only when the
// pooled upper bound stays at or below it; abstain otherwise. A regime change
// is judged only once `minimum_recent_samples` outcomes exist, so an
// unmeasured window never counts as a change, even at threshold 0.
// SWEGCA: src/swegca/mosaic_evidence_accumulator.py@5901a5a:285-357
[[nodiscard]] inline EvidenceJudgment judge_evidence(const EvidenceRules& r,
                                                     const EvidenceTally& t) noexcept {
    EvidenceJudgment out;
    out.source_diversity = t.source_diversity;
    out.context_diversity = t.context_diversity;
    out.revision = t.revision;
    if (!rules_valid(r) || t.revision == 0 || t.recent_count > r.recent_window_ ||
        !finite_count(t.recent_sum) || t.recent_sum > t.recent_count)
        return out;  // abstain, invalid_input
    for (std::size_t axis = 0; axis < r.axis_count_; ++axis)
        if (!finite_count(t.axis_support[axis]) || !finite_count(t.axis_refute[axis]))
            return out;

    double supports = 0;
    double refutes = 0;
    double causal_lower = 1;
    bool short_axis = false;
    bool narrow_axis = false;
    for (std::size_t axis = 0; axis < r.axis_count_; ++axis) {
        const double support = t.axis_support[axis];
        const double refute = t.axis_refute[axis];
        supports += support;
        refutes += refute;
        causal_lower =
            std::fmin(causal_lower, wilson_interval(support, refute, r.z_, r.z_squared_).lower);
        short_axis |= support + refute < r.minimum_effective_samples_per_axis_;
        narrow_axis |= t.axis_source_diversity[axis] < r.minimum_axis_source_diversity_;
    }
    const double samples = supports + refutes;
    if (!std::isfinite(samples)) return out;
    out.posterior_mean =
        (supports + r.prior_alpha_) / (samples + r.prior_alpha_ + r.prior_beta_);
    out.causal_lower_bound = causal_lower;
    out.overall_upper_bound = wilson_interval(supports, refutes, r.z_, r.z_squared_).upper;
    out.effective_sample_size = samples;
    const bool measured = t.recent_count >= r.minimum_recent_samples_;
    out.regime_change_score =
        measured ? std::fabs(t.recent_sum / t.recent_count - out.posterior_mean) : 0.0;

    using S = EvidenceStatus;
    using R = EvidenceReason;
    if (short_axis) {
        out.status = S::abstain, out.reason = R::minimum_effective_samples;
    } else if (t.source_diversity < r.minimum_source_diversity_) {
        out.status = S::abstain, out.reason = R::source_diversity;
    } else if (narrow_axis) {
        out.status = S::abstain, out.reason = R::axis_source_diversity;
    } else if (t.context_diversity < r.minimum_context_diversity_) {
        out.status = S::abstain, out.reason = R::context_diversity;
    } else if (measured && out.regime_change_score >= r.regime_change_threshold_) {
        out.status = S::abstain, out.reason = R::regime_change_suspected;
    } else if (out.causal_lower_bound > r.threshold_) {
        out.status = S::accept, out.reason = R::causal_lower_bound;
    } else if (out.overall_upper_bound <= r.threshold_) {
        out.status = S::reject, out.reason = R::upper_bound_below_threshold;
    } else {
        out.status = S::abstain, out.reason = R::uncertain;
    }
    return out;
}

// Judges items [first, last) of a batch; disjoint ranges may run on different
// workers. Returns false (and writes nothing) when a column does not fit.
// SWEGCA: src/swegca/mosaic_evidence_accumulator.py@5901a5a:285-357
[[nodiscard]] inline bool judge_evidence_batch(const EvidenceRules& rules,
                                               const EvidenceColumns& in,
                                               const EvidenceJudgmentColumns& out,
                                               std::size_t first, std::size_t last) noexcept {
    const std::size_t n = in.count;
    const std::size_t axis_count = rules.axis_count_;
    if (axis_count == 0 || axis_count > max_axes || first > last || last > n) return false;
    if (n > SIZE_MAX / max_axes) return false;
    if (in.axis_support.size() != axis_count * n || in.axis_refute.size() != axis_count * n ||
        in.axis_source_diversity.size() != axis_count * n || in.source_diversity.size() != n ||
        in.context_diversity.size() != n || in.recent_count.size() != n ||
        in.recent_sum.size() != n || in.revision.size() != n)
        return false;
    if (out.status.size() != n || out.reason.size() != n || out.posterior_mean.size() != n ||
        out.causal_lower_bound.size() != n || out.overall_upper_bound.size() != n ||
        out.effective_sample_size.size() != n || out.regime_change_score.size() != n)
        return false;
    for (std::size_t item = first; item < last; ++item) {
        EvidenceTally tally;
        for (std::size_t axis = 0; axis < axis_count; ++axis) {
            tally.axis_support[axis] = in.axis_support[axis * n + item];
            tally.axis_refute[axis] = in.axis_refute[axis * n + item];
            tally.axis_source_diversity[axis] = in.axis_source_diversity[axis * n + item];
        }
        tally.source_diversity = in.source_diversity[item];
        tally.context_diversity = in.context_diversity[item];
        tally.recent_count = in.recent_count[item];
        tally.recent_sum = in.recent_sum[item];
        tally.revision = in.revision[item];
        const auto judged = judge_evidence(rules, tally);
        out.status[item] = judged.status;
        out.reason[item] = judged.reason;
        out.posterior_mean[item] = judged.posterior_mean;
        out.causal_lower_bound[item] = judged.causal_lower_bound;
        out.overall_upper_bound[item] = judged.overall_upper_bound;
        out.effective_sample_size[item] = judged.effective_sample_size;
        out.regime_change_score[item] = judged.regime_change_score;
    }
    return true;
}

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

template <class T>
struct ProposalScoresOf {
    static_assert(std::is_same_v<T, float> || std::is_same_v<T, double>);
    T confidence = 0;
    T contradiction = 0;
    T uncertainty = 0;
    std::uint32_t source = 0;  // dense nonzero source id assigned by the shell
};
using ProposalScores = ProposalScoresOf<float>;

struct ArbiterShape {
    std::size_t proposals = 0;
    std::size_t slots = 0;
    std::size_t width = 0;
};

// Caller-owned buffers; the kernel allocates nothing. Layouts are row-major:
// masks[p][s], deltas[p][s][w], bounded[p][s][w], delta_out[s][w].
template <class T>
struct ArbiterBuffersOf {
    static_assert(std::is_same_v<T, float> || std::is_same_v<T, double>);
    std::span<const ProposalScoresOf<T>> scores;
    std::span<const std::uint8_t> masks;
    std::span<const T> deltas;
    std::span<T> weights;
    std::span<std::uint8_t> accepted;
    std::span<T> bounded;  // workspace
    std::span<std::uint8_t> conflict;
    std::span<T> delta_out;
};
using ArbiterBuffers = ArbiterBuffersOf<float>;

class ArbiterRules;
// SWEGCA: src/swegca/mosaic_synapse_arbiter.py@5901a5a:263-321
template <class T>
[[nodiscard]] bool arbitrate(const ArbiterRules& rules, const ArbiterShape& shape,
                             const ArbiterBuffersOf<T>& buffers) noexcept;

// Validated arbiter limits; only `make_arbiter_rules` constructs one.
class ArbiterRules final {
private:
    ArbiterRules() = default;
    friend ArbiterRules swegca::architecture::make_arbiter_rules(const ArbiterPolicy&);
    template <class T>
    friend bool arbitrate(const ArbiterRules& rules, const ArbiterShape& shape,
                          const ArbiterBuffersOf<T>& buffers) noexcept;

    float maximum_slot_delta_ = 0;
    float maximum_world_delta_ = 0;
    float minimum_weight_ = 0;
    double maximum_slot_delta_exact_ = 0;
    double maximum_world_delta_exact_ = 0;
    double minimum_weight_exact_ = 0;
};

// Clip by the Euclidean norm without constructing largest * sqrt(sum), which
// may overflow even when every input is finite. Input and output may alias.
// SWEGCA: src/swegca/mosaic_synapse_arbiter.py@5901a5a:274-286
template <class T>
inline void clip_norm(std::span<const T> values, std::span<T> output, T limit) noexcept {
    static_assert(std::is_same_v<T, float> || std::is_same_v<T, double>);
    T largest = 0;
    for (const T value : values) largest = std::fmax(largest, std::fabs(value));
    if (largest == 0) {
        for (std::size_t at = 0; at < values.size(); ++at) output[at] = values[at];
        return;
    }
    T sum = 0;
    for (const T value : values) {
        const T scaled = value / largest;
        sum += scaled * scaled;
    }
    const T root = std::sqrt(sum);
    // The source clamps each norm denominator to 1e-12 before clipping.
    const T denominator_floor = static_cast<T>(1.0e-12);
    if (largest < denominator_floor / root) {
        const T scale = std::fmin(T{1}, limit / denominator_floor);
        for (std::size_t at = 0; at < values.size(); ++at) output[at] = values[at] * scale;
        return;
    }
    if (largest <= limit / root) {
        for (std::size_t at = 0; at < values.size(); ++at) output[at] = values[at];
        return;
    }
    const T clipped_magnitude = limit / root;
    for (std::size_t at = 0; at < values.size(); ++at)
        output[at] = (values[at] / largest) * clipped_magnitude;
}

// Preflight before any write (codex KJ2): shape nonzero, buffers exact, rules
// finite and bounded, scores finite in [0, 1], sources nonzero, masks 0/1,
// every delta finite.
// SWEGCA: src/swegca/mosaic_synapse_arbiter.py@5901a5a:238-262
template <class T>
[[nodiscard]] inline bool arbiter_input_valid(const ArbiterShape& shape,
                                              const ArbiterBuffersOf<T>& b) noexcept {
    const std::size_t p = shape.proposals;
    const std::size_t s = shape.slots;
    const std::size_t w = shape.width;
    if (p == 0 || s == 0 || w == 0) return false;
    if (p > SIZE_MAX / s || p * s > SIZE_MAX / w || s > SIZE_MAX / w) return false;
    if (b.scores.size() != p || b.masks.size() != p * s || b.deltas.size() != p * s * w ||
        b.weights.size() != p || b.accepted.size() != p || b.bounded.size() != p * s * w ||
        b.conflict.size() != s || b.delta_out.size() != s * w)
        return false;
    for (const auto& score : b.scores)
        if (!finite_unit(score.confidence) || !finite_unit(score.contradiction) ||
            !finite_unit(score.uncertainty) || score.source == 0)
            return false;
    for (const auto mask : b.masks)
        if (mask > 1) return false;
    for (const T delta : b.deltas)
        if (!std::isfinite(delta)) return false;
    return true;
}

// Weight w_i = confidence * (1 - contradiction) * (1 - uncertainty); a
// proposal joins aggregation when w_i >= minimum. Deltas are masked and
// clipped per slot; accepted proposals with different sources and a negative
// per-slot inner product mark that slot unresolved, and its aggregate delta
// is zeroed before the whole-world norm bound. Bounded limits and unit
// weights keep every sum finite. Returns false and writes nothing on any
// invalid input.
// SWEGCA: src/swegca/mosaic_synapse_arbiter.py@5901a5a:263-321
template <class T>
[[nodiscard]] inline bool arbitrate(const ArbiterRules& rules, const ArbiterShape& shape,
                                    const ArbiterBuffersOf<T>& b) noexcept {
    static_assert(std::is_same_v<T, float> || std::is_same_v<T, double>);
    const T maximum_slot_delta = std::is_same_v<T, float>
                                     ? static_cast<T>(rules.maximum_slot_delta_)
                                     : static_cast<T>(rules.maximum_slot_delta_exact_);
    const T maximum_world_delta = std::is_same_v<T, float>
                                      ? static_cast<T>(rules.maximum_world_delta_)
                                      : static_cast<T>(rules.maximum_world_delta_exact_);
    const T minimum_weight = std::is_same_v<T, float>
                                 ? static_cast<T>(rules.minimum_weight_)
                                 : static_cast<T>(rules.minimum_weight_exact_);
    if (!(std::isfinite(maximum_slot_delta) && maximum_slot_delta > 0 &&
          maximum_slot_delta <= max_delta_limit && std::isfinite(maximum_world_delta) &&
          maximum_world_delta > 0 && maximum_world_delta <= max_delta_limit &&
          finite_unit(minimum_weight)))
        return false;
    if (!arbiter_input_valid(shape, b)) return false;
    const std::size_t P = shape.proposals;
    const std::size_t S = shape.slots;
    const std::size_t W = shape.width;

    for (std::size_t p = 0; p < P; ++p) {
        const auto& score = b.scores[p];
        const T weight = score.confidence * (T{1} - score.contradiction) *
                         (T{1} - score.uncertainty);
        b.weights[p] = weight;
        b.accepted[p] = weight >= minimum_weight ? 1 : 0;
        for (std::size_t s = 0; s < S; ++s) {
            const std::size_t row = (p * S + s) * W;
            if (b.masks[p * S + s] == 0) {
                for (std::size_t w = 0; w < W; ++w) b.bounded[row + w] = T{0};
                continue;
            }
            clip_norm<T>(b.deltas.subspan(row, W), b.bounded.subspan(row, W),
                         maximum_slot_delta);
        }
    }

    for (std::size_t s = 0; s < S; ++s) b.conflict[s] = 0;
    for (std::size_t left = 0; left < P; ++left) {
        if (!b.accepted[left]) continue;
        for (std::size_t right = left + 1; right < P; ++right) {
            if (!b.accepted[right] || b.scores[left].source == b.scores[right].source) continue;
            for (std::size_t s = 0; s < S; ++s) {
                if (!b.masks[left * S + s] || !b.masks[right * S + s]) continue;
                T product = 0;
                T absolute_sum = 0;
                bool underflowed_product = false;
                for (std::size_t w = 0; w < W; ++w) {
                    const T a = b.bounded[(left * S + s) * W + w];
                    const T c = b.bounded[(right * S + s) * W + w];
                    const T term = a * c;
                    underflowed_product |= a != 0 && c != 0 && term == 0;
                    product += term;
                    absolute_sum += std::fabs(term);
                }
                // A near-zero sum has an uncertain exact sign after rounded
                // products and additions. Treat it as unresolved instead of
                // silently accepting a possibly negative directional product.
                const T operations = static_cast<T>(W) + T{1};
                const T relative = operations * std::numeric_limits<T>::epsilon();
                const T error = relative >= T{1}
                                    ? std::numeric_limits<T>::infinity()
                                    : (relative / (T{1} - relative)) * absolute_sum +
                                          operations * std::numeric_limits<T>::denorm_min();
                if (underflowed_product || (absolute_sum > 0 && product <= error))
                    b.conflict[s] = 1;
            }
        }
    }

    for (std::size_t s = 0; s < S; ++s) {
        T weight_sum = 0;
        for (std::size_t p = 0; p < P; ++p)
            weight_sum += b.accepted[p] && b.masks[p * S + s] ? b.weights[p] : T{0};
        for (std::size_t w = 0; w < W; ++w) {
            T sum = 0;
            for (std::size_t p = 0; p < P; ++p)
                if (b.accepted[p] && b.masks[p * S + s])
                    sum += b.bounded[(p * S + s) * W + w] * b.weights[p];
            b.delta_out[s * W + w] = b.conflict[s]
                                         ? T{0}
                                         : sum / std::fmax(weight_sum, static_cast<T>(1.0e-12));
        }
    }
    clip_norm<T>(std::span<const T>(b.delta_out), b.delta_out, maximum_world_delta);
    return true;
}

}  // namespace kernel
}  // namespace swegca::architecture
