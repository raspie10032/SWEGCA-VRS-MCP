#include "world/synapse_arbiter.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <memory>
#include <stdexcept>
#include <utility>

namespace swegca::world {
namespace {

struct SlotShape final {
    std::uint64_t batch;
    std::uint64_t slots;
    std::uint64_t dimension;
    TensorDType dtype;
    std::string device;
};

[[nodiscard]] bool same_shape(std::span<const std::uint64_t> actual,
                              std::initializer_list<std::uint64_t> expected);

[[nodiscard]] std::size_t checked_product(
    const std::initializer_list<std::uint64_t> dimensions) {
    std::size_t result = 1;
    for (const auto dimension : dimensions) {
        if (dimension != 0 && result > std::numeric_limits<std::size_t>::max() / dimension) {
            throw std::overflow_error("slot tensor size overflow");
        }
        result *= static_cast<std::size_t>(dimension);
    }
    return result;
}

[[nodiscard]] std::size_t offset(const SlotShape& shape, const std::uint64_t batch,
                                 const std::uint64_t slot, const std::uint64_t dimension) {
    return static_cast<std::size_t>((batch * shape.slots + slot) * shape.dimension + dimension);
}

[[nodiscard]] SlotShape slot_shape(const WorldState& world) {
    const auto shape = world.semantic_slots().shape();
    if (shape.size() != 3) {
        throw std::invalid_argument("World State slots must have rank 3");
    }
    const auto expected_mask_size = checked_product({shape[0], shape[1]});
    if (!same_shape(world.active_mask().shape(), {shape[0], shape[1]}) ||
        !same_shape(world.dirty_mask().shape(), {shape[0], shape[1]}) ||
        world.active_mask().values().size() != expected_mask_size ||
        world.dirty_mask().values().size() != expected_mask_size) {
        throw std::invalid_argument("World State masks must match slot structure");
    }
    static_cast<void>(checked_product({shape[0], shape[1], shape[2]}));
    return {shape[0], shape[1], shape[2], world.semantic_slots().dtype(),
            std::string(world.semantic_slots().device())};
}

[[nodiscard]] SlotShape slot_shape(const CognitiveState& world) {
    const auto semantic = world.semantic_slots().shape();
    const auto executive = world.executive_slots().shape();
    const auto scratch = world.scratch_slots().shape();
    if (semantic.size() != 3 || executive.size() != 3 || scratch.size() != 3 ||
        executive[0] != semantic[0] || scratch[0] != semantic[0] ||
        executive[2] != semantic[2] || scratch[2] != semantic[2]) {
        throw std::invalid_argument("Cognitive State slots do not form one slot tensor");
    }
    if (world.executive_slots().dtype() != world.semantic_slots().dtype() ||
        world.scratch_slots().dtype() != world.semantic_slots().dtype() ||
        world.executive_slots().device() != world.semantic_slots().device() ||
        world.scratch_slots().device() != world.semantic_slots().device()) {
        throw std::invalid_argument("Cognitive State slot tensors must share dtype and device");
    }
    if (semantic[1] > std::numeric_limits<std::uint64_t>::max() - executive[1] ||
        semantic[1] + executive[1] >
            std::numeric_limits<std::uint64_t>::max() - scratch[1]) {
        throw std::overflow_error("Cognitive State slot count overflow");
    }
    const auto slots = semantic[1] + executive[1] + scratch[1];
    static_cast<void>(checked_product({semantic[0], slots, semantic[2]}));
    return {semantic[0], slots, semantic[2],
            world.semantic_slots().dtype(), std::string(world.semantic_slots().device())};
}

[[nodiscard]] bool same_shape(const std::span<const std::uint64_t> actual,
                              const std::initializer_list<std::uint64_t> expected) {
    return actual.size() == expected.size() &&
           std::equal(actual.begin(), actual.end(), expected.begin(), expected.end());
}

void validate_proposal(const SynapseProposal& proposal, const SlotShape& shape) {
    if (proposal.source.empty()) {
        throw std::invalid_argument("proposal source must be nonempty");
    }
    if (!same_shape(proposal.delta_candidate.shape(),
                    {shape.batch, shape.slots, shape.dimension})) {
        throw std::invalid_argument("proposal delta must match World State");
    }
    if (proposal.delta_candidate.device() != shape.device) {
        throw std::invalid_argument("proposal delta device must match World State");
    }
    if (!same_shape(proposal.target_slot_mask.shape(), {shape.batch, shape.slots})) {
        throw std::invalid_argument("proposal target mask must match World slots");
    }
    for (const auto& [name, values] :
         {std::pair{"confidence", &proposal.confidence},
          std::pair{"contradiction", &proposal.contradiction},
          std::pair{"uncertainty", &proposal.uncertainty}}) {
        if (values->size() != shape.batch) {
            throw std::invalid_argument(std::string("proposal ") + name +
                                        " must have shape [batch]");
        }
        for (const double value : *values) {
            if (!std::isfinite(value)) {
                throw std::invalid_argument(std::string("proposal ") + name +
                                            " must be finite");
            }
            if (value < 0.0 || value > 1.0) {
                throw std::invalid_argument(std::string("proposal ") + name +
                                            " must be in [0, 1]");
            }
        }
    }
    if (std::any_of(proposal.delta_candidate.values().begin(),
                    proposal.delta_candidate.values().end(),
                    [](const double value) { return !std::isfinite(value); })) {
        throw std::invalid_argument("proposal delta must be finite");
    }
    if (!proposal.evidence_addresses.empty() &&
        proposal.evidence_addresses.size() != shape.batch) {
        throw std::invalid_argument("proposal evidence addresses must match batch");
    }
}

template <typename State>
[[nodiscard]] SynapseProposal sufficiency_gate(const State& world,
                                               const SynapseProposal& proposal,
                                               const BooleanMask& sufficient_mask) {
    proposal.validate(world);
    const auto shape = slot_shape(world);
    if (!same_shape(sufficient_mask.shape(), {shape.batch})) {
        throw std::invalid_argument("sufficient mask must have shape [batch]");
    }
    auto result = proposal;
    for (std::size_t batch = 0; batch < result.confidence.size(); ++batch) {
        if (sufficient_mask.values()[batch] == 0) {
            result.confidence[batch] = 0.0;
            result.uncertainty[batch] = 1.0;
        }
    }
    result.validate(world);
    return result;
}

struct Reduction final {
    Tensor proposed_delta;
    Tensor proposal_weights;
    BooleanMask accepted;
    BooleanMask unresolved;
    std::vector<std::uint8_t> dirty;
    std::vector<std::string> sources;
};

template <typename State>
[[nodiscard]] Reduction reduce(const State& world,
                               const std::span<const SynapseProposal> proposals,
                               const double maximum_slot_delta,
                               const double maximum_world_delta,
                               const double minimum_weight) {
    const auto shape = slot_shape(world);
    const auto proposal_count = proposals.size();
    const auto slot_elements = checked_product({shape.batch, shape.slots, shape.dimension});
    if (proposal_count != 0 && slot_elements > std::numeric_limits<std::size_t>::max() / proposal_count) {
        throw std::overflow_error("proposal reduction size overflow");
    }
    if (proposal_count != 0 && static_cast<std::size_t>(shape.batch) >
                                   std::numeric_limits<std::size_t>::max() / proposal_count) {
        throw std::overflow_error("proposal weight size overflow");
    }
    const auto weight_elements = static_cast<std::size_t>(shape.batch) * proposal_count;
    const auto slot_count = checked_product({shape.batch, shape.slots});
    std::vector<double> result(slot_elements, 0.0);
    std::vector<double> weights(weight_elements, 0.0);
    std::vector<std::uint8_t> accepted(weight_elements, 0);
    std::vector<std::uint8_t> unresolved(slot_count, 0);
    std::vector<double> bounded(proposal_count * slot_elements, 0.0);
    std::vector<std::string> sources;
    sources.reserve(proposal_count);

    for (std::size_t proposal_index = 0; proposal_index < proposal_count; ++proposal_index) {
        const auto& proposal = proposals[proposal_index];
        proposal.validate(world);
        sources.push_back(proposal.source);
        for (std::uint64_t batch = 0; batch < shape.batch; ++batch) {
            const auto weight_index = static_cast<std::size_t>(batch) * proposal_count + proposal_index;
            const double weight = proposal.confidence[batch] *
                                  (1.0 - proposal.contradiction[batch]) *
                                  (1.0 - proposal.uncertainty[batch]);
            weights[weight_index] = weight;
            accepted[weight_index] = static_cast<std::uint8_t>(weight >= minimum_weight);
            for (std::uint64_t slot = 0; slot < shape.slots; ++slot) {
                if (!proposal.target_slot_mask.at(batch, slot)) {
                    continue;
                }
                double squared_norm = 0.0;
                for (std::uint64_t dimension = 0; dimension < shape.dimension; ++dimension) {
                    const double value = proposal.delta_candidate.values()[offset(shape, batch, slot, dimension)];
                    squared_norm += value * value;
                }
                const double norm = std::sqrt(squared_norm);
                const double scale = std::min(1.0, maximum_slot_delta / std::max(norm, 1e-12));
                for (std::uint64_t dimension = 0; dimension < shape.dimension; ++dimension) {
                    const auto element = offset(shape, batch, slot, dimension);
                    bounded[proposal_index * slot_elements + element] =
                        proposal.delta_candidate.values()[element] * scale;
                }
            }
        }
    }

    for (std::size_t left = 0; left < proposal_count; ++left) {
        for (std::size_t right = left + 1; right < proposal_count; ++right) {
            if (proposals[left].source == proposals[right].source) {
                continue;
            }
            for (std::uint64_t batch = 0; batch < shape.batch; ++batch) {
                const bool both_accepted =
                    accepted[static_cast<std::size_t>(batch) * proposal_count + left] != 0 &&
                    accepted[static_cast<std::size_t>(batch) * proposal_count + right] != 0;
                if (!both_accepted) {
                    continue;
                }
                for (std::uint64_t slot = 0; slot < shape.slots; ++slot) {
                    if (!proposals[left].target_slot_mask.at(batch, slot) ||
                        !proposals[right].target_slot_mask.at(batch, slot)) {
                        continue;
                    }
                    double dot = 0.0;
                    for (std::uint64_t dimension = 0; dimension < shape.dimension; ++dimension) {
                        const auto element = offset(shape, batch, slot, dimension);
                        dot += bounded[left * slot_elements + element] *
                               bounded[right * slot_elements + element];
                    }
                    if (dot < 0.0) {
                        unresolved[static_cast<std::size_t>(batch * shape.slots + slot)] = 1;
                    }
                }
            }
        }
    }

    for (std::uint64_t batch = 0; batch < shape.batch; ++batch) {
        for (std::uint64_t slot = 0; slot < shape.slots; ++slot) {
            double denominator = 0.0;
            for (std::size_t proposal_index = 0; proposal_index < proposal_count; ++proposal_index) {
                const auto weight_index = static_cast<std::size_t>(batch) * proposal_count + proposal_index;
                if (accepted[weight_index] != 0 &&
                    proposals[proposal_index].target_slot_mask.at(batch, slot)) {
                    denominator += weights[weight_index];
                }
            }
            if (unresolved[static_cast<std::size_t>(batch * shape.slots + slot)] != 0) {
                continue;
            }
            const double divisor = std::max(denominator, 1e-12);
            for (std::uint64_t dimension = 0; dimension < shape.dimension; ++dimension) {
                const auto element = offset(shape, batch, slot, dimension);
                double numerator = 0.0;
                for (std::size_t proposal_index = 0; proposal_index < proposal_count; ++proposal_index) {
                    const auto weight_index = static_cast<std::size_t>(batch) * proposal_count + proposal_index;
                    if (accepted[weight_index] != 0 &&
                        proposals[proposal_index].target_slot_mask.at(batch, slot)) {
                        numerator += bounded[proposal_index * slot_elements + element] *
                                     weights[weight_index];
                    }
                }
                result[element] = numerator / divisor;
            }
        }

        double squared_world_norm = 0.0;
        for (std::uint64_t slot = 0; slot < shape.slots; ++slot) {
            for (std::uint64_t dimension = 0; dimension < shape.dimension; ++dimension) {
                const double value = result[offset(shape, batch, slot, dimension)];
                squared_world_norm += value * value;
            }
        }
        const double world_norm = std::sqrt(squared_world_norm);
        const double scale = std::min(1.0, maximum_world_delta / std::max(world_norm, 1e-12));
        for (std::uint64_t slot = 0; slot < shape.slots; ++slot) {
            for (std::uint64_t dimension = 0; dimension < shape.dimension; ++dimension) {
                result[offset(shape, batch, slot, dimension)] *= scale;
            }
        }
    }

    std::vector<std::uint8_t> dirty(slot_count, 0);
    for (std::uint64_t batch = 0; batch < shape.batch; ++batch) {
        for (std::uint64_t slot = 0; slot < shape.slots; ++slot) {
            for (std::uint64_t dimension = 0; dimension < shape.dimension; ++dimension) {
                if (std::abs(result[offset(shape, batch, slot, dimension)]) > 0.0) {
                    dirty[static_cast<std::size_t>(batch * shape.slots + slot)] = 1;
                    break;
                }
            }
        }
    }

    return {Tensor(shape.dtype, {shape.batch, shape.slots, shape.dimension}, std::move(result), shape.device),
            Tensor(shape.dtype, {shape.batch, static_cast<std::uint64_t>(proposal_count)},
                   std::move(weights), shape.device),
            BooleanMask({shape.batch, static_cast<std::uint64_t>(proposal_count)}, std::move(accepted)),
            BooleanMask({shape.batch, shape.slots}, std::move(unresolved)), std::move(dirty),
            std::move(sources)};
}

[[nodiscard]] Tensor add_tensor(const Tensor& state, const std::span<const double> delta,
                                const std::size_t delta_offset = 0) {
    std::vector<double> values(state.values().begin(), state.values().end());
    for (std::size_t index = 0; index < values.size(); ++index) {
        values[index] += delta[delta_offset + index];
    }
    return Tensor(state.dtype(), std::vector<std::uint64_t>(state.shape().begin(), state.shape().end()),
                  std::move(values), std::string(state.device()));
}

[[nodiscard]] Tensor add_cognitive_slots(const Tensor& state,
                                         const std::span<const double> combined_delta,
                                         const std::uint64_t combined_slots,
                                         const std::uint64_t first_slot) {
    const auto shape = state.shape();
    std::vector<double> values(state.values().begin(), state.values().end());
    for (std::uint64_t batch = 0; batch < shape[0]; ++batch) {
        for (std::uint64_t slot = 0; slot < shape[1]; ++slot) {
            for (std::uint64_t dimension = 0; dimension < shape[2]; ++dimension) {
                const auto local = static_cast<std::size_t>(
                    (batch * shape[1] + slot) * shape[2] + dimension);
                const auto combined = static_cast<std::size_t>(
                    (batch * combined_slots + first_slot + slot) * shape[2] + dimension);
                values[local] += combined_delta[combined];
            }
        }
    }
    return Tensor(state.dtype(), std::vector<std::uint64_t>(shape.begin(), shape.end()),
                  std::move(values), std::string(state.device()));
}

}  // namespace

void SynapseProposal::validate(const WorldState& world) const {
    validate_proposal(*this, slot_shape(world));
}

void SynapseProposal::validate(const CognitiveState& world) const {
    validate_proposal(*this, slot_shape(world));
}

SynapseProposal sufficiency_gated_proposal(const WorldState& world,
                                           const SynapseProposal& proposal,
                                           const BooleanMask& sufficient_mask) {
    return sufficiency_gate(world, proposal, sufficient_mask);
}

SynapseProposal sufficiency_gated_proposal(const CognitiveState& world,
                                           const SynapseProposal& proposal,
                                           const BooleanMask& sufficient_mask) {
    return sufficiency_gate(world, proposal, sufficient_mask);
}

SingleWorldArbiter::SingleWorldArbiter(const double maximum_slot_delta,
                                       const double maximum_world_delta,
                                       const double minimum_weight)
    : maximum_slot_delta_(maximum_slot_delta), maximum_world_delta_(maximum_world_delta),
      minimum_weight_(minimum_weight) {
    if (!std::isfinite(maximum_slot_delta_) || !std::isfinite(maximum_world_delta_) ||
        std::min(maximum_slot_delta_, maximum_world_delta_) <= 0.0) {
        throw std::invalid_argument("delta limits must be positive");
    }
    if (!std::isfinite(minimum_weight_) || minimum_weight_ < 0.0 || minimum_weight_ > 1.0) {
        throw std::invalid_argument("minimum weight must be in [0, 1]");
    }
}

ArbitrationResult<WorldState> SingleWorldArbiter::operator()(
    std::shared_ptr<const WorldState> world, const std::span<const SynapseProposal> proposals,
    const bool commit) const {
    if (!world) throw std::invalid_argument("World State must not be null");
    auto reduction = reduce(*world, proposals, maximum_slot_delta_, maximum_world_delta_, minimum_weight_);
    const bool dirty = std::any_of(reduction.dirty.begin(), reduction.dirty.end(),
                                   [](const auto value) { return value != 0; });
    std::shared_ptr<const WorldState> output = world;
    if (commit && dirty) {
        std::vector<std::uint8_t> active(world->active_mask().values().begin(),
                                         world->active_mask().values().end());
        std::vector<std::uint8_t> dirty_mask(world->dirty_mask().values().begin(),
                                             world->dirty_mask().values().end());
        for (std::size_t index = 0; index < reduction.dirty.size(); ++index) {
            active[index] = static_cast<std::uint8_t>(active[index] != 0 || reduction.dirty[index] != 0);
            dirty_mask[index] = static_cast<std::uint8_t>(dirty_mask[index] != 0 || reduction.dirty[index] != 0);
        }
        const auto shape = slot_shape(*world);
        output = std::make_shared<const WorldState>(
            add_tensor(world->semantic_slots(), reduction.proposed_delta.values()),
            BooleanMask({shape.batch, shape.slots}, std::move(active)),
            BooleanMask({shape.batch, shape.slots}, std::move(dirty_mask)),
            std::string(world->source()) + "+synapse_arbiter",
            std::vector<SurfaceResidualRef>(world->surface_refs().begin(), world->surface_refs().end()));
    }
    return ArbitrationResult<WorldState>(std::move(output), commit && dirty,
                                         std::move(reduction.proposed_delta),
                                         std::move(reduction.proposal_weights),
                                         std::move(reduction.accepted),
                                         std::move(reduction.unresolved),
                                         std::move(reduction.sources));
}

ArbitrationResult<CognitiveState> SingleWorldArbiter::operator()(
    std::shared_ptr<const CognitiveState> world, const std::span<const SynapseProposal> proposals,
    const bool commit) const {
    if (!world) throw std::invalid_argument("Cognitive State must not be null");
    auto reduction = reduce(*world, proposals, maximum_slot_delta_, maximum_world_delta_, minimum_weight_);
    const bool dirty = std::any_of(reduction.dirty.begin(), reduction.dirty.end(),
                                   [](const auto value) { return value != 0; });
    std::shared_ptr<const CognitiveState> output = world;
    if (commit && dirty) {
        const auto semantic_slots = world->semantic_slots().shape()[1];
        const auto executive_slots = world->executive_slots().shape()[1];
        const auto combined_slots = semantic_slots + executive_slots + world->scratch_slots().shape()[1];
        const auto delta = reduction.proposed_delta.values();
        output = std::make_shared<const CognitiveState>(
            add_cognitive_slots(world->semantic_slots(), delta, combined_slots, 0),
            add_cognitive_slots(world->executive_slots(), delta, combined_slots, semantic_slots),
            add_cognitive_slots(world->scratch_slots(), delta, combined_slots,
                                semantic_slots + executive_slots),
            world->structured_world_graph(),
            std::vector<std::string>(world->evidence_refs().begin(), world->evidence_refs().end()),
            world->goal_state(), world->value_state(), world->self_state(), std::string(world->owner_id()));
    }
    return ArbitrationResult<CognitiveState>(std::move(output), commit && dirty,
                                             std::move(reduction.proposed_delta),
                                             std::move(reduction.proposal_weights),
                                             std::move(reduction.accepted),
                                             std::move(reduction.unresolved),
                                             std::move(reduction.sources));
}

}  // namespace swegca::world
