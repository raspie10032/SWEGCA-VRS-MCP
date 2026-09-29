#include "world/counterfactual_replay.hpp"

#include <cassert>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace swegca::world;

namespace {

std::vector<EvidenceObservation> observations() {
    const EvidenceAccumulatorConfig config;
    std::vector<EvidenceObservation> result;
    for (int index = 0; index != 16; ++index) {
        result.emplace_back(
            "가설-가속", "증거:" + std::to_string(index),
            "출처:" + std::to_string(index % 2), "문맥:" + std::to_string(index),
            config.required_axes[static_cast<std::size_t>(index) %
                                 config.required_axes.size()],
            "support", index, std::nullopt,
            "replay-producer:" + std::to_string(index), 0.0);
    }
    return result;
}

bool near(const double left, const double right) {
    return std::abs(left - right) < 1e-14;
}

}  // namespace

int main() {
    const EvidenceAccumulatorConfig config;
    const auto evidence = observations();
    const auto cache = compile_evidence_replay(
        evidence, config, "가설-가속", 15);
    assert(cache.cache_hash ==
           "5125f80aff0e458baba17fa1fb195f7c10c0f9eade731f40bad3e72b07277963");
    assert((cache.stats == ReplayCompileStats{16, 0, 0, 0, 0}));
    assert(cache.observation_count() == 16);
    assert(cache.evidence_addresses.front() == "증거:0");
    assert(cache.evidence_addresses.back() == "증거:15");

    const std::vector<ReplayIntervention> interventions{
        {"normal", {}, {}, {}},
        {"remove-axis", {"counterfactual"}, {}, {}},
        {"flip-axis", {}, {"observational"}, {}},
        {"remove-address", {}, {}, {"증거:0"}}};
    const auto result = evaluate_counterfactual_replays(
        cache, interventions, config);
    assert(result.cache_hash == cache.cache_hash);
    assert(result.decisions.size() == 4);
    const auto& normal = result.decisions[0];
    assert(normal.status == "accept" && normal.reason == "causal_lower_bound");
    assert(near(normal.posterior_mean, 0.9444444444444444));
    assert(near(normal.causal_lower_bound, 0.5965213747972955));
    assert(normal.overall_upper_bound == 1.0);
    assert(normal.effective_sample_size == 16.0);
    assert(normal.source_diversity == 2 && normal.context_diversity == 16);
    assert(near(normal.regime_change_score, 0.05555555555555558));

    const auto& removed_axis = result.decisions[1];
    assert(removed_axis.status == "abstain");
    assert(removed_axis.reason == "minimum_effective_samples");
    assert(near(removed_axis.posterior_mean, 0.9285714285714286));
    assert(removed_axis.causal_lower_bound == 0.0);
    assert(removed_axis.effective_sample_size == 12.0);

    const auto& flipped = result.decisions[2];
    assert(flipped.status == "abstain" && flipped.reason == "uncertain");
    assert(near(flipped.posterior_mean, 0.7222222222222222));
    assert(near(flipped.overall_upper_bound, 0.8824441955346628));
    assert(near(flipped.regime_change_score, 0.11111111111111116));

    const auto& removed_address = result.decisions[3];
    assert(removed_address.status == "abstain");
    assert(removed_address.reason == "minimum_effective_samples");
    assert(near(removed_address.posterior_mean, 0.9411764705882353));
    assert(near(removed_address.causal_lower_bound, 0.5258044258424874));
    assert(removed_address.effective_sample_size == 15.0);

    auto filtered_input = std::vector<EvidenceObservation>{
        evidence[0], evidence[0],
        EvidenceObservation("가설-가속", "만료", "출처:0", "문맥:0",
                            "observational", "support", 1, 2,
                            "replay-producer:0", 0.0),
        EvidenceObservation("가설-가속", "불충분", "출처:0", "문맥:0",
                            "observational", "insufficient", 0, std::nullopt,
                            "replay-producer:0", 0.0),
        EvidenceObservation("다른가설", "다른가설", "출처:0", "문맥:0",
                            "observational", "support", 0, std::nullopt,
                            "replay-producer:0", 0.0)};
    const auto filtered = compile_evidence_replay(
        filtered_input, config, "가설-가속", 3);
    assert(filtered.observation_count() == 1);
    assert((filtered.stats == ReplayCompileStats{1, 1, 1, 1, 1}));

    const auto compiled = compile_replay_interventions(cache, interventions);
    assert(evaluate_compiled_counterfactual_replays(cache, compiled, config) == result);
    assert(evaluate_counterfactual_replays(cache, interventions, config) == result);

    bool unknown_axis_rejected = false;
    try {
        const std::vector<ReplayIntervention> invalid{{"unknown", {"undefined"}, {}, {}}};
        static_cast<void>(evaluate_counterfactual_replays(cache, invalid, config));
    } catch (const std::invalid_argument&) {
        unknown_axis_rejected = true;
    }
    assert(unknown_axis_rejected);

    bool contradiction_rejected = false;
    try {
        const std::vector<ReplayIntervention> invalid{
            {"contradiction", {"observational"}, {"observational"}, {}}};
        static_cast<void>(evaluate_counterfactual_replays(cache, invalid, config));
    } catch (const std::invalid_argument&) {
        contradiction_rejected = true;
    }
    assert(contradiction_rejected);

    assert(counterfactual_replay_source_sha256() ==
           "6526d05104657ddf9a6c93c07da47068dd674ed5aeb52e8f7679e4b8c3068c39");
    std::cout << "counterfactual replay cache and batch decision tests passed\n";
}
