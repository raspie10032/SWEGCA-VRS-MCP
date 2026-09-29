#include "world/vrs_event_signal.hpp"

#include <array>
#include <bit>
#include <cassert>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <type_traits>

using namespace swegca::world;

namespace {

EventSignalInputs fixture() {
    return EventSignalInputs(
        std::string(64, 'a'), {0.1F, 0.8F, -0.7F}, {0.1F, 0.8F, -0.7F},
        {{0, 1, 1, 1.0F}, {0, 2, -1, 0.75F}}, {1.0F, 0.75F}, {0, 0, 0});
}

std::shared_ptr<const DetachedVrsStateUpdateReceipt> receipt() {
    const std::string snapshot(64, 'a');
    std::vector<VrsConnectionStateUpdate> updates;
    updates.emplace_back(
        "vrs-edge:0", "vrs-edge:0", "vrs-edge:0", CurrentEvidenceVerdict::support,
        1.0, 1.01, "reinforce",
        assess_vrs_experience_promotion(snapshot, "vrs-edge:0", 1.0, 1.01));
    updates.emplace_back(
        "vrs-edge:1", "vrs-edge:1", "vrs-edge:1", CurrentEvidenceVerdict::refute,
        0.75, 0.75 * 0.995, "weaken",
        assess_vrs_experience_promotion(snapshot, "vrs-edge:1", 0.75, 0.75 * 0.995));
    return std::make_shared<const DetachedVrsStateUpdateReceipt>(snapshot, std::move(updates));
}

std::uint32_t bits(const float value) { return std::bit_cast<std::uint32_t>(value); }

bool rejects(const auto& operation) {
    try { operation(); }
    catch (const std::invalid_argument&) { return true; }
    return false;
}

}  // namespace

