#pragma once

#include "world/cognitive_state.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace swegca::world {

// Pinned source: src/tinylm_slicer/mosaic_omni.py
inline constexpr std::string_view mosaic_omni_visual_source_sha256 =
    "a06f7a85128827b036a7c1801920e570e187c0163df6c3851d85ef2dd3bb5e20";

// Matrices retain torch.nn.Linear's row-major [out_features, in_features]
// layout. Layer-normalization parameters are one-dimensional [features].
struct VisualTeacherSlotBridgeWeights final {
    std::vector<double> slot_norm_weight;
    std::vector<double> slot_norm_bias;
    std::vector<double> patch_norm_weight;
    std::vector<double> patch_norm_bias;
    std::vector<double> query_weight;
    std::vector<double> key_weight;
    std::vector<double> value_down_weight;
    std::vector<double> value_down_bias;
    std::vector<double> value_up_weight;
    std::vector<double> value_up_bias;

    [[nodiscard]] static VisualTeacherSlotBridgeWeights initialize(
        std::size_t world_dim, std::size_t rank);
    void validate(std::size_t world_dim, std::size_t rank) const;
};

class VisualTeacherSlotBridge final {
public:
    VisualTeacherSlotBridge(std::size_t world_dim, std::size_t rank);
    VisualTeacherSlotBridge(std::size_t world_dim, std::size_t rank,
                            VisualTeacherSlotBridgeWeights weights);

    [[nodiscard]] Tensor forward(const Tensor& world_slots,
                                 const Tensor& image_patches,
                                 std::size_t patch_height,
                                 std::size_t patch_width) const;

private:
    std::size_t world_dim_{};
    std::size_t rank_{};
    VisualTeacherSlotBridgeWeights weights_;
};

struct ExplicitObjectRelationGrounderWeights final {
    std::vector<double> descriptor_norm_weight;
    std::vector<double> descriptor_norm_bias;
    std::vector<double> patch_norm_weight;
    std::vector<double> patch_norm_bias;
    std::vector<double> query_weight;
    std::vector<double> key_weight;
    std::vector<double> value_weight;
    std::vector<double> value_bias;
    std::vector<double> object_up_weight;
    std::vector<double> object_up_bias;
    std::vector<double> relation_norm_weight;
    std::vector<double> relation_norm_bias;
    std::vector<double> relation_down_weight;
    std::vector<double> relation_down_bias;
    std::vector<double> relation_up_weight;
    std::vector<double> relation_up_bias;

    [[nodiscard]] static ExplicitObjectRelationGrounderWeights initialize(
        std::size_t world_dim, std::size_t rank);
    void validate(std::size_t world_dim, std::size_t rank) const;
};

struct ExplicitObjectRelationGrounderOutput final {
    Tensor slots;
    Tensor attention;
};

class ExplicitObjectRelationGrounder final {
public:
    ExplicitObjectRelationGrounder(std::size_t world_dim, std::size_t rank);
    ExplicitObjectRelationGrounder(
        std::size_t world_dim, std::size_t rank,
        ExplicitObjectRelationGrounderWeights weights);

    [[nodiscard]] ExplicitObjectRelationGrounderOutput forward(
        const Tensor& image_patches, const Tensor& subject_descriptor,
        const Tensor& object_descriptor, std::size_t patch_height,
        std::size_t patch_width) const;

private:
    std::size_t world_dim_{};
    std::size_t rank_{};
    ExplicitObjectRelationGrounderWeights weights_;
};

struct ExplicitRelationHeadWeights final {
    std::vector<double> norm_weight;
    std::vector<double> norm_bias;
    std::vector<double> output_weight;
    std::vector<double> output_bias;

    [[nodiscard]] static ExplicitRelationHeadWeights initialize(
        std::size_t dimension, std::size_t classes);
    void validate(std::size_t dimension, std::size_t classes) const;
};

class ExplicitRelationHead final {
public:
    ExplicitRelationHead(std::size_t dimension, std::size_t classes);
    ExplicitRelationHead(std::size_t dimension, std::size_t classes,
                         ExplicitRelationHeadWeights weights);

    [[nodiscard]] Tensor forward(const Tensor& relation_slot) const;

private:
    std::size_t dimension_{};
    std::size_t classes_{};
    ExplicitRelationHeadWeights weights_;
};

}  // namespace swegca::world
