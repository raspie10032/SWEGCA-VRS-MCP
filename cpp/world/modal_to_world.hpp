#pragma once

#include "checkpoint/materialized_tensor.hpp"
#include "world/world_state.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace swegca::world {

struct ModalToWorldConfig final {
    std::uint64_t source_dim{};
    WorldConfig world{};
    std::uint64_t attention_heads{};

    void validate() const;
};

class ModalToWorldWeights final {
public:
    ModalToWorldWeights(
        checkpoint::MaterializedTensor world_queries,
        checkpoint::MaterializedTensor source_norm_weight,
        checkpoint::MaterializedTensor source_norm_bias,
        checkpoint::MaterializedTensor source_linear_weight,
        checkpoint::MaterializedTensor source_linear_bias,
        checkpoint::MaterializedTensor attention_in_weight,
        checkpoint::MaterializedTensor attention_in_bias,
        checkpoint::MaterializedTensor attention_out_weight,
        checkpoint::MaterializedTensor attention_out_bias,
        checkpoint::MaterializedTensor output_norm_weight,
        checkpoint::MaterializedTensor output_norm_bias,
        const ModalToWorldConfig& config);

    [[nodiscard]] static ModalToWorldWeights load(
        const checkpoint::RestrictedCheckpoint& checkpoint,
        const ModalToWorldConfig& config);

private:
    friend class ModalToWorldAdapter;

    const ModalToWorldConfig config_;
    std::vector<float> world_queries_;
    std::vector<float> source_norm_weight_;
    std::vector<float> source_norm_bias_;
    std::vector<float> source_linear_weight_;
    std::vector<float> source_linear_bias_;
    std::vector<float> attention_in_weight_;
    std::vector<float> attention_in_bias_;
    std::vector<float> attention_out_weight_;
    std::vector<float> attention_out_bias_;
    std::vector<float> output_norm_weight_;
    std::vector<float> output_norm_bias_;
};

class ModalToWorldAdapter final {
public:
    ModalToWorldAdapter(ModalToWorldConfig config, ModalToWorldWeights weights);

    [[nodiscard]] WorldState forward(const Tensor& source_states,
                                     const BooleanMask* source_mask,
                                     std::string source) const;
    [[nodiscard]] WorldState forward(const Tensor& source_states,
                                     std::string source) const;

private:
    const ModalToWorldConfig config_;
    const ModalToWorldWeights weights_;
};

}  // namespace swegca::world
