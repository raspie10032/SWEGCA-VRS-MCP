#pragma once

#include "swegca_architecture/digest_bytes.hpp"
#include "swegca_architecture/native_tensor.hpp"

#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
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

struct ScoreValue final {
    ScalarType scalar_type = ScalarType::float32;
    double value = 0;
};

template <class T>
struct ProposalScoresOf {
    static_assert(std::is_same_v<T, float> || std::is_same_v<T, double>);
    ScoreValue confidence;
    ScoreValue contradiction;
    ScoreValue uncertainty;
    ScalarType delta_scalar_type = ScalarType::float32;
    std::uint32_t source = 0;  // dense nonzero source id assigned by the shell
};
using ProposalScores = ProposalScoresOf<float>;

struct ArbiterShape {
    std::size_t proposals = 0;
    std::size_t slots = 0;
    std::size_t width = 0;
};

// Streaming arbitration keeps only one slot's P clip parameters. `work_*`
// buffers stage all results so a failed accessor never changes result spans.
// Final and work spans must be distinct; the kernel checks their byte ranges.
template <class T, class R = T>
struct ArbiterViewBuffersOf {
    static_assert(std::is_same_v<T, float> || std::is_same_v<T, double>);
    static_assert(std::is_same_v<R, float> || std::is_same_v<R, double>);
    std::span<double> weights;
    std::span<std::uint8_t> accepted;
    std::span<std::uint8_t> conflict;
    std::span<R> delta_out;
    std::span<double> work_weights;
    std::span<std::uint8_t> work_accepted;
    std::span<std::uint8_t> work_conflict;
    std::span<R> work_delta_out;
    std::span<std::uint8_t> slot_mask;
    std::span<T> slot_denominator;
    std::span<T> slot_scale;
    ScalarType scalar_type;
    ScalarType result_scalar_type;
};

class ArbiterRules;
// SWEGCA: src/swegca/mosaic_synapse_arbiter.py@5901a5a:263-321
template <class T, class R, class MaskAt, class DeltaAt>
[[nodiscard]] bool arbitrate_view(const ArbiterRules& rules, const ArbiterShape& shape,
                                  std::span<const ProposalScoresOf<T>> scores,
                                  const MaskAt& mask_at, const DeltaAt& delta_at,
                                  const ArbiterViewBuffersOf<T, R>& buffers) noexcept;

// Validated arbiter limits; only `make_arbiter_rules` constructs one.
class ArbiterRules final {
private:
    ArbiterRules() = default;
    friend ArbiterRules swegca::architecture::make_arbiter_rules(const ArbiterPolicy&);
    template <class T, class R, class MaskAt, class DeltaAt>
    friend bool arbitrate_view(const ArbiterRules& rules, const ArbiterShape& shape,
                               std::span<const ProposalScoresOf<T>> scores,
                               const MaskAt& mask_at, const DeltaAt& delta_at,
                               const ArbiterViewBuffersOf<T, R>& buffers) noexcept;

    double maximum_slot_delta_ = 0;
    double maximum_world_delta_ = 0;
    double minimum_weight_ = 0;
};

