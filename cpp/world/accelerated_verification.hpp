#pragma once

#include "world/autonomous_cognition.hpp"
#include "world/counterfactual_replay.hpp"

#include <cstddef>
#include <cstdint>
#include <string>

namespace swegca::world {

struct SelectedReplayDecision final {
    AccumulatorDecision decision;
    std::string intervention;
    std::size_t index;
    std::uint64_t selection_cycle_ns;
};

struct AcceleratedVerificationReceipt final {
    AutonomyTransition transition;
    SelectedReplayDecision selected;
    std::string cache_hash;
    std::uint64_t transition_cycle_ns;
    bool world_tensor_objects_reused;
};

[[nodiscard]] SelectedReplayDecision select_replay_decision(
    const ReplayBatchResult& result, std::string intervention);

[[nodiscard]] AcceleratedVerificationReceipt advance_accelerated_verification(
    const CognitiveState& state, const AutonomyEvent& event,
    const AutonomousCognitionConfig& config,
    const ReplayBatchResult& result, std::string_view intervention);

[[nodiscard]] constexpr std::string_view accelerated_verification_source_sha256() noexcept {
    return "72a9c65a0e40451661b24fef926e26564baf5d2fd2c0128bda6673af55685c94";
}

}  // namespace swegca::world
