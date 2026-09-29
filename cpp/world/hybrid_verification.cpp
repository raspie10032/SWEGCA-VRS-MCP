#include "world/hybrid_verification.hpp"

#include <chrono>
#include <stdexcept>

namespace swegca::world {
namespace {

using Clock = std::chrono::steady_clock;

std::uint64_t elapsed_ns(const Clock::time_point started) {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - started).count());
}

}  // namespace

HybridVerificationConfig::HybridVerificationConfig(
    const std::size_t crossover)
    : minimum_accelerator_variants(crossover) {
    if (minimum_accelerator_variants == 0) {
        throw std::invalid_argument("accelerator crossover must be positive");
    }
}

VerificationBackend select_verification_backend(
    const std::size_t variant_count, const bool accelerator_available,
    const HybridVerificationConfig& config) {
    if (variant_count == 0) {
        throw std::invalid_argument("variant count must be positive");
    }
    if (accelerator_available &&
        variant_count >= config.minimum_accelerator_variants) {
        return VerificationBackend::accelerator;
    }
    return VerificationBackend::cpu;
}

HybridVerificationReceipt evaluate_and_advance_hybrid_verification(
    const CognitiveState& state, const AutonomyEvent& event,
    const AutonomousCognitionConfig& autonomy_config,
    const EvidenceAccumulatorConfig& evidence_config,
    const CompiledEvidenceReplay& cpu_cache,
    const CompiledReplayInterventions& cpu_interventions,
    const std::string_view intervention,
    const HybridVerificationConfig& config) {
    const auto backend = select_verification_backend(
        cpu_interventions.variant_count(), false, config);
    const auto total_started = Clock::now();
    const auto evaluation_started = Clock::now();
    const auto result = evaluate_compiled_counterfactual_replays(
        cpu_cache, cpu_interventions, evidence_config);
    const auto evaluation_ns = elapsed_ns(evaluation_started);
    auto verification = advance_accelerated_verification(
        state, event, autonomy_config, result, intervention);
    return {backend, std::move(verification), evaluation_ns,
            elapsed_ns(total_started)};
}

}  // namespace swegca::world