// Clip by the Euclidean norm without constructing largest * sqrt(sum), which
// may overflow even when every input is finite. Input and output may alias.
// SWEGCA: src/swegca/mosaic_synapse_arbiter.py@5901a5a:274-286
template <class T>
[[nodiscard]] inline bool clip_norm(std::span<const T> values, std::span<T> output,
                                    double limit) noexcept {
    static_assert(std::is_same_v<T, float> || std::is_same_v<T, double>);
    T largest = 0;
    for (const T value : values) largest = std::fmax(largest, std::fabs(value));
    if (largest == 0) {
        for (std::size_t at = 0; at < values.size(); ++at) output[at] = values[at];
        return true;
    }
    T sum = 0;
    for (const T value : values) {
        const T scaled = value / largest;
        sum += scaled * scaled;
    }
    if (!std::isfinite(sum)) return false;
    const T root = std::sqrt(sum);
    if (!std::isfinite(root)) return false;
    // The source clamps each norm denominator to 1e-12 before clipping.
    const T denominator_floor = static_cast<T>(1.0e-12);
    if (static_cast<double>(largest) < static_cast<double>(denominator_floor) / root) {
        const T scale = limit >= denominator_floor
                            ? T{1}
                            : static_cast<T>(limit / denominator_floor);
        for (std::size_t at = 0; at < values.size(); ++at) {
            output[at] = values[at] * scale;
            if (!std::isfinite(output[at])) return false;
        }
        return true;
    }
    if (static_cast<double>(largest) <= limit / root) {
        for (std::size_t at = 0; at < values.size(); ++at) output[at] = values[at];
        return true;
    }
    const T clipped_magnitude = static_cast<T>(limit / root);
    if (!std::isfinite(clipped_magnitude)) return false;
    for (std::size_t at = 0; at < values.size(); ++at) {
        output[at] = (values[at] / largest) * clipped_magnitude;
        if (!std::isfinite(output[at])) return false;
    }
    return true;
}

// Return whether two caller-owned spans occupy any common byte. The total
// pointer order avoids subtraction between unrelated allocations.
// SWEGCA: src/swegca/mosaic_synapse_arbiter.py@5901a5a:263-321
[[nodiscard]] inline bool arbiter_ranges_overlap(std::span<const std::byte> left,
                                                 std::span<const std::byte> right) noexcept {
    const std::less<const std::byte*> less;
    return less(left.data(), right.data() + right.size()) &&
           less(right.data(), left.data() + left.size());
}

// The low-precision pointwise product materializes in its tensor dtype before
// reduction. Ties round to even; signed zero, infinity, and NaN remain values
// for the source's final `< 0` comparison.
// SWEGCA: src/swegca/mosaic_synapse_arbiter.py@5901a5a:301-310
[[nodiscard]] inline std::uint32_t arbiter_round_shift_even(std::uint32_t value,
                                                             unsigned shift) noexcept {
    const std::uint32_t kept = value >> shift;
    const std::uint32_t remainder = value & ((std::uint32_t{1} << shift) - 1);
    const std::uint32_t halfway = std::uint32_t{1} << (shift - 1);
    return kept + (remainder > halfway || (remainder == halfway && (kept & 1u)));
}

// SWEGCA: src/swegca/mosaic_synapse_arbiter.py@5901a5a:301-310
[[nodiscard]] inline std::uint16_t arbiter_half_bits(float value) noexcept {
    const std::uint32_t bits = std::bit_cast<std::uint32_t>(value);
    const std::uint16_t sign = static_cast<std::uint16_t>((bits >> 16) & 0x8000u);
    const std::uint32_t encoded_exponent = (bits >> 23) & 0xffu;
    const std::uint32_t fraction = bits & 0x7fffffu;
    if (encoded_exponent == 0xffu)
        return static_cast<std::uint16_t>(sign | 0x7c00u |
                                          (fraction == 0 ? 0u : 0x0200u));
    if (encoded_exponent == 0) return sign;
    const int power = static_cast<int>(encoded_exponent) - 127;
    if (power > 15) return static_cast<std::uint16_t>(sign | 0x7c00u);
    if (power < -25) return sign;
    if (power < -14) {
        const std::uint32_t rounded =
            arbiter_round_shift_even(0x800000u | fraction, unsigned(-power - 1));
        return static_cast<std::uint16_t>(sign | rounded);
    }
    std::uint32_t rounded = arbiter_round_shift_even(fraction, 13);
    std::uint32_t half_exponent = static_cast<std::uint32_t>(power + 15);
    if (rounded == 0x400u) {
        rounded = 0;
        ++half_exponent;
    }
    if (half_exponent >= 31) return static_cast<std::uint16_t>(sign | 0x7c00u);
    return static_cast<std::uint16_t>(sign | (half_exponent << 10) | rounded);
}

