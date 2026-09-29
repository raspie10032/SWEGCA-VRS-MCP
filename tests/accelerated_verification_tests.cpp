#include "world/accelerated_verification.hpp"

#include <cstdlib>
#include <functional>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace {

using namespace swegca::world;

void check(const bool condition, const char* expression, const int line) {
    if (!condition) {
        std::cerr << "CHECK failed at line " << line << ": " << expression << '\n';
        std::exit(1);
    }
}
#define CHECK(expression) check(static_cast<bool>(expression), #expression, __LINE__)

void rejects(const std::function<void()>& operation, const std::string_view expected) {
    try {
        operation();
    } catch (const std::exception& error) {
        CHECK(std::string_view(error.what()).find(expected) != std::string_view::npos);
        return;
    }
    CHECK(false);
}

constexpr std::string_view hypothesis = "가설-검증";

CognitiveState state() {
    const auto tensor = [](const std::uint64_t slots) {
        return Tensor(TensorDType::float32, {1, slots, 8},
                      std::vector<double>(static_cast<std::size_t>(slots * 8)));
    };
    return CognitiveState(tensor(4), tensor(2), tensor(2), {}, {},
                          {{"autonomy_phase", "verify"},
                           {"active_hypothesis_id", std::string(hypothesis)}});
}

AutonomousCognitionConfig autonomy_config() {
    return AutonomousCognitionConfig({"read_memory"}, {"read_memory"});
}

std::vector<EvidenceObservation> observations(const std::string_view outcome) {
    const EvidenceAccumulatorConfig config;
    std::vector<EvidenceObservation> result;
    for (int index = 0; index != 16; ++index) {
        result.emplace_back(
            std::string(hypothesis), "증거:" + std::to_string(index),
            "출처:" + std::to_string(index % 2), "문맥:" + std::to_string(index),
            config.required_axes[static_cast<std::size_t>(index) % config.required_axes.size()],
            std::string(outcome), index, std::nullopt,
            "accelerated-producer:" + std::to_string(index), 0.0);
    }
    return result;
}

ReplayBatchResult result(const std::string_view outcome) {
    const EvidenceAccumulatorConfig config;
    const auto source = observations(outcome);
    const auto cache = compile_evidence_replay(source, config, std::string(hypothesis), 15);
    const std::vector<ReplayIntervention> interventions{{"normal", {}, {}, {}}};
    return evaluate_counterfactual_replays(cache, interventions, config);
}

AutonomyEvent verification_event(const ReplayBatchResult& replay,
                                 std::string cache_hash = {},
                                 const std::optional<std::size_t> revision = std::nullopt,
                                 std::string intervention = "normal") {
    if (cache_hash.empty()) cache_hash = replay.cache_hash;
    return AutonomyEvent(
        "가속검증", AutonomyEventKind::verification, std::string(hypothesis),
        {"replay-cache:" + cache_hash}, "한국어-연속센서", "문맥-검증", 1.0,
        {{"replay_revision", static_cast<std::int64_t>(revision.value_or(replay.revision))},
         {"replay_intervention", std::move(intervention)}});
}

void test_accept_and_abstain_transitions() {
    const auto accepted_result = result("support");
    const auto before = state();
    const auto accepted = advance_accelerated_verification(
        before, verification_event(accepted_result), autonomy_config(),
        accepted_result, "normal");
    CHECK(accepted.selected.decision.status == "accept");
    CHECK(accepted.selected.decision.revision == 16);
    CHECK(accepted.transition.accepted);
    CHECK(accepted.transition.next_phase == AutonomyPhase::remember);
    CHECK(accepted.transition.memory_write_allowed);
    CHECK(accepted.world_tensor_objects_reused);
    CHECK(accepted.transition.state.semantic_slots().storage_identity() ==
          before.semantic_slots().storage_identity());
    CHECK(accepted.transition.state.persistent_state_count() == 1);

    const auto insufficient_result = result("insufficient");
    const auto abstained = advance_accelerated_verification(
        state(), verification_event(insufficient_result), autonomy_config(),
        insufficient_result, "normal");
    CHECK(abstained.selected.decision.status == "abstain");
    CHECK(abstained.selected.decision.reason == "minimum_effective_samples");
    CHECK(abstained.transition.next_phase == AutonomyPhase::request_evidence);
    CHECK(abstained.transition.reason == "additional_evidence_required");
    CHECK(!abstained.transition.memory_write_allowed);
}

void test_selection_and_binding() {
    const auto replay = result("support");
    const auto selected = select_replay_decision(replay, "normal");
    CHECK(selected.index == 0);
    CHECK(selected.decision.status == "accept");
    CHECK(selected.decision.revision == replay.revision);
    rejects([&] { static_cast<void>(select_replay_decision(replay, "missing")); },
            "not in replay result");
    rejects(
        [&] {
            static_cast<void>(advance_accelerated_verification(
                state(), verification_event(replay, "wrong"), autonomy_config(),
                replay, "normal"));
        },
        "not bound");
    rejects(
        [&] {
            static_cast<void>(advance_accelerated_verification(
                state(), verification_event(replay, {}, replay.revision - 1),
                autonomy_config(), replay, "normal"));
        },
        "revision is stale");
    rejects(
        [&] {
            static_cast<void>(advance_accelerated_verification(
                state(), verification_event(replay, {}, std::nullopt, "other"),
                autonomy_config(), replay, "normal"));
        },
        "differs from selection");
    CHECK(accelerated_verification_source_sha256() ==
          "72a9c65a0e40451661b24fef926e26564baf5d2fd2c0128bda6673af55685c94");
}

}  // namespace

int main() {
    test_accept_and_abstain_transitions();
    test_selection_and_binding();
    std::cout << "accelerated verification binding tests passed\n";
}
