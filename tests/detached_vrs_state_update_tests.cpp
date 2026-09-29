#include "world/detached_vrs_state_update.hpp"

#include <cassert>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <type_traits>

using namespace swegca::world;

namespace {

std::vector<VrsReplayedEpisode> edges() {
    return {
        {"vrs-edge:0", {{0, 1.0, std::nullopt, std::nullopt}}},
        {"vrs-edge:1", {{1, 0.75, std::nullopt, std::nullopt}}},
    };
}

bool rejects(const auto& operation) {
    try { operation(); }
    catch (const std::invalid_argument&) { return true; }
    return false;
}

}  // namespace

int main() {
    static_assert(!std::is_copy_assignable_v<VrsExperiencePromotionDecision>);
    static_assert(!std::is_move_assignable_v<VrsExperiencePromotionDecision>);
    static_assert(!std::is_copy_assignable_v<VrsConnectionStateUpdate>);
    static_assert(!std::is_move_assignable_v<VrsConnectionStateUpdate>);
    static_assert(!std::is_copy_assignable_v<DetachedVrsStateUpdateReceipt>);
    static_assert(!std::is_move_assignable_v<DetachedVrsStateUpdateReceipt>);
    assert(detached_vrs_update_source_sha256 ==
           "e43aafe08e0a3b6e0ba6b8fa10150768d563645f5767749836552d1d8ecef39a");
    const std::vector<VrsReEvidenceJudgment> judgments{
        {"vrs-edge:0", "vrs-edge:0", CurrentEvidenceVerdict::support},
        {"vrs-edge:1", "vrs-edge:1", CurrentEvidenceVerdict::refute}};
    const auto receipt = plan_detached_vrs_state_update(
        "snapshot:fixture", edges(), judgments, {});
    assert(receipt.detached_state_only && !receipt.persistent_state_mutated);
    assert((receipt.stage_order == std::array<std::string, 5>{
        "deja_vu", "recall", "replay", "re_evidence", "vrs_state_update"}));
    assert(!receipt.action_authorized && !receipt.persistent_write_authorized &&
           !receipt.semantic_promotion_authorized);
    assert(receipt.updates.size() == 2);
    assert(receipt.updates[0].connection_id == "vrs-edge:0");
    assert(receipt.updates[0].current_strength == 1.01);
    assert(receipt.updates[0].update_action == "reinforce");
    assert(receipt.updates[0].promotion.action == "retain");
    assert(receipt.updates[1].current_strength == 0.75 * 0.995);
    assert(receipt.updates[1].update_action == "weaken");
    assert(receipt.updates[1].promotion.action == "remain_unpromoted");

    const std::vector<VrsReEvidenceJudgment> same_proposition{
        {"vrs-edge:0", "door-state", CurrentEvidenceVerdict::support},
        {"vrs-edge:1", "door-state", CurrentEvidenceVerdict::refute}};
    const auto conflicts = plan_detached_vrs_state_update(
        "snapshot:fixture", edges(), same_proposition, {"door-state"});
    assert(conflicts.updates.size() == 2);
    for (const auto& row : conflicts.updates) {
        assert(row.update_action == "abstain_conflict");
        assert(row.current_strength == row.previous_strength);
    }

    const std::vector<VrsReplayedEpisode> aliases{
        {"vrs-edge:0", {{std::nullopt, std::nullopt, 0, 1.0}}},
        {"vrs-edge-group:0", {{std::nullopt, std::nullopt, 0, 1.0}}}};
    for (const bool opposed : {false, true}) {
        const std::vector<VrsReEvidenceJudgment> alias_judgments{
            {"vrs-edge:0", "p:vrs-edge:0", CurrentEvidenceVerdict::support},
            {"vrs-edge-group:0", "p:vrs-edge-group:0",
             opposed ? CurrentEvidenceVerdict::refute : CurrentEvidenceVerdict::support}};
        const auto update = plan_detached_vrs_state_update(
            "snapshot:fixture", aliases, alias_judgments, {});
        assert(update.updates.size() == 1);
        const auto& row = update.updates.front();
        assert(row.connection_id == "vrs-edge-group:0");
        assert(row.source_judgments.size() == 2);
        assert(row.current_strength == (opposed ? 1.0 : 1.01));
        assert(row.update_action == (opposed ? "abstain_conflict" : "reinforce"));
    }

    const auto promoted = assess_vrs_experience_promotion(
        "snapshot:fixture", "vrs-edge-group:0", 0.999, 0.999 * 1.01);
    assert(promoted.action == "promote" && promoted.promoted &&
           promoted.semantic_evidence_allowed);
    const auto revoked = assess_vrs_experience_promotion(
        "snapshot:fixture", "vrs-edge-group:0", 1.0, 0.995);
    assert(revoked.action == "revoke" && !revoked.promoted &&
           !revoked.semantic_evidence_allowed && revoked.underlying_experience_preserved);

    auto mismatched = aliases;
    mismatched[1].observations[0].deweighted_vrs_strength = 0.9;
    assert(rejects([&] {
        (void)plan_detached_vrs_state_update(
            "snapshot:fixture", mismatched,
            {{"vrs-edge:0", "a", CurrentEvidenceVerdict::support},
             {"vrs-edge-group:0", "b", CurrentEvidenceVerdict::support}}, {});
    }));
    assert(rejects([&] {
        (void)plan_detached_vrs_state_update(
            "snapshot:fixture", edges(),
            {{"absent", "p", CurrentEvidenceVerdict::support}}, {});
    }));

    const auto no_edge = plan_detached_vrs_state_update(
        "snapshot:fixture", {{"plain-episode", {{}}}},
        {{"plain-episode", "p", CurrentEvidenceVerdict::support}}, {});
    assert(no_edge.updates.empty());

    std::cout << "detached VRS state update tests passed\n";
}