// SWEGCA: src/swegca/mosaic_synapse_arbiter.py@5901a5a:301-310
[[nodiscard]] inline float arbiter_widen_half(std::uint16_t bits) noexcept {
    const std::uint32_t sign = std::uint32_t(bits & 0x8000u) << 16;
    const std::uint32_t encoded_exponent = (bits >> 10) & 0x1fu;
    std::uint32_t fraction = bits & 0x3ffu;
    if (encoded_exponent == 31)
        return std::bit_cast<float>(sign | 0x7f800000u | (fraction << 13));
    if (encoded_exponent == 0 && fraction == 0) return std::bit_cast<float>(sign);
    if (encoded_exponent == 0) {
        int power = -14;
        while ((fraction & 0x400u) == 0) {
            fraction <<= 1;
            --power;
        }
        return std::bit_cast<float>(sign | (std::uint32_t(power + 127) << 23) |
                                    ((fraction & 0x3ffu) << 13));
    }
    return std::bit_cast<float>(sign | ((encoded_exponent + 112u) << 23) |
                                (fraction << 13));
}

// SWEGCA: src/swegca/mosaic_synapse_arbiter.py@5901a5a:301-310
[[nodiscard]] inline float arbiter_round_storage(float value,
                                                 ScalarType type) noexcept {
    if (type == ScalarType::float16) return arbiter_widen_half(arbiter_half_bits(value));
    if (type == ScalarType::bfloat16) {
        const std::uint32_t bits = std::bit_cast<std::uint32_t>(value);
        const std::uint32_t upper = bits >> 16;
        const std::uint32_t lower = bits & 0xffffu;
        if ((bits & 0x7f800000u) == 0x7f800000u && (bits & 0x007fffffu) != 0)
            return std::bit_cast<float>((upper | 0x0040u) << 16);
        const std::uint32_t rounded =
            upper + (lower > 0x8000u || (lower == 0x8000u && (upper & 1u)));
        return std::bit_cast<float>(rounded << 16);
    }
    return value;
}

// PyTorch's floating result_type: Half + BFloat16 promotes to Float;
// otherwise the wider floating type wins. A Python numeric scalar is wrapped
// below a dimensional tensor, so it is converted to the tensor's dtype.
// SWEGCA: src/swegca/mosaic_synapse_arbiter.py@5901a5a:263-286
[[nodiscard]] inline ScalarType arbiter_promote(ScalarType left,
                                                ScalarType right) noexcept {
    if (left == right) return left;
    if (left == ScalarType::float64 || right == ScalarType::float64)
        return ScalarType::float64;
    if (left == ScalarType::float32 || right == ScalarType::float32)
        return ScalarType::float32;
    return ScalarType::float32;  // Half + BFloat16
}

// SWEGCA: src/swegca/mosaic_synapse_arbiter.py@5901a5a:263-286
[[nodiscard]] inline bool arbiter_score_type(ScalarType type) noexcept {
    return type == ScalarType::float16 || type == ScalarType::bfloat16 ||
           type == ScalarType::float32 || type == ScalarType::float64;
}

// Round an IEEE binary64 value directly into a 16-bit tensor format. Routing
// a policy double through binary32 first can double-round a halfway value.
// SWEGCA: src/swegca/mosaic_synapse_arbiter.py@5901a5a:222-236
[[nodiscard]] inline std::uint64_t arbiter_round_shift_even64(
    std::uint64_t value, unsigned shift) noexcept {
    const std::uint64_t kept = value >> shift;
    const std::uint64_t remainder = value & ((std::uint64_t{1} << shift) - 1);
    const std::uint64_t halfway = std::uint64_t{1} << (shift - 1);
    return kept + (remainder > halfway || (remainder == halfway && (kept & 1u)));
}

