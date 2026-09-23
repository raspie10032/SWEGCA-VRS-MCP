#include "swegca_vrs/gate_rules.hpp"

#include "swegca_vrs/core_sha256.hpp"

#include <array>
#include <bit>
#include <cmath>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

namespace swegca::vrs {
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

// Lineage: native mechanism — the author judges and previews under one
// config object in one call; the C++ gate and writer are separate, so the
// capability carries this identity of the gate's thresholds.
// SWEGCA: src/tinylm_slicer/mosaic_bounded_world_write.py@3bddcb7:379-412
Digest256 gate_policy_digest(const GatePolicy& p) {
    Sha256 hash;
    hash.update(std::string_view("swegca.gate_policy.v1"));
    const auto put = [&hash](std::uint64_t value, std::size_t width) {
        std::array<std::byte, 8> bytes{};
        for (std::size_t at = 0; at < width; ++at)
            bytes[at] = static_cast<std::byte>((value >> (8 * at)) & 0xff);
        hash.update(std::span<const std::byte>(bytes.data(), width));
    };
    // +0.0 folds a negative zero, which compares equal, to one identity.
    put(std::bit_cast<std::uint64_t>(p.minimum_causal_lower_bound + 0.0), 8);
    put(p.minimum_source_diversity, 4);
    put(p.minimum_context_diversity, 4);
    return Digest256(hash.finish());
}

}  // namespace swegca::vrs