int main() {
    static_assert(!std::is_copy_assignable_v<EventSignalProposal>);
    static_assert(!std::is_move_assignable_v<EventSignalProposal>);
    assert(vrs_event_signal_source_sha256 ==
           "f851084f095a539a8098ce3f94a366a7ec91defcc040b7c2ce9db5ba305a51b1");
    auto base = fixture();
    auto update = receipt();
    const std::array<std::size_t, 0> no_seeds{};
    const auto waiting = settle_event_signal(
        base, no_seeds, update, "vrs-edge:", nullptr, 0);
    assert((waiting.pending_nodes == std::vector<std::size_t>{1, 2}));
    assert(waiting.rounds == 0 && waiting.node_evaluations == 0 && waiting.edge_evaluations == 0);
    assert(waiting.strengths.size() == 2);
    assert(bits(waiting.strengths.at(0)) == 0x3f8147aeU);
    assert(bits(waiting.strengths.at(1)) == 0x3f3f0a3dU);

    struct Expected final {
        std::uint64_t limit;
        std::uint32_t one;
        std::uint32_t two;
        std::uint64_t rounds;
        std::uint64_t nodes;
        std::uint64_t reads;
        bool pending;
    };
    for (const auto expected : {
             Expected{1, 0x3f466751U, 0xbf2ec8c8U, 1, 2, 2, true},
             Expected{2, 0x3f414954U, 0xbf2b4072U, 2, 4, 4, true},
             Expected{20, 0x3f2d2fc6U, 0xbf1d6045U, 20, 40, 40, true},
             Expected{1000, 0x3f2cd163U, 0xbf1d1f1dU, 62, 123, 123, false},
         }) {
        const auto result = settle_event_signal(
            base, no_seeds, update, "vrs-edge:", nullptr, expected.limit);
        assert(bits(result.scores.at(1)) == expected.one);
        assert(bits(result.scores.at(2)) == expected.two);
        assert(result.rounds == expected.rounds);
        assert(result.node_evaluations == expected.nodes);
        assert(result.edge_evaluations == expected.reads);
        assert(result.pending() == expected.pending);
        assert(result.strength_receipt == update);
        assert(result.input_strength_proposal_count() == 2);
        assert(!result.authority_granted() && !result.persistent_state_mutated());
        assert(!result.whole_graph_convergence_claimed() && !result.cognitive_completion());
    }

    const auto first = settle_event_signal(base, no_seeds, update, "vrs-edge:", nullptr, 2);
    const auto resumed = settle_event_signal(
        base, no_seeds, {}, "vrs-edge:", &first, 998);
    const auto once = settle_event_signal(base, no_seeds, update, "vrs-edge:", nullptr, 1000);
    assert(resumed.scores == once.scores && resumed.strengths == once.strengths);
    assert(resumed.pending_nodes == once.pending_nodes && resumed.rounds == once.rounds);
    assert(first.rounds == 2 && first.pending());
    assert(rejects([&] {
        (void)settle_event_signal(base, no_seeds, update, "vrs-edge:", &first, 1);
    }));

    const auto idle = settle_event_signal(base, no_seeds, {}, "vrs-edge:", nullptr, 1000);
    assert(idle.rounds == 0 && idle.scores.empty() && idle.strengths.empty() && !idle.pending());
    const std::array<std::size_t, 1> seed{0};
    const auto budgeted = settle_event_signal(base, seed, {}, "vrs-edge:", nullptr, 0);
    assert((budgeted.pending_nodes == std::vector<std::size_t>{0}));

    EventSignalInputs disconnected(
        std::string(64, 'a'), {0.5F, 0.5F, 0.5F, 0.5F},
        {0.0F, 0.0F, 0.0F, 0.0F},
        {{0, 1, 1, 0.7F}, {2, 3, -1, 0.8F}}, {1.0F, 1.0F}, {0, 1, 0, 1});
    const std::array<std::size_t, 3> repeated_seeds{0, 0, 0};
    const auto local = settle_event_signal(
        disconnected, repeated_seeds, {}, "vrs-edge:", nullptr, 5);
    assert(!local.scores.contains(2) && !local.scores.contains(3));
    assert(local.strengths.empty());
    assert((local.seed_nodes == std::vector<std::size_t>{0}));

    EventSignalInputs signed_zero(
        std::string(64, 'a'), {0.0F}, {-0.0F}, {}, {}, {0});
    const auto zero = settle_event_signal(
        signed_zero, seed, {}, "vrs-edge:", nullptr, 3);
    assert(!zero.pending() && zero.scores.contains(0));
    assert(bits(zero.scores.at(0)) == 0U && bits(signed_zero.score[0]) == 0x80000000U);

    const auto conflict_receipt = std::make_shared<const DetachedVrsStateUpdateReceipt>(
        std::string(64, 'a'), std::vector<VrsConnectionStateUpdate>{
            VrsConnectionStateUpdate(
                "vrs-edge:0", "vrs-edge:0", "p", CurrentEvidenceVerdict::support,
                1.0, 1.0, "abstain_conflict",
                assess_vrs_experience_promotion(std::string(64, 'a'), "vrs-edge:0", 1.0, 1.0))});
    const auto conflict = settle_event_signal(
        base, no_seeds, conflict_receipt, "vrs-edge:", nullptr, 100);
    assert(conflict.strengths.empty() && conflict.rounds == 0);

    const auto half = settle_event_signal(
        base, no_seeds, update, "vrs-edge:", nullptr, 0,
        EventStrengthStorage::float16);
    assert(bits(half.strengths.at(0)) == 0x3f814000U);
    assert(bits(half.strengths.at(1)) == 0x3f3f0000U);
    const auto half_resumed = settle_event_signal(
        base, no_seeds, {}, "vrs-edge:", &half, 1000);
    assert(half_resumed.strength_storage == EventStrengthStorage::float16);

    EventSignalInputs crossing(
        std::string(64, 'a'), {0.0F}, {0.0F}, {{0, 0, 1, 0.99F}}, {0.99F}, {0});
    const double old = static_cast<double>(crossing.strength[0]);
    const double changed = old * legacy_vrs_stable_reinforcement_factor;
    const auto crossing_receipt = std::make_shared<const DetachedVrsStateUpdateReceipt>(
        std::string(64, 'a'), std::vector<VrsConnectionStateUpdate>{
            VrsConnectionStateUpdate(
                "vrs-edge:0", "vrs-edge:0", "p", CurrentEvidenceVerdict::support,
                old, changed, "reinforce",
                assess_vrs_experience_promotion(
                    std::string(64, 'a'), "vrs-edge:0", old, changed))});
    assert(rejects([&] {
        (void)settle_event_signal(
            crossing, no_seeds, crossing_receipt, "vrs-edge:", nullptr, 0,
            EventStrengthStorage::float16);
    }));

    const auto stale = std::make_shared<const DetachedVrsStateUpdateReceipt>(
        std::string(64, 'b'), std::vector<VrsConnectionStateUpdate>{});
    assert(rejects([&] {
        (void)settle_event_signal(base, no_seeds, stale, "vrs-edge:", nullptr, 1);
    }));
    const auto duplicate = std::make_shared<const DetachedVrsStateUpdateReceipt>(
        std::string(64, 'a'), std::vector<VrsConnectionStateUpdate>{
            VrsConnectionStateUpdate(
                "a", "vrs-edge:0", "a", CurrentEvidenceVerdict::support, 1.0, 1.01,
                "reinforce", assess_vrs_experience_promotion(std::string(64, 'a'), "vrs-edge:0", 1.0, 1.01)),
            VrsConnectionStateUpdate(
                "b", "vrs-edge:00", "b", CurrentEvidenceVerdict::support, 1.0, 1.01,
                "reinforce", assess_vrs_experience_promotion(std::string(64, 'a'), "vrs-edge:00", 1.0, 1.01))});
    assert(rejects([&] {
        (void)settle_event_signal(base, no_seeds, duplicate, "vrs-edge:", nullptr, 1);
    }));

    std::cout << "VRS event signal tests passed\n";
}
