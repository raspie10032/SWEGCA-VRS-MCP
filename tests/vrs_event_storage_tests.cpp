#include "world/vrs_event_storage.hpp"
#include "world/vrs_event_delta.hpp"

#include <array>
#include <bit>
#include <cassert>
#include <cstring>
#include <iostream>
#include <memory>
#include <span>
#include <type_traits>
#include <vector>

using namespace swegca::world;

namespace {

std::vector<std::byte> bytes(const auto& values) {
    const auto view = std::as_bytes(std::span(values));
    return {view.begin(), view.end()};
}

std::shared_ptr<const EventSignalInputs> fixture() {
    return std::make_shared<const EventSignalInputs>(
        std::string(64, 'a'), std::vector<float>{0.1F, 0.8F, -0.7F},
        std::vector<float>{0.1F, 0.8F, -0.7F},
        std::vector<EventSignalEdge>{{0, 1, 1, 1.0F}, {0, 2, -1, 0.75F}},
        std::vector<float>{1.0F, 0.75F}, std::vector<std::uint8_t>{0, 0, 0});
}

std::shared_ptr<const DetachedVrsStateUpdateReceipt> receipt() {
    const std::string snapshot(64, 'a');
    std::vector<VrsConnectionStateUpdate> updates;
    updates.emplace_back(
        "fixture-observation", "vrs-edge:0", "fixture-proposition",
        CurrentEvidenceVerdict::support, 1.0, 1.01, "reinforce",
        assess_vrs_experience_promotion(snapshot, "vrs-edge:0", 1.0, 1.01));
    updates.emplace_back(
        "fixture-observation", "vrs-edge:1", "fixture-proposition",
        CurrentEvidenceVerdict::refute, 0.75, 0.75 * 0.995, "weaken",
        assess_vrs_experience_promotion(
            snapshot, "vrs-edge:1", 0.75, 0.75 * 0.995));
    return std::make_shared<const DetachedVrsStateUpdateReceipt>(
        snapshot, std::move(updates));
}

std::shared_ptr<const BoundEventSignalStorage> bind(
    const std::shared_ptr<const EventSignalInputs>& inputs) {
    const std::vector<std::size_t> score_shape{inputs->score.size()};
    const auto dense_score = inputs->score.materialize();
    const auto score_bytes = bytes(dense_score);
    auto scores = VrsArrayBlocks::prepare(
        {"<f4", score_shape, score_bytes}, {}, 64).array;
    std::vector<std::uint16_t> half;
    for (std::size_t index = 0; index < inputs->strength.size(); ++index) {
        const auto strength = inputs->strength[index];
        if (strength == 1.0F) half.push_back(0x3c00U);
        else if (strength == 0.75F) half.push_back(0x3a00U);
        else throw std::runtime_error("fixture strength is not encoded");
    }
    const std::vector<std::size_t> strength_shape{half.size()};
    const auto strength_bytes = bytes(half);
    auto strengths = VrsArrayBlocks::prepare(
        {"<f2", strength_shape, strength_bytes}, {}, 64).array;
    return BoundEventSignalStorage::cold_bind(inputs, scores, strengths);
}

std::vector<float> restore_float32(const VrsArrayBlocks& array) {
    const auto raw = array.restore();
    assert(raw.size() % sizeof(float) == 0);
    std::vector<float> result(raw.size() / sizeof(float));
    std::memcpy(result.data(), raw.data(), raw.size());
    return result;
}

std::vector<std::uint16_t> restore_float16(const VrsArrayBlocks& array) {
    const auto raw = array.restore();
    assert(raw.size() % sizeof(std::uint16_t) == 0);
    std::vector<std::uint16_t> result(raw.size() / sizeof(std::uint16_t));
    std::memcpy(result.data(), raw.data(), raw.size());
    return result;
}

bool rejects(const auto& operation) {
    try { operation(); }
    catch (const std::invalid_argument&) { return true; }
    return false;
}

void paired_storage_exact() {
    const auto inputs = fixture();
    const auto bound = bind(inputs);
    const auto original_scores = bound->scores;
    const auto original_strengths = bound->strengths;
    const auto update = receipt();
    const auto proposal = bound->settle({}, update, "vrs-edge:", nullptr, 1000);
    assert(!proposal.signal.pending());
    assert(proposal.signal.strength_receipt == update);
    assert(proposal.signal.strength_storage == EventStrengthStorage::float16);
    assert(proposal.signal.belongs_to(*inputs));
    assert(std::bit_cast<std::uint32_t>(proposal.signal.scores.at(1)) == 0x3f2cd163U);
    assert(std::bit_cast<std::uint32_t>(proposal.signal.scores.at(2)) == 0xbf1d1efcU);
    assert(std::bit_cast<std::uint32_t>(proposal.signal.strengths.at(0)) == 0x3f814000U);
    assert(std::bit_cast<std::uint32_t>(proposal.signal.strengths.at(1)) == 0x3f3f0000U);

    const auto stored = bound->prepare(proposal);
    const auto scores = restore_float32(*stored.scores);
    const auto strengths = restore_float16(*stored.strengths);
    assert(scores.size() == 3 && std::bit_cast<std::uint32_t>(scores[0]) == 0x3dcccccdU);
    assert(std::bit_cast<std::uint32_t>(scores[1]) == 0x3f2cd163U);
    assert(std::bit_cast<std::uint32_t>(scores[2]) == 0xbf1d1efcU);
    assert((strengths == std::vector<std::uint16_t>{0x3c0aU, 0x39f8U}));
    assert(stored.promotions.size() == 2);
    assert(stored.promotions[0].current_strength ==
           static_cast<double>(proposal.signal.strengths.at(0)));
    assert(stored.promotions[1].current_strength ==
           static_cast<double>(proposal.signal.strengths.at(1)));
    assert(!stored.authority_granted() && !stored.persistent_state_mutated() &&
           !stored.main_committed());
    assert(stored.diagnostics.scores.candidate_bytes_scanned == 8);
    assert(stored.diagnostics.strengths.candidate_bytes_scanned == 4);
    assert(bound->scores == original_scores && bound->strengths == original_strengths);
    assert(restore_float32(*bound->scores) == inputs->score.materialize());
    assert((restore_float16(*bound->strengths) ==
            std::vector<std::uint16_t>{0x3c00U, 0x3a00U}));
}

void no_change_pending_and_owner_boundaries() {
    const auto inputs = fixture();
    const auto bound = bind(inputs);
    const auto conflict_update = std::make_shared<const DetachedVrsStateUpdateReceipt>(
        std::string(64, 'a'), std::vector<VrsConnectionStateUpdate>{
            VrsConnectionStateUpdate(
                "fixture", "vrs-edge:0", "fixture", CurrentEvidenceVerdict::support,
                1.0, 1.0, "abstain_conflict",
                assess_vrs_experience_promotion(
                    std::string(64, 'a'), "vrs-edge:0", 1.0, 1.0))});
    const auto unchanged = bound->settle({}, conflict_update, "vrs-edge:", nullptr, 100);
    assert(unchanged.signal.rounds == 0 && unchanged.signal.scores.empty() &&
           unchanged.signal.strengths.empty());
    const auto stored = bound->prepare(unchanged);
    assert(stored.scores == bound->scores && stored.strengths == bound->strengths);
    assert(stored.promotions.size() == 1 && stored.promotions[0].action == "retain");

    const auto pending = bound->settle({}, receipt(), "vrs-edge:", nullptr, 1);
    assert(pending.signal.pending());
    assert(rejects([&] { (void)bound->prepare(pending); }));
    const auto other = bind(inputs);
    assert(rejects([&] {
        (void)other->settle({}, {}, "vrs-edge:", &pending, 1000);
    }));
    assert(rejects([&] { (void)other->prepare(unchanged); }));
    const auto resumed = bound->settle({}, {}, "vrs-edge:", &pending, 999);
    const auto once = bound->settle({}, receipt(), "vrs-edge:", nullptr, 1000);
    assert(resumed.signal.scores == once.signal.scores);
    assert(resumed.signal.strengths == once.signal.strengths);
}

void cold_binding_rejects_layout_and_values() {
    const auto inputs = fixture();
    const std::vector<std::size_t> score_shape{3};
    auto changed_score = inputs->score.materialize();
    changed_score[0] += 0.1F;
    const auto score_raw = bytes(changed_score);
    auto bad_scores = VrsArrayBlocks::prepare(
        {"<f4", score_shape, score_raw}, {}, 64).array;
    const std::vector<std::uint16_t> half{0x3c00U, 0x3a00U};
    const std::vector<std::size_t> strength_shape{2};
    const auto half_raw = bytes(half);
    auto strengths = VrsArrayBlocks::prepare(
        {"<f2", strength_shape, half_raw}, {}, 64).array;
    assert(rejects([&] {
        (void)BoundEventSignalStorage::cold_bind(inputs, bad_scores, strengths);
    }));
    const auto dense_score = inputs->score.materialize();
    const auto score_raw_good = bytes(dense_score);
    auto scores = VrsArrayBlocks::prepare(
        {"<f4", score_shape, score_raw_good}, {}, 64).array;
    const std::vector<float> wrong_strength{1.0F, 0.75F};
    const auto wrong_raw = bytes(wrong_strength);
    auto wrong_dtype = VrsArrayBlocks::prepare(
        {"<f4", strength_shape, wrong_raw}, {}, 64).array;
    assert(rejects([&] {
        (void)BoundEventSignalStorage::cold_bind(inputs, scores, wrong_dtype);
    }));
    const std::vector<std::uint16_t> wrong_half{0x3800U, 0x3a00U};
    const auto wrong_half_raw = bytes(wrong_half);
    auto wrong_values = VrsArrayBlocks::prepare(
        {"<f2", strength_shape, wrong_half_raw}, {}, 64).array;
    assert(rejects([&] {
        (void)BoundEventSignalStorage::cold_bind(inputs, scores, wrong_values);
    }));
}

void sparse_delta_settles_and_persists_only_candidate_changes() {
    const auto parent = fixture();
    const auto bound = bind(parent);
    const auto parent_scores = bound->scores;
    const auto parent_strengths = bound->strengths;
    const std::array<float, 2> appended_direct{0.4F, -0.2F};
    const std::array<float, 2> appended_score{0.2F, -0.1F};
    const std::array<std::uint8_t, 2> appended_unresolved{0, 1};
    const std::array<EventSignalEdge, 2> appended_edges{
        EventSignalEdge{1, 3, 1, 0.5F}, EventSignalEdge{3, 4, -1, 0.25F}};
    const std::array<float, 2> appended_strength{0.5F, 0.25F};
    const std::array<std::size_t, 1> strength_indices{1};
    const std::array<float, 1> strength_values{0.5F};
    const std::array<std::size_t, 1> direct_indices{0};
    const std::array<float, 1> direct_values{0.3F};
    const std::array<std::size_t, 1> score_indices{2};
    const std::array<float, 1> score_values{-0.25F};
    const auto candidate = prepare_event_delta(
        parent, std::string(64, 'b'), appended_direct, appended_score,
        appended_unresolved, appended_edges, appended_strength,
        {}, {}, strength_indices, strength_values, direct_indices, direct_values,
        score_indices, score_values);

    const auto proposal = bound->settle_candidate(candidate, {}, {}, "vrs-edge:", nullptr, 1000);
    assert(!proposal.signal.pending());
    assert(proposal.inputs_owner == candidate);
    assert(proposal.signal.belongs_to(*candidate));
    assert(proposal.signal.seed_nodes == std::vector<std::size_t>({0, 2, 3, 4}));
    const auto stored = bound->prepare(proposal);

    auto expected_scores = candidate->score.materialize();
    for (const auto& [index, value] : proposal.signal.scores) expected_scores[index] = value;
    auto expected_strengths = candidate->strength.materialize();
    for (const auto& [index, value] : proposal.signal.strengths) expected_strengths[index] = value;
    std::vector<std::uint16_t> expected_halves;
    for (const auto value : expected_strengths)
        expected_halves.push_back(event_strength_float16_bits(value));
    assert(restore_float32(*stored.scores) == expected_scores);
    assert(restore_float16(*stored.strengths) == expected_halves);
    assert(stored.scores->shape == std::vector<std::size_t>{5});
    assert(stored.strengths->shape == std::vector<std::size_t>{4});
    assert(stored.diagnostics.scores.sparse_storage_update);
    assert(stored.diagnostics.strengths.sparse_storage_update);
    assert(bound->scores == parent_scores && bound->strengths == parent_strengths);
    assert(restore_float32(*bound->scores) == parent->score.materialize());
    assert((restore_float16(*bound->strengths) ==
            std::vector<std::uint16_t>{0x3c00U, 0x3a00U}));

    const auto successor = stored.successor_inputs(std::string(64, 'c'));
    assert(successor->inputs->snapshot_id == std::string(64, 'c'));
    assert(successor->inputs->delta_parent() == nullptr);
    assert(successor->scores == stored.scores && successor->strengths == stored.strengths);
    assert(successor->inputs->score.materialize() == expected_scores);
    assert(successor->inputs->strength.materialize() == expected_strengths);
    assert(successor->inputs->dependency_segment_count() ==
           candidate->dependency_segment_count());
    assert(successor->inputs->dependency_index_bytes() ==
           candidate->dependency_index_bytes());
    for (std::size_t node = 0; node < candidate->score.size(); ++node) {
        assert(successor->inputs->incoming(node) == candidate->incoming(node));
        assert(successor->inputs->outgoing(node) == candidate->outgoing(node));
    }
    assert(rejects([&] { (void)stored.successor_inputs("bad"); }));
    assert(rejects([&] {
        (void)stored.successor_inputs(candidate->snapshot_id);
    }));
    assert(rejects([&] {
        (void)stored.successor_inputs(parent->snapshot_id);
    }));

    const std::array<float, 1> next_direct{0.1F};
    const std::array<float, 1> next_score{0.0F};
    const std::array<std::uint8_t, 1> next_unresolved{0};
    const std::array<EventSignalEdge, 1> next_edge{EventSignalEdge{4, 5, 1, 0.5F}};
    const std::array<float, 1> next_strength{0.5F};
    const auto next = prepare_event_delta(
        successor->inputs, std::string(64, 'd'), next_direct, next_score,
        next_unresolved, next_edge, next_strength);
    const auto next_proposal = successor->settle_candidate(
        next, {}, {}, "vrs-edge:", nullptr, 1000);
    assert(!next_proposal.signal.pending());
    assert(next_proposal.inputs_owner == next);
}

void sparse_delta_rejects_non_durable_and_foreign_generations() {
    const auto parent = fixture();
    const auto bound = bind(parent);
    const std::array<float, 1> direct{0.1F};
    const std::array<float, 1> score{0.2F};
    const std::array<std::uint8_t, 1> unresolved{0};
    const std::array<EventSignalEdge, 1> edge{EventSignalEdge{0, 3, 1, 0.1F}};
    const std::array<float, 1> unrounded_strength{0.1F};
    const auto unrounded = prepare_event_delta(
        parent, std::string(64, 'b'), direct, score, unresolved, edge,
        unrounded_strength);
    assert(rejects([&] {
        (void)bound->settle_candidate(unrounded, {}, {}, "vrs-edge:", nullptr, 1000);
    }));

    const auto foreign_parent = fixture();
    const auto foreign = prepare_event_delta(
        foreign_parent, std::string(64, 'c'), {}, {}, {}, {}, {});
    assert(rejects([&] {
        (void)bound->settle_candidate(foreign, {}, {}, "vrs-edge:", nullptr, 1000);
    }));

    const auto first = prepare_event_delta(
        parent, std::string(64, 'd'), {}, {}, {}, {}, {});
    const auto second = prepare_event_delta(
        first, std::string(64, 'e'), {}, {}, {}, {}, {});
    assert(rejects([&] {
        (void)bound->settle_candidate(second, {}, {}, "vrs-edge:", nullptr, 1000);
    }));
}

}  // namespace

int main() {
    static_assert(!std::is_copy_assignable_v<StorageBoundEventSignalProposal>);
    static_assert(!std::is_copy_assignable_v<PreparedEventSignalStorage>);
    assert(vrs_event_storage_source_sha256 ==
           "e1d10843da65ef8938d1ae138c5180a0680e24531a56441b3c37aaebaabc68e4");
    paired_storage_exact();
    no_change_pending_and_owner_boundaries();
    cold_binding_rejects_layout_and_values();
    sparse_delta_settles_and_persists_only_candidate_changes();
    sparse_delta_rejects_non_durable_and_foreign_generations();
    std::cout << "VRS event storage tests passed\n";
}
