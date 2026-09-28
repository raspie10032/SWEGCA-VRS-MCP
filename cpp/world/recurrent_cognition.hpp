#pragma once

#include "world/cognitive_state.hpp"
#include "world/world_state.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace swegca::world {

struct RecurrentCognitionConfig final {
    CognitiveKernelConfig state;
    std::uint64_t attention_heads{4};
    std::uint64_t mlp_hidden_dim{512};
    std::uint64_t minimum_cycles{1};
    std::uint64_t maximum_cycles{8};
    float halt_threshold{0.9F};
    float evidence_logit_epsilon{1.0e-4F};
    float maximum_update{1.0F};

    void validate() const;
};

// Exact parameter topology of mosaic_recurrent_cognition.SharedCognitionCell
// and RecurrentCognitionCore. Matrices use PyTorch Linear row-major layout
// [out_features, in_features].
struct RecurrentCognitionWeights final {
    std::vector<float> attention_norm_weight;
    std::vector<float> attention_norm_bias;
    std::vector<float> attention_in_projection_weight;
    std::vector<float> attention_in_projection_bias;
    std::vector<float> attention_out_projection_weight;
    std::vector<float> attention_out_projection_bias;
    std::vector<float> mlp_norm_weight;
    std::vector<float> mlp_norm_bias;
    std::vector<float> mlp_in_weight;
    std::vector<float> mlp_out_weight;
    std::vector<float> update_gate_weight;
    std::vector<float> update_gate_bias;
    std::vector<float> role_embeddings;
    std::vector<float> final_norm_weight;
    std::vector<float> final_norm_bias;
    std::vector<float> halt_head_weight;
    std::vector<float> halt_head_bias;
    float halt_evidence_scale{};

    void validate(const RecurrentCognitionConfig& config) const;
};

struct RecurrentEvidence final {
    // The source core accepts coverage/confidence without evidence tokens.
    // When tokens are absent, mask is ignored exactly as in the pinned Python.
    std::optional<Tensor> tokens;
    std::optional<BooleanMask> mask;
    std::optional<std::vector<float>> coverage;
    std::optional<std::vector<float>> confidence;
    std::optional<std::vector<float>> attention_key_weights;
};

enum class RecurrentExecutionMode : std::uint8_t { evaluation, training_selection };

struct CognitionTrace final {
    std::vector<std::uint64_t> cycles_used;
    std::vector<std::vector<float>> halt_logits;
    std::vector<std::vector<float>> halt_probabilities;
};

struct RecurrentCognitionOutput final {
    CognitiveState state;
    CognitionTrace trace;
};

class RecurrentCognitionCore final {
public:
    RecurrentCognitionCore(RecurrentCognitionConfig config,
                           RecurrentCognitionWeights weights);

    [[nodiscard]] const RecurrentCognitionConfig& config() const noexcept { return config_; }
    [[nodiscard]] RecurrentCognitionOutput run(
        const CognitiveState& state,
        const RecurrentEvidence* evidence = nullptr,
        RecurrentExecutionMode mode = RecurrentExecutionMode::evaluation) const;

private:
    RecurrentCognitionConfig config_;
    RecurrentCognitionWeights weights_;
};

// This preserves the historical caller-supplied Boolean routing contract. It
// does not create, validate, or imply Main commit authority.
[[nodiscard]] RecurrentCognitionOutput integrate_authorized_evidence(
    const RecurrentCognitionCore& core, const CognitiveState& state,
    const RecurrentEvidence& evidence, std::span<const std::string> evidence_refs,
    bool authorized, std::uint64_t maximum_evidence_refs,
    RecurrentExecutionMode mode = RecurrentExecutionMode::evaluation);

}  // namespace swegca::world
