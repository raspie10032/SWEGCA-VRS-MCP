#include "world/vrs_event_storage.hpp"

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
    const auto score_bytes = bytes(inputs->score);
    auto scores = VrsArrayBlocks::prepare(
        {"<f4", score_shape, score_bytes}, {}, 64).array;
    std::vector<std::uint16_t> half;
    for (const auto strength : inputs->strength) {
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
    assert(restore_float32(*bound->scores) == inputs->score);
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
    auto changed_score = inputs->score;
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
    const auto score_raw_good = bytes(inputs->score);
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

}  // namespace

int main() {
    static_assert(!std::is_copy_assignable_v<StorageBoundEventSignalProposal>);
    static_assert(!std::is_copy_assignable_v<PreparedEventSignalStorage>);
    assert(vrs_event_storage_source_sha256 ==
           "e1d10843da65ef8938d1ae138c5180a0680e24531a56441b3c37aaebaabc68e4");
    paired_storage_exact();
    no_change_pending_and_owner_boundaries();
    cold_binding_rejects_layout_and_values();
    std::cout << "VRS event storage tests passed\n";
}
