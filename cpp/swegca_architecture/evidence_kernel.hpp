#pragma once

#include "swegca_architecture/digest_bytes.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

// SWEGCA nano-core: the pure ternary judgment of evidence (accept, reject,
// abstain), the verifier the core is (user 2026-09-23). Gate and arbiter
// kernels are VRS (gate_kernel.hpp, arbiter_kernel.hpp; codex 16:55).
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
// Rules: ARCHITECTURE_SPEC.md@5901a5a §4.4 (decision).
namespace swegca::architecture {

struct EvidencePolicy;
namespace kernel {
class EvidenceRules;
}  // namespace kernel
[[nodiscard]] kernel::EvidenceRules make_evidence_rules(const EvidencePolicy& policy);

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
// too few samples, diversity or a regime change; accept only when
// the weakest axis lower bound clears the threshold; reject only when the
// pooled upper bound stays at or below it; abstain otherwise. As the author
// does (:314-318, :335), a window with fewer than `minimum_recent_samples`
// outcomes scores 0 and is still compared, so threshold 0 always abstains
// (Codex 2026-09-23 17:28).
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
    // D8(h), I04: an invalid tally must abstain. The author's Python lets
    // finite components overflow the posterior to NaN and can then accept;
    // this explicit fail-closed guard deliberately rejects that arithmetic.
    if (!std::isfinite(samples)) return out;
    const double posterior_numerator = supports + r.prior_alpha_;
    const double posterior_denominator = samples + r.prior_alpha_ + r.prior_beta_;
    if (!std::isfinite(posterior_numerator) || !std::isfinite(posterior_denominator))
        return out;
    const double posterior_mean = posterior_numerator / posterior_denominator;
    const double overall_upper = wilson_interval(supports, refutes, r.z_, r.z_squared_).upper;
    const bool measured = t.recent_count >= r.minimum_recent_samples_;
    const double regime_score =
        measured ? std::fabs(t.recent_sum / t.recent_count - posterior_mean) : 0.0;
    if (!std::isfinite(posterior_mean) || !std::isfinite(causal_lower) ||
        !std::isfinite(overall_upper) || !std::isfinite(regime_score))
        return out;
    out.posterior_mean = posterior_mean;
    out.causal_lower_bound = causal_lower;
    out.overall_upper_bound = overall_upper;
    out.effective_sample_size = samples;
    out.regime_change_score = regime_score;

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
    } else if (out.regime_change_score >= r.regime_change_threshold_) {
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

struct EvidenceByteRange {
    std::uintptr_t begin = 0;
    std::uintptr_t end = 0;
};

template <class T>
[[nodiscard]] inline bool evidence_byte_range(std::span<T> column,
                                              EvidenceByteRange& range) noexcept {
    if (column.empty()) {
        range = {};
        return true;
    }
    constexpr auto max_address = std::numeric_limits<std::uintptr_t>::max();
    if (column.size() > max_address / sizeof(T)) return false;
    const auto bytes = column.size() * sizeof(T);
    const auto begin = reinterpret_cast<std::uintptr_t>(column.data());
    if (begin > max_address - bytes) return false;
    range = {begin, begin + bytes};
    return true;
}

[[nodiscard]] inline bool evidence_ranges_overlap(EvidenceByteRange a,
                                                  EvidenceByteRange b) noexcept {
    return a.begin < a.end && b.begin < b.end && a.begin < b.end && b.begin < a.end;
}

// Judges items [first, last) of a batch; disjoint ranges may run on different
// workers. Returns false (and writes nothing) when a column does not fit or
// any output column overlaps an input or another output column.
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
    std::array<EvidenceByteRange, 8> inputs;
    if (!evidence_byte_range(in.axis_support, inputs[0]) ||
        !evidence_byte_range(in.axis_refute, inputs[1]) ||
        !evidence_byte_range(in.axis_source_diversity, inputs[2]) ||
        !evidence_byte_range(in.source_diversity, inputs[3]) ||
        !evidence_byte_range(in.context_diversity, inputs[4]) ||
        !evidence_byte_range(in.recent_count, inputs[5]) ||
        !evidence_byte_range(in.recent_sum, inputs[6]) ||
        !evidence_byte_range(in.revision, inputs[7]))
        return false;
    std::array<EvidenceByteRange, 7> outputs;
    if (!evidence_byte_range(out.status, outputs[0]) ||
        !evidence_byte_range(out.reason, outputs[1]) ||
        !evidence_byte_range(out.posterior_mean, outputs[2]) ||
        !evidence_byte_range(out.causal_lower_bound, outputs[3]) ||
        !evidence_byte_range(out.overall_upper_bound, outputs[4]) ||
        !evidence_byte_range(out.effective_sample_size, outputs[5]) ||
        !evidence_byte_range(out.regime_change_score, outputs[6]))
        return false;
    for (std::size_t output = 0; output < outputs.size(); ++output) {
        for (const auto input : inputs)
            if (evidence_ranges_overlap(outputs[output], input)) return false;
        for (std::size_t earlier = 0; earlier < output; ++earlier)
            if (evidence_ranges_overlap(outputs[output], outputs[earlier])) return false;
    }
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

}  // namespace kernel
}  // namespace swegca::architecture
