#pragma once

#include "swegca_architecture/evidence_kernel.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>

// VRS nano-kernel: the Single-World arbiter. Its output is a proposal
// weight and a bounded delta, not a verdict, so it belongs to the synapse
// (VRS), not the core verifier (codex 16:55).
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
// Rules: ARCHITECTURE_SPEC.md@5901a5a §4.6 (arbitration).
namespace swegca::architecture {

struct ArbiterPolicy;
namespace kernel {
class ArbiterRules;
}  // namespace kernel
[[nodiscard]] kernel::ArbiterRules make_arbiter_rules(const ArbiterPolicy& policy);

namespace kernel {

inline constexpr double max_delta_limit = 1.0e6;  // keeps every sum finite

struct ProposalScores {
    float confidence = 0;
    float contradiction = 0;
    float uncertainty = 0;
    std::uint32_t source = 0;  // dense nonzero source id assigned by the shell
};

struct ArbiterShape {
    std::size_t proposals = 0;
    std::size_t slots = 0;
    std::size_t width = 0;
};

// Caller-owned buffers; the kernel allocates nothing. Layouts are row-major:
// masks[p][s], deltas[p][s][w], bounded[p][s][w], delta_out[s][w].
struct ArbiterBuffers {
    std::span<const ProposalScores> scores;
    std::span<const std::uint8_t> masks;
    std::span<const float> deltas;
    std::span<float> weights;
    std::span<std::uint8_t> accepted;
    std::span<float> bounded;  // workspace
    std::span<std::uint8_t> conflict;
    std::span<float> delta_out;
};

// Validated arbiter limits; only `make_arbiter_rules` constructs one.
class ArbiterRules final {
private:
    ArbiterRules() = default;
    friend ArbiterRules swegca::architecture::make_arbiter_rules(const ArbiterPolicy&);
    friend bool arbitrate(const ArbiterRules& rules, const ArbiterShape& shape,
                          const ArbiterBuffers& buffers) noexcept;

    float maximum_slot_delta_ = 0;
    float maximum_world_delta_ = 0;
    float minimum_weight_ = 0;
};

// Euclidean norm scaled by the largest magnitude so no square overflows.
// SWEGCA: src/swegca/mosaic_synapse_arbiter.py@5901a5a:274-286
[[nodiscard]] inline float stable_norm(std::span<const float> values) noexcept {
    float largest = 0;
    for (const float value : values) largest = std::fmax(largest, std::fabs(value));
    if (largest == 0) return 0;
    float sum = 0;
    for (const float value : values) {
        const float scaled = value / largest;
        sum += scaled * scaled;
    }
    return largest * std::sqrt(sum);
}

// Preflight before any write (codex KJ2): shape nonzero, buffers exact, rules
// finite and bounded, scores finite in [0, 1], sources nonzero, masks 0/1,
// every delta finite.
// SWEGCA: src/swegca/mosaic_synapse_arbiter.py@5901a5a:238-262
[[nodiscard]] inline bool arbiter_input_valid(const ArbiterShape& shape,
                                              const ArbiterBuffers& b) noexcept {
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
    for (const float delta : b.deltas)
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
[[nodiscard]] inline bool arbitrate(const ArbiterRules& rules, const ArbiterShape& shape,
                                    const ArbiterBuffers& b) noexcept {
    if (!(std::isfinite(rules.maximum_slot_delta_) && rules.maximum_slot_delta_ > 0 &&
          rules.maximum_slot_delta_ <= max_delta_limit &&
          std::isfinite(rules.maximum_world_delta_) && rules.maximum_world_delta_ > 0 &&
          rules.maximum_world_delta_ <= max_delta_limit && finite_unit(rules.minimum_weight_)))
        return false;
    if (!arbiter_input_valid(shape, b)) return false;
    const std::size_t P = shape.proposals;
    const std::size_t S = shape.slots;
    const std::size_t W = shape.width;

    for (std::size_t p = 0; p < P; ++p) {
        const auto& score = b.scores[p];
        const float weight =
            score.confidence * (1 - score.contradiction) * (1 - score.uncertainty);
        b.weights[p] = weight;
        b.accepted[p] = weight >= rules.minimum_weight_ ? 1 : 0;
        for (std::size_t s = 0; s < S; ++s) {
            const std::size_t row = (p * S + s) * W;
            if (b.masks[p * S + s] == 0) {
                for (std::size_t w = 0; w < W; ++w) b.bounded[row + w] = 0.0f;
                continue;
            }
            const float norm = stable_norm(b.deltas.subspan(row, W));
            const float scale = norm > rules.maximum_slot_delta_
                                    ? rules.maximum_slot_delta_ / norm
                                    : 1.0f;
            for (std::size_t w = 0; w < W; ++w) b.bounded[row + w] = b.deltas[row + w] * scale;
        }
    }

    for (std::size_t s = 0; s < S; ++s) b.conflict[s] = 0;
    for (std::size_t left = 0; left < P; ++left) {
        if (!b.accepted[left]) continue;
        for (std::size_t right = left + 1; right < P; ++right) {
            if (!b.accepted[right] || b.scores[left].source == b.scores[right].source) continue;
            for (std::size_t s = 0; s < S; ++s) {
                if (!b.masks[left * S + s] || !b.masks[right * S + s]) continue;
                float product = 0;
                for (std::size_t w = 0; w < W; ++w)
                    product += b.bounded[(left * S + s) * W + w] *
                               b.bounded[(right * S + s) * W + w];
                if (product < 0) b.conflict[s] = 1;
            }
        }
    }

    for (std::size_t s = 0; s < S; ++s) {
        float weight_sum = 0;
        for (std::size_t p = 0; p < P; ++p)
            weight_sum += b.accepted[p] && b.masks[p * S + s] ? b.weights[p] : 0.0f;
        for (std::size_t w = 0; w < W; ++w) {
            float sum = 0;
            for (std::size_t p = 0; p < P; ++p)
                if (b.accepted[p] && b.masks[p * S + s])
                    sum += b.bounded[(p * S + s) * W + w] * b.weights[p];
            b.delta_out[s * W + w] =
                b.conflict[s] || !(weight_sum > 0) ? 0.0f : sum / weight_sum;
        }
    }
    const float world = stable_norm(b.delta_out);
    if (world > rules.maximum_world_delta_) {
        const float scale = rules.maximum_world_delta_ / world;
        for (std::size_t at = 0; at < S * W; ++at) b.delta_out[at] *= scale;
    }
    return true;
}

}  // namespace kernel
}  // namespace swegca::architecture
