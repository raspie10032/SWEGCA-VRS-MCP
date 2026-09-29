#pragma once

#include "world/evidence_accumulator.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace swegca::world {

struct ReplayCompileStats final {
    std::size_t applied = 0;
    std::size_t duplicate = 0;
    std::size_t expired = 0;
    std::size_t insufficient = 0;
    std::size_t other_hypothesis = 0;

    friend bool operator==(const ReplayCompileStats&, const ReplayCompileStats&) = default;
};

struct ReplayIntervention final {
    std::string name;
    std::vector<std::string> drop_axes;
    std::vector<std::string> flip_axes;
    std::vector<std::string> drop_evidence_addresses;
};

struct CompiledEvidenceReplay final {
    std::string hypothesis_id;
    std::string cache_hash;
    std::vector<std::string> axes;
    std::vector<std::string> evidence_addresses;
    std::vector<EvidenceObservation> observations;
    ReplayCompileStats stats;
    std::int64_t current_step = 0;

    [[nodiscard]] std::size_t observation_count() const noexcept {
        return observations.size();
    }
};

struct CompiledReplayInterventions final {
    std::vector<std::string> names;
    std::vector<ReplayIntervention> variants;

    [[nodiscard]] std::size_t variant_count() const noexcept {
        return variants.size();
    }
};

struct ReplayDecision final {
    std::string intervention;
    std::string status;
    std::string reason;
    double posterior_mean = 0.0;
    double causal_lower_bound = 0.0;
    double overall_upper_bound = 0.0;
    double effective_sample_size = 0.0;
    std::size_t source_diversity = 0;
    std::size_t context_diversity = 0;
    double regime_change_score = 0.0;

    friend bool operator==(const ReplayDecision&, const ReplayDecision&) = default;
};

struct ReplayBatchResult final {
    std::string cache_hash;
    std::vector<ReplayDecision> decisions;

    friend bool operator==(const ReplayBatchResult&, const ReplayBatchResult&) = default;
};

[[nodiscard]] CompiledEvidenceReplay compile_evidence_replay(
    std::span<const EvidenceObservation> observations,
    const EvidenceAccumulatorConfig& config,
    std::string hypothesis_id,
    std::int64_t current_step);

[[nodiscard]] CompiledReplayInterventions compile_replay_interventions(
    const CompiledEvidenceReplay& cache,
    std::span<const ReplayIntervention> interventions);

[[nodiscard]] ReplayBatchResult evaluate_compiled_counterfactual_replays(
    const CompiledEvidenceReplay& cache,
    const CompiledReplayInterventions& interventions,
    const EvidenceAccumulatorConfig& config);

[[nodiscard]] ReplayBatchResult evaluate_counterfactual_replays(
    const CompiledEvidenceReplay& cache,
    std::span<const ReplayIntervention> interventions,
    const EvidenceAccumulatorConfig& config);

[[nodiscard]] constexpr std::string_view counterfactual_replay_source_sha256() noexcept {
    return "6526d05104657ddf9a6c93c07da47068dd674ed5aeb52e8f7679e4b8c3068c39";
}

}  // namespace swegca::world
