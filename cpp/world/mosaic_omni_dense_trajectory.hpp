#pragma once

#include "world/mosaic_omni.hpp"

#include <cstddef>
#include <optional>
#include <string_view>
#include <vector>

namespace swegca::world {

inline constexpr std::string_view mosaic_omni_dense_trajectory_source_sha256 =
    "a06f7a85128827b036a7c1801920e570e187c0163df6c3851d85ef2dd3bb5e20";

struct DescriptorConditionedDenseTrajectoryConfig final {
    std::size_t dim{};
    bool pair_centered_queries{};
    bool persistent_identity_state{};
    bool object_memory{};
    double object_memory_scale{1.0};
    bool object_memory_query_gate{};
    bool object_memory_reliability_gate{};
    bool object_memory_contrast_visibility{};
    bool object_memory_contrast_readout{};
    bool object_memory_temporal_relative_visibility{};
    bool object_memory_temporal_relative_readout{};

    void validate() const;
};

// Linear matrices use torch.nn.Linear's [out_features, in_features] layout.
// Empty vectors represent modules that the selected configuration does not own.
struct DescriptorConditionedDenseTrajectoryWeights final {
    std::vector<double> feature_norm_weight;
    std::vector<double> feature_norm_bias;
    std::vector<double> query_weight;
    std::vector<double> key_weight;
    std::vector<double> trajectory_norm_weight;
    std::vector<double> trajectory_norm_bias;
    std::vector<double> value_weight;
    std::vector<double> output_weight;
    std::vector<double> memory_visibility_weight;
    std::vector<double> memory_visibility_bias;
    std::vector<double> memory_output_weight;
    std::vector<double> memory_contrast_output_weight;
    std::vector<double> memory_temporal_relative_output_weight;
    std::vector<double> memory_gate_weight;
    std::vector<double> memory_gate_bias;
    std::vector<double> memory_reliability_gate_weight;
    std::vector<double> memory_reliability_gate_bias;

    [[nodiscard]] static DescriptorConditionedDenseTrajectoryWeights initialize(
        const DescriptorConditionedDenseTrajectoryConfig& config);
    void validate(const DescriptorConditionedDenseTrajectoryConfig& config) const;
};

struct DescriptorConditionedDenseTrajectoryOutput final {
    Tensor decision;
    Tensor attention;
    std::optional<Tensor> visibility_logits;
    std::optional<Tensor> memory_margin;
    std::optional<Tensor> memory_reliability_logits;
};

class DescriptorConditionedDenseTrajectoryBinding final {
public:
    static constexpr std::size_t trajectory_width = 12;
    static constexpr std::size_t object_memory_width = 19;
    static constexpr std::size_t object_memory_visibility_width = 2;
    static constexpr std::size_t object_memory_contrast_visibility_width = 4;
    static constexpr std::size_t object_memory_contrast_summary_width = 8;

    explicit DescriptorConditionedDenseTrajectoryBinding(
        DescriptorConditionedDenseTrajectoryConfig config);
    DescriptorConditionedDenseTrajectoryBinding(
        DescriptorConditionedDenseTrajectoryConfig config,
        DescriptorConditionedDenseTrajectoryWeights weights);

    [[nodiscard]] DescriptorConditionedDenseTrajectoryOutput forward(
        const Tensor& features, const Tensor& queries,
        const Tensor* condition = nullptr,
        const Tensor* memory_scale = nullptr) const;

private:
    DescriptorConditionedDenseTrajectoryConfig config_;
    DescriptorConditionedDenseTrajectoryWeights weights_;
};

}  // namespace swegca::world
