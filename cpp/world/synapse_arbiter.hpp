#pragma once

#include "world/cognitive_state.hpp"
#include "world/world_state.hpp"

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace swegca::world {

struct SynapseProposal final {
    std::string source;
    Tensor delta_candidate;
    std::vector<double> confidence;
    std::vector<double> contradiction;
    std::vector<double> uncertainty;
    BooleanMask target_slot_mask;
    std::vector<std::vector<std::string>> evidence_addresses;
    std::string hypothesis_id;

    void validate(const WorldState& world) const;
    void validate(const CognitiveState& world) const;
};

[[nodiscard]] SynapseProposal sufficiency_gated_proposal(
    const WorldState& world, const SynapseProposal& proposal,
    const BooleanMask& sufficient_mask);

[[nodiscard]] SynapseProposal sufficiency_gated_proposal(
    const CognitiveState& world, const SynapseProposal& proposal,
    const BooleanMask& sufficient_mask);

template <typename State>
class ArbitrationResult final {
public:
    ArbitrationResult(std::shared_ptr<const State> world_state, bool committed,
                      Tensor proposed_delta, Tensor proposal_weights,
                      BooleanMask accepted, BooleanMask unresolved_contradiction,
                      std::vector<std::string> sources)
        : world_state_(std::move(world_state)), committed_(committed),
          proposed_delta_(std::move(proposed_delta)),
          proposal_weights_(std::move(proposal_weights)), accepted_(std::move(accepted)),
          unresolved_contradiction_(std::move(unresolved_contradiction)),
          sources_(std::move(sources)) {}

    [[nodiscard]] const std::shared_ptr<const State>& world_state() const noexcept {
        return world_state_;
    }
    [[nodiscard]] const Tensor& proposed_delta() const noexcept { return proposed_delta_; }
    [[nodiscard]] const Tensor& proposal_weights() const noexcept { return proposal_weights_; }
    [[nodiscard]] const BooleanMask& accepted() const noexcept { return accepted_; }
    [[nodiscard]] const BooleanMask& unresolved_contradiction() const noexcept {
        return unresolved_contradiction_;
    }
    [[nodiscard]] std::span<const std::string> sources() const noexcept { return sources_; }
    [[nodiscard]] bool committed() const noexcept { return committed_; }

private:
    std::shared_ptr<const State> world_state_;
    bool committed_;
    Tensor proposed_delta_;
    Tensor proposal_weights_;
    BooleanMask accepted_;
    BooleanMask unresolved_contradiction_;
    std::vector<std::string> sources_;
};

class SingleWorldArbiter final {
public:
    explicit SingleWorldArbiter(double maximum_slot_delta = 0.1,
                                double maximum_world_delta = 0.5,
                                double minimum_weight = 0.25);

    [[nodiscard]] ArbitrationResult<WorldState> operator()(
        std::shared_ptr<const WorldState> world, std::span<const SynapseProposal> proposals,
        bool commit = false) const;

    [[nodiscard]] ArbitrationResult<CognitiveState> operator()(
        std::shared_ptr<const CognitiveState> world, std::span<const SynapseProposal> proposals,
        bool commit = false) const;

    [[nodiscard]] double maximum_slot_delta() const noexcept { return maximum_slot_delta_; }
    [[nodiscard]] double maximum_world_delta() const noexcept { return maximum_world_delta_; }
    [[nodiscard]] double minimum_weight() const noexcept { return minimum_weight_; }

private:
    double maximum_slot_delta_;
    double maximum_world_delta_;
    double minimum_weight_;
};

}  // namespace swegca::world
