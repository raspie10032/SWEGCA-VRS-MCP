#include "world/hybrid_verification.hpp"

#include <cassert>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

using namespace swegca::world;

namespace {

constexpr std::string_view hypothesis = "hybrid-test";

std::vector<EvidenceObservation> observations() {
    const EvidenceAccumulatorConfig config;
    std::vector<EvidenceObservation> result;
    for (int index = 0; index != 16; ++index) {
        result.emplace_back(
            std::string(hypothesis), "evidence:" + std::to_string(index),
            "source:" + std::to_string(index % 2), "context:" + std::to_string(index),
            config.required_axes[static_cast<std::size_t>(index) % config.required_axes.size()],
            "support", index, std::nullopt,
            "hybrid-producer:" + std::to_string(index), 0.0);
    }
    return result;
}

CognitiveState state() {
    const auto tensor = [](const std::uint64_t slots) {
        return Tensor(TensorDType::float32, {1, slots, 8},
                      std::vector<double>(static_cast<std::size_t>(slots * 8)));
    };
    return CognitiveState(tensor(4), tensor(2), tensor(2), {}, {},
                          {{"autonomy_phase", "verify"},
                           {"active_hypothesis_id", std::string(hypothesis)}});
}

}  // namespace

int main() {
    const HybridVerificationConfig config(8);
    assert(select_verification_backend(7, true, config) == VerificationBackend::cpu);
    assert(select_verification_backend(8, true, config) == VerificationBackend::accelerator);
    assert(select_verification_backend(128, false, config) == VerificationBackend::cpu);
    bool invalid = false;
    try {
        static_cast<void>(HybridVerificationConfig(0));
    } catch (const std::invalid_argument&) {
        invalid = true;
    }
    assert(invalid);

    const EvidenceAccumulatorConfig evidence_config;
    const auto evidence = observations();
    const auto cache = compile_evidence_replay(
        evidence, evidence_config, std::string(hypothesis), 15);
    const std::vector<ReplayIntervention> plans{{"normal", {}, {}, {}}};
    const auto interventions = compile_replay_interventions(cache, plans);
    const AutonomyEvent event(
        "hybrid-verification", AutonomyEventKind::verification,
        std::string(hypothesis), {"replay-cache:" + cache.cache_hash},
        "hybrid-test", "hybrid-context", 1.0,
        {{"replay_revision", static_cast<std::int64_t>(cache.stats.applied)},
         {"replay_intervention", "normal"}});
    const auto original = state();
    const auto receipt = evaluate_and_advance_hybrid_verification(
        original, event, AutonomousCognitionConfig({"inspect"}, {"inspect"}),
        evidence_config, cache, interventions, "normal", config);
    assert(receipt.backend == VerificationBackend::cpu);
    assert(receipt.verification.transition.next_phase == AutonomyPhase::remember);
    assert(receipt.verification.world_tensor_objects_reused);
    assert(receipt.verification.transition.state.semantic_slots().storage_identity() ==
           original.semantic_slots().storage_identity());
    assert(hybrid_verification_source_sha256() ==
           "0264ad061c48fdd52b9447f280dfef05bf7ac54f404d0a496e1212b5f38f87cb");
    std::cout << "hybrid verification CPU fallback tests passed\n";
}
