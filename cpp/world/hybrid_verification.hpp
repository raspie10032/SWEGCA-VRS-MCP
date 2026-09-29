#pragma once

#include "world/accelerated_verification.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace swegca::world {

enum class VerificationBackend : std::uint8_t { cpu, accelerator };

class HybridVerificationConfig final {
public:
    explicit HybridVerificationConfig(std::size_t minimum_accelerator_variants);

    const std::size_t minimum_accelerator_variants;
};

struct HybridVerificationReceipt final {
    VerificationBackend backend;
    AcceleratedVerificationReceipt verification;
    std::uint64_t evaluation_cycle_ns;
    std::uint64_t total_cycle_ns;
};

[[nodiscard]] VerificationBackend select_verification_backend(
    std::size_t variant_count, bool accelerator_available,
    const HybridVerificationConfig& config);

// CompiledEvidenceReplay is CPU-resident in the current C++ target. This path
// therefore implements the pinned fallback without pretending that a host
// vector is an accelerator cache.
[[nodiscard]] HybridVerificationReceipt evaluate_and_advance_hybrid_verification(
    const CognitiveState& state, const AutonomyEvent& event,
    const AutonomousCognitionConfig& autonomy_config,
    const EvidenceAccumulatorConfig& evidence_config,
    const CompiledEvidenceReplay& cpu_cache,
    const CompiledReplayInterventions& cpu_interventions,
    std::string_view intervention, const HybridVerificationConfig& config);

[[nodiscard]] constexpr std::string_view hybrid_verification_source_sha256() noexcept {
    return "0264ad061c48fdd52b9447f280dfef05bf7ac54f404d0a496e1212b5f38f87cb";
}

}  // namespace swegca::world