// SWEGCA: src/swegca/mosaic_synapse_arbiter.py@5901a5a:222-236
[[nodiscard]] inline double arbiter_round_binary16(double value,
                                                    ScalarType type) noexcept {
    const std::uint64_t bits = std::bit_cast<std::uint64_t>(value);
    const std::uint16_t sign = static_cast<std::uint16_t>((bits >> 48) & 0x8000u);
    const std::uint64_t exponent = (bits >> 52) & 0x7ffu;
    const std::uint64_t fraction = bits & 0x000fffffffffffffull;
    if (exponent == 0x7ffu) return value;
    if (exponent == 0) return type == ScalarType::float16
        ? static_cast<double>(arbiter_widen_half(sign))
        : static_cast<double>(std::bit_cast<float>(std::uint32_t(sign) << 16));
    const int power = static_cast<int>(exponent) - 1023;
    const std::uint64_t significand = (std::uint64_t{1} << 52) | fraction;
    if (type == ScalarType::float16) {
        if (power > 15) return sign == 0
            ? std::numeric_limits<double>::infinity()
            : -std::numeric_limits<double>::infinity();
        if (power < -25) return static_cast<double>(arbiter_widen_half(sign));
        std::uint16_t encoded = sign;
        if (power < -14) {
            encoded |= static_cast<std::uint16_t>(
                arbiter_round_shift_even64(significand, unsigned(28 - power)));
        } else {
            std::uint64_t rounded = arbiter_round_shift_even64(significand, 42);
            int encoded_exponent = power + 15;
            if (rounded == 2048) rounded = 1024, ++encoded_exponent;
            encoded |= static_cast<std::uint16_t>(
                (encoded_exponent << 10) | (rounded & 0x3ffu));
        }
        return static_cast<double>(arbiter_widen_half(encoded));
    }
    if (power > 127) return sign == 0
        ? std::numeric_limits<double>::infinity()
        : -std::numeric_limits<double>::infinity();
    if (power < -134)
        return static_cast<double>(std::bit_cast<float>(std::uint32_t(sign) << 16));
    std::uint16_t encoded = sign;
    if (power < -126) {
        encoded |= static_cast<std::uint16_t>(
            arbiter_round_shift_even64(significand, unsigned(-81 - power)));
    } else {
        std::uint64_t rounded = arbiter_round_shift_even64(significand, 45);
        int encoded_exponent = power + 127;
        if (rounded == 256) rounded = 128, ++encoded_exponent;
        encoded |= static_cast<std::uint16_t>(
            (encoded_exponent << 7) | (rounded & 0x7fu));
    }
    return static_cast<double>(std::bit_cast<float>(std::uint32_t(encoded) << 16));
}

// SWEGCA: src/swegca/mosaic_synapse_arbiter.py@5901a5a:263-286
[[nodiscard]] inline double arbiter_round_score(double value,
                                                 ScalarType type) noexcept {
    if (type == ScalarType::float64) return value;
    if (type == ScalarType::float16 || type == ScalarType::bfloat16)
        return arbiter_round_binary16(value, type);
    return static_cast<double>(arbiter_round_storage(static_cast<float>(value), type));
}

// SWEGCA: src/swegca/mosaic_synapse_arbiter.py@5901a5a:263-286
[[nodiscard]] inline ScoreValue arbiter_score_multiply(ScoreValue left,
                                                       ScoreValue right) noexcept {
    const ScalarType type = arbiter_promote(left.scalar_type, right.scalar_type);
    if (type == ScalarType::float64)
        return {type, left.value * right.value};
    const float result = static_cast<float>(left.value) * static_cast<float>(right.value);
    return {type, arbiter_round_score(result, type)};
}

// SWEGCA: src/swegca/mosaic_synapse_arbiter.py@5901a5a:263-286
[[nodiscard]] inline ScoreValue arbiter_score_complement(ScoreValue score) noexcept {
    if (score.scalar_type == ScalarType::float64)
        return {score.scalar_type, 1.0 - score.value};
    const float result = 1.0f - static_cast<float>(score.value);
    return {score.scalar_type, arbiter_round_score(result, score.scalar_type)};
}

