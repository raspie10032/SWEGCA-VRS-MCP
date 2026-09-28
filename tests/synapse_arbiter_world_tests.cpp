#include "world/synapse_arbiter.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using swegca::world::BooleanMask;
using swegca::world::CognitiveState;
using swegca::world::SingleWorldArbiter;
using swegca::world::SynapseProposal;
using swegca::world::Tensor;
using swegca::world::TensorDType;
using swegca::world::WorldState;
using swegca::world::evidence_delta_proposal;
using swegca::world::state_slot_tensor;
using swegca::world::sufficiency_gated_proposal;

void require(const bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template <typename Callable>
void require_invalid_argument(Callable&& callable, const char* message) {
    try {
        callable();
    } catch (const std::invalid_argument&) {
        return;
    }
    throw std::runtime_error(message);
}

Tensor zeros(const std::vector<std::uint64_t>& shape) {
    std::size_t count = 1;
    for (const auto value : shape) count *= static_cast<std::size_t>(value);
    return Tensor(TensorDType::float32, shape, std::vector<double>(count, 0.0));
}

WorldState world() {
    return WorldState(zeros({2, 32, 4}), BooleanMask({2, 32}, std::vector<std::uint8_t>(64, 0)),
                      BooleanMask({2, 32}, std::vector<std::uint8_t>(64, 0)), "test");
}

CognitiveState cognitive() {
    return CognitiveState(zeros({2, 20, 4}), zeros({2, 6, 4}), zeros({2, 6, 4}));
}

SynapseProposal proposal(const WorldState& state, std::string source, const double value,
                         const double confidence = 1.0, const double contradiction = 0.0,
                         const double uncertainty = 0.0, const std::uint64_t slot = 30) {
    const auto shape = state.semantic_slots().shape();
    std::vector<double> delta(state.semantic_slots().values().size(), 0.0);
    std::vector<std::uint8_t> mask(static_cast<std::size_t>(shape[0] * shape[1]), 0);
    for (std::uint64_t batch = 0; batch < shape[0]; ++batch) {
        mask[static_cast<std::size_t>(batch * shape[1] + slot)] = 1;
        const auto start = static_cast<std::size_t>((batch * shape[1] + slot) * shape[2]);
        for (std::uint64_t dimension = 0; dimension < shape[2]; ++dimension) {
            delta[start + static_cast<std::size_t>(dimension)] = value;
        }
    }
    return {std::move(source),
            Tensor(TensorDType::float32, {shape[0], shape[1], shape[2]}, std::move(delta)),
            std::vector<double>(static_cast<std::size_t>(shape[0]), confidence),
            std::vector<double>(static_cast<std::size_t>(shape[0]), contradiction),
            std::vector<double>(static_cast<std::size_t>(shape[0]), uncertainty),
            BooleanMask({shape[0], shape[1]}, std::move(mask)), {}, "hypothesis"};
}

SynapseProposal cognitive_proposal(const CognitiveState& state, std::string source,
                                   const double value, const std::uint64_t slot = 30) {
    const auto semantic = state.semantic_slots().shape();
    const auto slots = semantic[1] + state.executive_slots().shape()[1] + state.scratch_slots().shape()[1];
    std::vector<double> delta(static_cast<std::size_t>(semantic[0] * slots * semantic[2]), 0.0);
    std::vector<std::uint8_t> mask(static_cast<std::size_t>(semantic[0] * slots), 0);
    for (std::uint64_t batch = 0; batch < semantic[0]; ++batch) {
        mask[static_cast<std::size_t>(batch * slots + slot)] = 1;
        const auto start = static_cast<std::size_t>((batch * slots + slot) * semantic[2]);
        for (std::uint64_t dimension = 0; dimension < semantic[2]; ++dimension) {
            delta[start + static_cast<std::size_t>(dimension)] = value;
        }
    }
    return {std::move(source), Tensor(TensorDType::float32, {semantic[0], slots, semantic[2]},
                                     std::move(delta)),
            std::vector<double>(static_cast<std::size_t>(semantic[0]), 1.0),
            std::vector<double>(static_cast<std::size_t>(semantic[0]), 0.0),
            std::vector<double>(static_cast<std::size_t>(semantic[0]), 0.0),
            BooleanMask({semantic[0], slots}, std::move(mask)), {}, "hypothesis"};
}

void test_preview_and_caps() {
    const auto state = std::make_shared<const WorldState>(world());
    const std::vector proposals{proposal(*state, "large", 100.0)};
    const SingleWorldArbiter arbiter(0.1, 0.08, 0.25);
    const auto preview = arbiter(state, proposals, false);
    require(preview.world_state() == state, "preview did not preserve input identity");
    require(!preview.committed(), "preview committed");
    require(state->semantic_slots().values()[0] == 0.0, "preview mutated input");

    double slot_squared = 0.0;
    double world_squared = 0.0;
    for (std::uint64_t dimension = 0; dimension < 4; ++dimension) {
        const double value = preview.proposed_delta().values()[static_cast<std::size_t>(30 * 4 + dimension)];
        slot_squared += value * value;
        world_squared += value * value;
    }
    require(std::sqrt(slot_squared) <= 0.1000001, "slot L2 cap failed");
    require(std::sqrt(world_squared) <= 0.0800001, "world L2 cap failed");

    const auto committed = arbiter(state, proposals, true);
    require(committed.committed(), "nonzero proposal did not commit");
    require(committed.world_state() != state, "commit reused input object");
    require(committed.world_state()->active_mask().at(0, 30), "commit did not activate slot");
    require(committed.world_state()->dirty_mask().at(0, 30), "commit did not dirty slot");
    require(state->semantic_slots().values()[0] == 0.0, "commit mutated input");
}

void test_opposition_and_same_source() {
    const auto state = std::make_shared<const WorldState>(world());
    const SingleWorldArbiter arbiter;
    const std::vector opposed{proposal(*state, "positive", 1.0),
                              proposal(*state, "negative", -0.7)};
    const auto conflict = arbiter(state, opposed, true);
    require(conflict.unresolved_contradiction().at(0, 30), "independent opposition unresolved flag missing");
    require(!conflict.committed(), "independent opposition committed");
    for (const auto value : conflict.proposed_delta().values()) {
        require(value == 0.0, "independent opposition left residual delta");
    }

    const std::vector same_source{proposal(*state, "same", 1.0), proposal(*state, "same", -0.7)};
    const auto related = arbiter(state, same_source, false);
    require(!related.unresolved_contradiction().at(0, 30), "same source was treated as independent conflict");
}

void test_inclusive_weight_uncertainty_and_sufficiency() {
    const auto state = std::make_shared<const WorldState>(world());
    const SingleWorldArbiter arbiter(0.1, 0.5, 0.25);
    const std::vector inclusive{proposal(*state, "inclusive", 1.0, 0.5, 0.0, 0.5)};
    const auto accepted = arbiter(state, inclusive, false);
    require(accepted.accepted().at(0, 0), "minimum weight was not inclusive");

    const std::vector uncertain{proposal(*state, "uncertain", 1.0, 1.0, 0.0, 1.0)};
    const auto rejected = arbiter(state, uncertain, true);
    require(!rejected.accepted().at(0, 0), "fully uncertain proposal was accepted");
    require(!rejected.committed(), "fully uncertain proposal committed");

    auto base = proposal(*state, "sufficiency", 1.0);
    const auto gated = sufficiency_gated_proposal(
        *state, base, BooleanMask({2}, std::vector<std::uint8_t>{1, 0}));
    const std::vector gated_proposals{gated};
    const auto gated_result = arbiter(state, gated_proposals, false);
    require(gated_result.accepted().at(0, 0), "sufficient batch was rejected");
    require(!gated_result.accepted().at(1, 0), "insufficient batch did not fail closed");
}

void test_weighted_reduction() {
    const auto state = std::make_shared<const WorldState>(world());
    const SingleWorldArbiter arbiter(10.0, 100.0, 0.25);
    const std::vector proposals{proposal(*state, "lighter", 1.0, 0.5),
                                proposal(*state, "heavier", 3.0, 1.0)};
    const auto result = arbiter(state, proposals, false);
    const auto first_target = static_cast<std::size_t>(30 * 4);
    require(std::abs(result.proposed_delta().values()[first_target] - (3.5 / 1.5)) < 1e-12,
            "weighted proposal reduction changed");
}

void test_empty_and_zero_delta() {
    const auto state = std::make_shared<const WorldState>(world());
    const SingleWorldArbiter arbiter;
    const std::vector<SynapseProposal> empty;
    const auto empty_result = arbiter(state, empty, true);
    require(empty_result.world_state() == state, "empty proposals changed state identity");
    require(!empty_result.committed(), "empty proposals committed");
    require(empty_result.proposal_weights().shape()[1] == 0, "empty proposal weight shape mismatch");

    const std::vector zero{proposal(*state, "zero", 0.0)};
    const auto zero_result = arbiter(state, zero, true);
    require(zero_result.accepted().at(0, 0), "zero delta proposal acceptance changed");
    require(!zero_result.committed(), "zero delta committed");
    require(zero_result.world_state() == state, "zero delta created a state");
}

void test_cognitive_split_commit() {
    const auto state = std::make_shared<const CognitiveState>(cognitive());
    const std::vector proposals{cognitive_proposal(*state, "visual", 1.0)};
    const SingleWorldArbiter arbiter;
    const auto preview = arbiter(state, proposals, false);
    require(preview.world_state() == state, "cognitive preview did not preserve identity");
    const auto committed = arbiter(state, proposals, true);
    require(committed.committed(), "cognitive proposal did not commit");
    require(committed.world_state()->semantic_slots().exact_equal(state->semantic_slots()),
            "cognitive semantic split changed");
    require(committed.world_state()->executive_slots().exact_equal(state->executive_slots()),
            "cognitive executive split changed");
    require(!committed.world_state()->scratch_slots().exact_equal(state->scratch_slots()),
            "cognitive scratch target did not change");
    const auto scratch = committed.world_state()->scratch_slots().values();
    for (std::uint64_t batch = 0; batch < 2; ++batch) {
        for (std::uint64_t slot = 0; slot < 6; ++slot) {
            const bool target = slot == 4;
            for (std::uint64_t dimension = 0; dimension < 4; ++dimension) {
                const auto index = static_cast<std::size_t>((batch * 6 + slot) * 4 + dimension);
                require((scratch[index] != 0.0) == target, "cognitive split changed wrong scratch slot");
            }
        }
    }
}

void test_nonfinite_configuration_is_rejected() {
    const auto nan = std::numeric_limits<double>::quiet_NaN();
    const auto infinity = std::numeric_limits<double>::infinity();
    require_invalid_argument([&] { static_cast<void>(SingleWorldArbiter(nan, 0.5, 0.25)); },
                             "NaN slot limit was accepted");
    require_invalid_argument([&] { static_cast<void>(SingleWorldArbiter(0.1, infinity, 0.25)); },
                             "infinite world limit was accepted");
    require_invalid_argument([&] { static_cast<void>(SingleWorldArbiter(0.1, 0.5, nan)); },
                             "NaN minimum weight was accepted");
    require_invalid_argument([&] { static_cast<void>(SingleWorldArbiter(0.1, 0.5, infinity)); },
                             "infinite minimum weight was accepted");
}

void test_malformed_state_and_device_fail_closed() {
    const auto malformed_active = std::make_shared<const WorldState>(
        zeros({2, 32, 4}), BooleanMask({1, 32}, std::vector<std::uint8_t>(32, 0)),
        BooleanMask({2, 32}, std::vector<std::uint8_t>(64, 0)), "malformed");
    const std::vector<SynapseProposal> empty;
    require_invalid_argument(
        [&] { static_cast<void>(SingleWorldArbiter()(malformed_active, empty, false)); },
        "malformed World State mask reached reduction");

    const auto state = std::make_shared<const WorldState>(world());
    auto mismatched = proposal(*state, "wrong-device", 1.0);
    mismatched.delta_candidate = Tensor(
        mismatched.delta_candidate.dtype(),
        std::vector<std::uint64_t>(mismatched.delta_candidate.shape().begin(),
                                   mismatched.delta_candidate.shape().end()),
        std::vector<double>(mismatched.delta_candidate.values().begin(),
                            mismatched.delta_candidate.values().end()),
        "cuda:0");
    const std::vector proposals{mismatched};
    require_invalid_argument(
        [&] { static_cast<void>(SingleWorldArbiter()(state, proposals, false)); },
        "proposal delta device mismatch was accepted");

    const std::shared_ptr<const WorldState> null_state;
    require_invalid_argument(
        [&] { static_cast<void>(SingleWorldArbiter()(null_state, empty, false)); },
        "null shared World State was accepted");
}

void test_preview_owns_shared_input_lifetime() {
    auto state = std::make_shared<const WorldState>(world());
    const auto original_address = state.get();
    const std::vector<SynapseProposal> empty;
    auto preview = SingleWorldArbiter()(state, empty, false);
    require(preview.world_state() == state, "preview changed shared pointer identity");
    state.reset();
    require(preview.world_state().get() == original_address,
            "preview did not retain shared input lifetime");
    require(preview.world_state()->source() == "test", "retained preview state is unusable");
}

void test_public_state_slot_tensor() {
    const auto world_state = std::make_shared<const WorldState>(world());
    const auto world_slots = state_slot_tensor(world_state);
    require(world_slots.exact_equal(world_state->semantic_slots()),
            "WorldState slot tensor changed values");

    const auto cognitive_state = std::make_shared<const CognitiveState>(
        Tensor(TensorDType::float32, {1, 2, 2}, {1.0, 2.0, 3.0, 4.0}),
        Tensor(TensorDType::float32, {1, 1, 2}, {5.0, 6.0}),
        Tensor(TensorDType::float32, {1, 1, 2}, {7.0, 8.0}));
    const auto cognitive_slots = state_slot_tensor(cognitive_state);
    require(cognitive_slots.shape()[0] == 1 && cognitive_slots.shape()[1] == 4 &&
                cognitive_slots.shape()[2] == 2,
            "CognitiveState combined slot shape changed");
    const std::vector<double> expected{1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0};
    require(std::equal(cognitive_slots.values().begin(), cognitive_slots.values().end(),
                       expected.begin(), expected.end()),
            "CognitiveState slot order changed");
}

void test_evidence_delta_world_proposal() {
    const auto state = std::make_shared<const WorldState>(world());
    const Tensor evidence_delta(TensorDType::float32, {2, 4},
                                {1.0, 2.0, 3.0, 4.0, -1.0, -2.0, -3.0, -4.0});
    const Tensor logits(TensorDType::float32, {2, 2}, {8.0, -8.0, -8.0, 8.0});
    const std::vector<std::vector<std::string>> addresses{{"clip:1"}, {"clip:2", "text:2"}};
    const auto result = evidence_delta_proposal(state, evidence_delta, logits, "text-video",
                                                addresses, "hypothesis:1");
    require(result.source == "text-video", "proposal source changed");
    require(result.evidence_addresses == addresses, "proposal evidence addresses changed");
    require(result.hypothesis_id == "hypothesis:1", "proposal hypothesis changed");
    require(result.target_slot_mask.at(0, 30) && result.target_slot_mask.at(1, 30),
            "verification role did not map to slot 30");
    require(result.confidence[0] > result.confidence[1], "softmax confidence order changed");
    require(result.contradiction[0] < result.contradiction[1],
            "softmax contradiction order changed");
    for (std::uint64_t batch = 0; batch < 2; ++batch) {
        for (std::uint64_t slot = 0; slot < 32; ++slot) {
            for (std::uint64_t dimension = 0; dimension < 4; ++dimension) {
                const auto index = static_cast<std::size_t>((batch * 32 + slot) * 4 + dimension);
                const auto evidence_index = static_cast<std::size_t>(batch * 4 + dimension);
                const double expected = slot == 30 ? evidence_delta.values()[evidence_index] : 0.0;
                require(result.delta_candidate.values()[index] == expected,
                        "evidence delta escaped target slot");
            }
        }
    }

    const Tensor uniform_logits(TensorDType::float32, {2, 2}, {0.0, 0.0, 0.0, 0.0});
    const auto uniform = evidence_delta_proposal(
        state, evidence_delta, uniform_logits, "uniform", {}, {}, std::int64_t{31});
    require(std::abs(uniform.confidence[0] - 0.5) < 1e-12,
            "binary softmax probability changed");
    require(std::abs(uniform.uncertainty[0] - 1.0) < 1e-12,
            "normalized binary entropy changed");
    require(uniform.target_slot_mask.at(0, 31), "numeric target slot changed");
    const auto global = evidence_delta_proposal(
        state, evidence_delta, uniform_logits, "global", {}, {}, std::string("global"));
    require(global.target_slot_mask.at(0, 31), "fixed global role did not map to slot 31");
}

void test_evidence_delta_cognitive_proposal() {
    const auto state = std::make_shared<const CognitiveState>(cognitive());
    const Tensor evidence_delta(TensorDType::float32, {2, 4},
                                std::vector<double>(8, 1.0));
    const Tensor logits(TensorDType::float32, {2, 3},
                        {4.0, 1.0, -2.0, 4.0, 1.0, -2.0});
    const auto result = evidence_delta_proposal(
        state, evidence_delta, logits, "cognitive", {{"a"}, {"b"}}, "h",
        std::string("verification"), 0, 1);
    require(result.delta_candidate.shape()[1] == 32,
            "CognitiveState proposal did not use combined slots");
    require(result.target_slot_mask.at(0, 30),
            "CognitiveState verification role did not map to combined slot 30");
    require(result.confidence[0] > result.contradiction[0],
            "multiclass softmax class selection changed");
    require(result.uncertainty[0] >= 0.0 && result.uncertainty[0] <= 1.0,
            "multiclass normalized entropy left [0,1]");
}

void test_evidence_delta_rejects_invalid_shapes_classes_and_roles() {
    const auto state = std::make_shared<const WorldState>(world());
    const Tensor delta(TensorDType::float32, {2, 4}, std::vector<double>(8, 1.0));
    const Tensor logits(TensorDType::float32, {2, 2}, {1.0, 0.0, 1.0, 0.0});
    require_invalid_argument(
        [&] {
            static_cast<void>(evidence_delta_proposal(
                state, Tensor(TensorDType::float32, {2, 1, 4}, std::vector<double>(8, 1.0)),
                logits, "bad-delta"));
        },
        "rank-3 evidence delta was accepted");
    require_invalid_argument(
        [&] {
            static_cast<void>(evidence_delta_proposal(
                state, delta, Tensor(TensorDType::float32, {4}, std::vector<double>(4, 0.0)),
                "bad-logits"));
        },
        "rank-1 evidence logits were accepted");
    require_invalid_argument(
        [&] {
            static_cast<void>(evidence_delta_proposal(
                state, delta,
                Tensor(TensorDType::float32, {1, 2}, std::vector<double>(2, 0.0)),
                "bad-logit-batch"));
        },
        "wrong evidence logits batch was accepted");
    for (const auto classes : {std::pair{-1, 1}, std::pair{0, -1}, std::pair{2, 1},
                               std::pair{0, 2}}) {
        require_invalid_argument(
            [&] {
                static_cast<void>(evidence_delta_proposal(
                    state, delta, logits, "bad-class", {}, {}, std::string("verification"),
                    classes.first, classes.second));
            },
            "invalid signed class index was accepted");
    }
    require_invalid_argument(
        [&] {
            static_cast<void>(evidence_delta_proposal(
                state, delta, logits, "negative-slot", {}, {}, std::int64_t{-1}));
        },
        "negative target slot was accepted");
    require_invalid_argument(
        [&] {
            static_cast<void>(evidence_delta_proposal(
                state, delta, logits, "outside-slot", {}, {}, std::int64_t{32}));
        },
        "outside target slot was accepted");
    require_invalid_argument(
        [&] {
            static_cast<void>(evidence_delta_proposal(
                state, delta, logits, "unknown-role", {}, {}, std::string("missing")));
        },
        "unknown target role was accepted");
}

}  // namespace

int main() {
    try {
        test_preview_and_caps();
        test_opposition_and_same_source();
        test_inclusive_weight_uncertainty_and_sufficiency();
        test_weighted_reduction();
        test_empty_and_zero_delta();
        test_cognitive_split_commit();
        test_nonfinite_configuration_is_rejected();
        test_malformed_state_and_device_fail_closed();
        test_preview_owns_shared_input_lifetime();
        test_public_state_slot_tensor();
        test_evidence_delta_world_proposal();
        test_evidence_delta_cognitive_proposal();
        test_evidence_delta_rejects_invalid_shapes_classes_and_roles();
        std::cout << "synapse arbiter world tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "synapse arbiter world tests failed: " << error.what() << '\n';
        return 1;
    }
}
