#pragma once

#include "world/cognitive_state.hpp"

#include <cstddef>
#include <string_view>
#include <vector>

namespace swegca::world {

inline constexpr std::string_view mosaic_omni_geometry_source_sha256 =
    "a06f7a85128827b036a7c1801920e570e187c0163df6c3851d85ef2dd3bb5e20";
inline constexpr std::size_t video_spatial_geometry_output_dim = 192;

struct VideoEgomotionWeights final {
    // PyTorch Conv2d layout is [out_channels,in_channels,kernel_h,kernel_w].
    std::vector<float> convolution1_weight;
    std::vector<float> convolution1_bias;
    std::vector<float> convolution2_weight;
    std::vector<float> convolution2_bias;
    std::vector<float> convolution3_weight;
    std::vector<float> convolution3_bias;
    // PyTorch Linear layout is [out_features,in_features].
    std::vector<float> output_weight;
    std::vector<float> output_bias;

    void validate(std::size_t world_dim) const;
};

class VideoEgomotionReasoner final {
public:
    VideoEgomotionReasoner(std::size_t world_dim, VideoEgomotionWeights weights);
    [[nodiscard]] Tensor forward(const Tensor& video) const;

private:
    std::size_t world_dim_{};
    VideoEgomotionWeights weights_;
};

// Direct port of mosaic_omni._video_egomotion_validity_statistics.
[[nodiscard]] Tensor video_egomotion_validity_statistics(const Tensor& video);

struct VideoSpatialGeometryWeights final {
    std::vector<float> frame_norm_weight;
    std::vector<float> frame_norm_bias;
    std::vector<float> frame_linear1_weight;
    std::vector<float> frame_linear1_bias;
    std::vector<float> frame_linear2_weight;
    std::vector<float> frame_linear2_bias;

    std::vector<float> stereo_norm_weight;
    std::vector<float> stereo_norm_bias;
    std::vector<float> stereo_linear1_weight;
    std::vector<float> stereo_linear1_bias;
    std::vector<float> stereo_linear2_weight;
    std::vector<float> stereo_linear2_bias;

    void validate(std::size_t camera_pose_dim) const;
};

class VideoSpatialGeometryReasoner final {
public:
    static constexpr std::size_t output_dim = video_spatial_geometry_output_dim;

    VideoSpatialGeometryReasoner(
        std::size_t camera_pose_dim, VideoSpatialGeometryWeights weights);
    [[nodiscard]] Tensor forward(
        const Tensor& attention, const Tensor& camera_pose) const;

private:
    [[nodiscard]] Tensor stereo_summary(
        const Tensor& role, const Tensor& camera_pose) const;

    std::size_t camera_pose_dim_{};
    VideoSpatialGeometryWeights weights_;
};

}  // namespace swegca::world
