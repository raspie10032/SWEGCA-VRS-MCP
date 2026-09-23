#include "swegca_architecture/judgment_rules.hpp"

#include "swegca_architecture/sha256.hpp"

#include <array>
#include <bit>

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

// SWEGCA: src/swegca/mosaic_evidence_accumulator.py@5901a5a:261-283
double standard_normal_quantile(double probability) {
    if (!(probability > 0 && probability < 1))
        throw std::invalid_argument("normal_quantile_probability_invalid");
    // Phi(z) = erfc(-z / sqrt(2)) / 2 is increasing; bisect to double precision.
    double low = -40;
    double high = 40;
    for (int step = 0; step < 200 && low < high; ++step) {
        const double middle = low + (high - low) / 2;
        if (middle <= low || middle >= high) break;
        if (0.5 * std::erfc(-middle / std::sqrt(2.0)) < probability)
            low = middle;
        else
            high = middle;
    }
    return low + (high - low) / 2;
}

// SWEGCA: src/swegca/mosaic_evidence_accumulator.py@5901a5a:49-95
kernel::EvidenceRules make_evidence_rules(const EvidencePolicy& p) {
    constexpr const char* name = "evidence_policy";
    if (!(std::isfinite(p.chance_rate) && p.chance_rate >= 0 && p.chance_rate < 1))
        invalid(name, "chance_rate");
    if (!(std::isfinite(p.accept_margin) && p.accept_margin >= 0 &&
          p.accept_margin <= 1 - p.chance_rate))
        invalid(name, "accept_margin");
    if (!(std::isfinite(p.confidence_level) && p.confidence_level > 0 &&
          p.confidence_level < 1))
        invalid(name, "confidence_level");
    if (!(std::isfinite(p.prior_alpha) && p.prior_alpha > 0)) invalid(name, "prior_alpha");
    if (!(std::isfinite(p.prior_beta) && p.prior_beta > 0)) invalid(name, "prior_beta");
    if (p.minimum_effective_samples_per_axis == 0 || p.minimum_source_diversity == 0 ||
        p.minimum_axis_source_diversity == 0 || p.minimum_context_diversity == 0 ||
        p.recent_window == 0 || p.minimum_recent_samples == 0)
        invalid(name, "minimum_limits");
    if (p.minimum_recent_samples > p.recent_window) invalid(name, "minimum_recent_samples");
    if (!unit(p.regime_change_threshold)) invalid(name, "regime_change_threshold");
    if (p.axis_count == 0 || p.axis_count > kernel::max_axes) invalid(name, "axis_count");

    kernel::EvidenceRules rules;
    rules.z_ = standard_normal_quantile(0.5 + p.confidence_level / 2);
    rules.z_squared_ = rules.z_ * rules.z_;
    rules.threshold_ = p.chance_rate + p.accept_margin;
    rules.prior_alpha_ = p.prior_alpha;
    rules.prior_beta_ = p.prior_beta;
    rules.minimum_effective_samples_per_axis_ = p.minimum_effective_samples_per_axis;
    rules.minimum_source_diversity_ = p.minimum_source_diversity;
    rules.minimum_axis_source_diversity_ = p.minimum_axis_source_diversity;
    rules.minimum_context_diversity_ = p.minimum_context_diversity;
    rules.minimum_recent_samples_ = p.minimum_recent_samples;
    rules.recent_window_ = p.recent_window;
    rules.regime_change_threshold_ = p.regime_change_threshold;
    rules.axis_count_ = p.axis_count;
    if (!kernel::rules_valid(rules)) invalid(name, "derived_rules");
    return rules;
}

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

// SWEGCA: src/swegca/mosaic_evidence_accumulator.py@5901a5a:49-67
Digest256 evidence_policy_digest(const EvidencePolicy& p) {
    Sha256 hash;
    hash.update(std::string_view("swegca.evidence_policy.v1"));
    const auto put = [&hash](std::uint64_t value, std::size_t width) {
        std::array<std::byte, 8> bytes{};
        for (std::size_t at = 0; at < width; ++at)
            bytes[at] = static_cast<std::byte>((value >> (8 * at)) & 0xff);
        hash.update(std::span<const std::byte>(bytes.data(), width));
    };
    for (const double value : {p.chance_rate, p.accept_margin, p.confidence_level,
                               p.prior_alpha, p.prior_beta})
        put(std::bit_cast<std::uint64_t>(value), 8);
    for (const std::uint32_t value :
         {p.minimum_effective_samples_per_axis, p.minimum_source_diversity,
          p.minimum_axis_source_diversity, p.minimum_context_diversity, p.recent_window,
          p.minimum_recent_samples})
        put(value, 4);
    put(std::bit_cast<std::uint64_t>(p.regime_change_threshold), 8);
    put(p.axis_count, 4);
    return Digest256(hash.finish());
}

}  // namespace swegca::architecture