// MaskAt(p, s, uint8_t&) and DeltaAt(p, s, w, T&) must be const noexcept
// bool-returning accessors over the same immutable, pinned Main snapshot.
// All values are checked before the first workspace write. A later failed
// read leaves final result spans unchanged because all writes remain staged.
// Slot clipping, directional products, weighted reductions, and world clipping
// follow the source's proposal and slot order. Storage is O(P + S + S*W).
// SWEGCA: src/swegca/mosaic_synapse_arbiter.py@5901a5a:263-321
template <class T, class R, class MaskAt, class DeltaAt>
[[nodiscard]] inline bool arbitrate_view(const ArbiterRules& rules, const ArbiterShape& shape,
                                         std::span<const ProposalScoresOf<T>> scores,
                                         const MaskAt& mask_at, const DeltaAt& delta_at,
                                         const ArbiterViewBuffersOf<T, R>& b) noexcept {
    static_assert(std::is_same_v<T, float> || std::is_same_v<T, double>);
    static_assert(std::is_same_v<R, float> || std::is_same_v<R, double>);
    static_assert(std::is_nothrow_invocable_r_v<bool, const MaskAt&, std::size_t, std::size_t,
                                                std::uint8_t&>);
    static_assert(std::is_nothrow_invocable_r_v<bool, const DeltaAt&, std::size_t, std::size_t,
                                                std::size_t, T&>);
    if constexpr (std::is_same_v<T, float>) {
        if (b.scalar_type != ScalarType::bfloat16 &&
            b.scalar_type != ScalarType::float16 &&
            b.scalar_type != ScalarType::float32)
            return false;
    } else if (b.scalar_type != ScalarType::float64) {
        return false;
    }
    if constexpr (std::is_same_v<R, float>) {
        if (b.result_scalar_type != ScalarType::float16 &&
            b.result_scalar_type != ScalarType::bfloat16 &&
            b.result_scalar_type != ScalarType::float32)
            return false;
    } else if (b.result_scalar_type != ScalarType::float64) {
        return false;
    }
    const std::size_t P = shape.proposals;
    const std::size_t S = shape.slots;
    const std::size_t W = shape.width;
    if (P == 0 || S == 0 || W == 0 || P > SIZE_MAX / S || P * S > SIZE_MAX / W ||
        S > SIZE_MAX / W)
        return false;
    if (scores.size() != P || b.weights.size() != P || b.accepted.size() != P ||
        b.conflict.size() != S || b.delta_out.size() != S * W ||
        b.work_weights.size() != P || b.work_accepted.size() != P ||
        b.work_conflict.size() != S || b.work_delta_out.size() != S * W ||
        b.slot_mask.size() != P || b.slot_denominator.size() != P ||
        b.slot_scale.size() != P)
        return false;
    const double maximum_slot_delta = rules.maximum_slot_delta_;
    const double maximum_world_delta = rules.maximum_world_delta_;
    const double minimum_weight = rules.minimum_weight_;
    if (!(std::isfinite(maximum_slot_delta) && maximum_slot_delta > 0 &&
          std::isfinite(maximum_world_delta) && maximum_world_delta > 0 &&
          finite_unit(minimum_weight)))
        return false;

    const std::array<std::span<const std::byte>, 4> final_spans{
        std::as_bytes(b.weights), std::as_bytes(b.accepted), std::as_bytes(b.conflict),
        std::as_bytes(b.delta_out)};
    const std::array<std::span<const std::byte>, 7> work_spans{
        std::as_bytes(b.work_weights), std::as_bytes(b.work_accepted),
        std::as_bytes(b.work_conflict), std::as_bytes(b.work_delta_out),
        std::as_bytes(b.slot_mask), std::as_bytes(b.slot_denominator),
        std::as_bytes(b.slot_scale)};
    for (std::size_t left = 0; left < final_spans.size(); ++left)
        for (std::size_t right = left + 1; right < final_spans.size(); ++right)
            if (arbiter_ranges_overlap(final_spans[left], final_spans[right])) return false;
    for (std::size_t left = 0; left < work_spans.size(); ++left) {
        for (std::size_t right = left + 1; right < work_spans.size(); ++right)
            if (arbiter_ranges_overlap(work_spans[left], work_spans[right])) return false;
        for (const auto final_span : final_spans)
            if (arbiter_ranges_overlap(work_spans[left], final_span)) return false;
    }
    const auto score_bytes = std::as_bytes(scores);
    for (const auto work_span : work_spans)
        if (arbiter_ranges_overlap(work_span, score_bytes)) return false;
    for (const auto final_span : final_spans)
        if (arbiter_ranges_overlap(final_span, score_bytes)) return false;

    const auto valid_score = [](ScoreValue value) noexcept {
        return arbiter_score_type(value.scalar_type) && finite_unit(value.value) &&
               arbiter_round_score(value.value, value.scalar_type) == value.value;
    };
    for (const auto& score : scores)
        if (!valid_score(score.confidence) || !valid_score(score.contradiction) ||
            !valid_score(score.uncertainty) ||
            !arbiter_score_type(score.delta_scalar_type) || score.source == 0)
            return false;
    ScalarType stacked_delta_type = scores.front().delta_scalar_type;
    for (const auto& score : scores)
        stacked_delta_type = arbiter_promote(stacked_delta_type,
                                             score.delta_scalar_type);
    if (b.scalar_type != stacked_delta_type) return false;
    for (std::size_t p = 0; p < P; ++p)
        for (std::size_t s = 0; s < S; ++s) {
            std::uint8_t mask = 0;
            if (!mask_at(p, s, mask) || mask > 1) return false;
            for (std::size_t w = 0; w < W; ++w) {
                T delta = 0;
                if (!delta_at(p, s, w, delta) || !std::isfinite(delta)) return false;
            }
        }

    ScalarType stacked_weight_type = ScalarType::float16;
    for (std::size_t p = 0; p < P; ++p) {
        const auto& score = scores[p];
        const ScoreValue first = arbiter_score_multiply(
            score.confidence, arbiter_score_complement(score.contradiction));
        const ScoreValue weight = arbiter_score_multiply(
            first, arbiter_score_complement(score.uncertainty));
        stacked_weight_type = p == 0 ? weight.scalar_type
                                     : arbiter_promote(stacked_weight_type,
                                                       weight.scalar_type);
        b.work_weights[p] = weight.value;
    }
    if (b.result_scalar_type != arbiter_promote(stacked_delta_type,
                                                stacked_weight_type))
        return false;
    // torch.stack converts each proposal weight to the common result_type.
    // `>=` wraps the Python policy scalar at that tensor dtype.
    const double threshold = arbiter_round_score(minimum_weight, stacked_weight_type);
    for (std::size_t p = 0; p < P; ++p) {
        const double weight = arbiter_round_score(b.work_weights[p], stacked_weight_type);
        b.work_weights[p] = weight;
        b.work_accepted[p] = weight >= threshold ? 1 : 0;
    }

    const auto bounded_at = [&](std::size_t p, std::size_t s, std::size_t w,
                                T& output) noexcept -> bool {
        T delta = 0;
        if (!delta_at(p, s, w, delta) || !std::isfinite(delta)) return false;
        if constexpr (std::is_same_v<T, double>) {
            if (scores[p].delta_scalar_type != ScalarType::float64) {
                const float narrow = static_cast<float>(delta);
                const float denominator = static_cast<float>(b.slot_denominator[p]);
                const float scale = static_cast<float>(b.slot_scale[p]);
                output = denominator == 0 ? static_cast<double>(narrow * scale)
                                          : static_cast<double>((narrow / denominator) * scale);
            } else {
                output = b.slot_denominator[p] == T{0}
                             ? delta * b.slot_scale[p]
                             : (delta / b.slot_denominator[p]) * b.slot_scale[p];
            }
        } else {
            output = b.slot_denominator[p] == T{0}
                         ? delta * b.slot_scale[p]
                         : (delta / b.slot_denominator[p]) * b.slot_scale[p];
        }
        // masked * scale materializes in this proposal's own dtype; stack
        // then materializes each bounded tensor in the promoted delta dtype.
        output = static_cast<T>(arbiter_round_score(
            output, scores[p].delta_scalar_type));
        output = static_cast<T>(arbiter_round_score(output, stacked_delta_type));
        return std::isfinite(output);
    };
    const T denominator_floor = static_cast<T>(1.0e-12);
    for (std::size_t s = 0; s < S; ++s) {
        for (std::size_t p = 0; p < P; ++p) {
            std::uint8_t mask = 0;
            if (!mask_at(p, s, mask) || mask > 1) return false;
            b.slot_mask[p] = mask;
            b.slot_denominator[p] = T{0};
            b.slot_scale[p] = T{1};
            if (mask == 0) continue;
            if constexpr (std::is_same_v<T, double>) {
                if (scores[p].delta_scalar_type != ScalarType::float64) {
                    float largest = 0;
                    for (std::size_t w = 0; w < W; ++w) {
                        T delta = 0;
                        if (!delta_at(p, s, w, delta) || !std::isfinite(delta)) return false;
                        largest = std::fmax(largest, std::fabs(static_cast<float>(delta)));
                    }
                    if (largest == 0) continue;
                    float sum = 0;
                    for (std::size_t w = 0; w < W; ++w) {
                        T delta = 0;
                        if (!delta_at(p, s, w, delta) || !std::isfinite(delta)) return false;
                        const float scaled = static_cast<float>(delta) / largest;
                        sum += scaled * scaled;
                    }
                    const float root = std::sqrt(sum);
                    if (!std::isfinite(sum) || !std::isfinite(root)) return false;
                    const float floor = static_cast<float>(1.0e-12);
                    if (static_cast<double>(largest) < static_cast<double>(floor) / root)
                        b.slot_scale[p] = maximum_slot_delta >= floor
                            ? 1.0 : static_cast<float>(maximum_slot_delta / floor);
                    else if (static_cast<double>(largest) > maximum_slot_delta / root) {
                        b.slot_denominator[p] = largest;
                        b.slot_scale[p] = static_cast<float>(maximum_slot_delta / root);
                    }
                    if (!std::isfinite(b.slot_scale[p])) return false;
                    continue;
                }
            }
            T largest = 0;
            for (std::size_t w = 0; w < W; ++w) {
                T delta = 0;
                if (!delta_at(p, s, w, delta) || !std::isfinite(delta)) return false;
                largest = std::fmax(largest, std::fabs(delta));
            }
            if (largest == T{0}) continue;
            T sum = 0;
            for (std::size_t w = 0; w < W; ++w) {
                T delta = 0;
                if (!delta_at(p, s, w, delta) || !std::isfinite(delta)) return false;
                const T scaled = delta / largest;
                sum += scaled * scaled;
            }
            const T root = std::sqrt(sum);
            if (!std::isfinite(sum) || !std::isfinite(root)) return false;
            if (static_cast<double>(largest) <
                static_cast<double>(denominator_floor) / root)
                b.slot_scale[p] = maximum_slot_delta >= denominator_floor
                                      ? T{1}
                                      : static_cast<T>(maximum_slot_delta /
                                                       denominator_floor);
            else if (static_cast<double>(largest) > maximum_slot_delta / root) {
                b.slot_denominator[p] = largest;
                b.slot_scale[p] = static_cast<T>(maximum_slot_delta / root);
            }
            if (!std::isfinite(b.slot_scale[p])) return false;
        }

        b.work_conflict[s] = 0;
        for (std::size_t left = 0; left < P; ++left) {
            if (!b.work_accepted[left] || !b.slot_mask[left]) continue;
            for (std::size_t right = left + 1; right < P; ++right) {
                if (!b.work_accepted[right] || !b.slot_mask[right] ||
                    scores[left].source == scores[right].source)
                    continue;
                // Pointwise products are materialized in storage dtype; the
                // reduced sum is cast to storage dtype before `< 0`. Half and
                // bfloat sum accumulation uses binary32, as in the source's
                // reduction backend. Float32/64 retain their ordered T sum.
                T product = 0;
                for (std::size_t w = 0; w < W; ++w) {
                    T a = 0;
                    T c = 0;
                    if (!bounded_at(left, s, w, a) || !bounded_at(right, s, w, c))
                        return false;
                    if constexpr (std::is_same_v<T, float>)
                        product += arbiter_round_storage(a * c, b.scalar_type);
                    else
                        product += a * c;
                }
                if constexpr (std::is_same_v<T, float>)
                    product = arbiter_round_storage(product, b.scalar_type);
                if (product < T{0}) b.work_conflict[s] = 1;
            }
        }

        double weight_sum = 0;
        if (stacked_weight_type == ScalarType::float64) {
            for (std::size_t p = 0; p < P; ++p)
                weight_sum += b.work_accepted[p] && b.slot_mask[p]
                                  ? b.work_weights[p] : 0.0;
        } else {
            float reduced = 0;
            for (std::size_t p = 0; p < P; ++p)
                reduced += b.work_accepted[p] && b.slot_mask[p]
                               ? static_cast<float>(b.work_weights[p]) : 0.0f;
            weight_sum = reduced;
        }
        weight_sum = arbiter_round_score(weight_sum, stacked_weight_type);
        const double denominator = std::fmax(
            weight_sum, arbiter_round_score(1.0e-12, stacked_weight_type));
        if (!std::isfinite(denominator)) return false;
        if (denominator == 0) {
            // A valid slot with no usable accepted weight abstains. The
            // source's f16 1e-12 clamp rounds to zero in this case.
            for (std::size_t w = 0; w < W; ++w)
                b.work_delta_out[s * W + w] = R{0};
            continue;
        }
        for (std::size_t w = 0; w < W; ++w) {
            R sum = 0;
            for (std::size_t p = 0; p < P; ++p) {
                if (!b.work_accepted[p] || !b.slot_mask[p]) continue;
                T bounded = 0;
                if (!bounded_at(p, s, w, bounded)) return false;
                const R term = static_cast<R>(arbiter_round_score(
                    static_cast<R>(bounded) * static_cast<R>(b.work_weights[p]),
                    b.result_scalar_type));
                if (!std::isfinite(term) || !std::isfinite(sum + term)) return false;
                sum += term;
            }
            sum = static_cast<R>(arbiter_round_score(sum, b.result_scalar_type));
            b.work_delta_out[s * W + w] = b.work_conflict[s]
                ? R{0}
                : static_cast<R>(arbiter_round_score(
                      sum / static_cast<R>(denominator), b.result_scalar_type));
            if (!std::isfinite(b.work_delta_out[s * W + w])) return false;
        }
    }
    if (!clip_norm<R>(std::span<const R>(b.work_delta_out), b.work_delta_out,
                      maximum_world_delta))
        return false;
    if constexpr (std::is_same_v<R, float>)
        for (R& value : b.work_delta_out) {
            value = static_cast<R>(arbiter_round_score(value, b.result_scalar_type));
            if (!std::isfinite(value)) return false;
        }
    for (std::size_t p = 0; p < P; ++p) {
        b.weights[p] = b.work_weights[p];
        b.accepted[p] = b.work_accepted[p];
    }
    for (std::size_t s = 0; s < S; ++s) b.conflict[s] = b.work_conflict[s];
    for (std::size_t at = 0; at < S * W; ++at) b.delta_out[at] = b.work_delta_out[at];
    return true;
}

}  // namespace kernel
}  // namespace swegca::architecture
