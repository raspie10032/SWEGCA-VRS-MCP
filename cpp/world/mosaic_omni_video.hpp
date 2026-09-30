#pragma once

#include "world/cognitive_state.hpp"

#include <cstddef>
#include <vector>

namespace swegca::world {

struct VideoObjectTrackerWeights final {
    std::vector<float> queries;
    std::vector<float> query_norm_weight, query_norm_bias;
    std::vector<float> token_norm_weight, token_norm_bias;
    std::vector<float> position_projection_weight;
};

struct VideoObjectTracks final { Tensor tracks; Tensor attention; };

class LearnedVideoObjectTracker final {
public:
    LearnedVideoObjectTracker(std::size_t slots, std::size_t dimension,
        bool spatial_coordinates, VideoObjectTrackerWeights weights);
    [[nodiscard]] VideoObjectTracks track_with_attention(
        const Tensor& feature_map, std::size_t batch, std::size_t frames) const;
    [[nodiscard]] Tensor forward(
        const Tensor& feature_map, std::size_t batch, std::size_t frames) const;
private:
    std::size_t slots_{}, dimension_{};
    bool spatial_coordinates_{};
    VideoObjectTrackerWeights weights_;
};

struct ObjectTrajectoryBindingWeights final {
    std::vector<float> identity_norm_weight, identity_norm_bias;
    std::vector<float> trajectory_norm_weight, trajectory_norm_bias;
    std::vector<float> query_weight, key_weight, value_weight, output_weight;
    void validate(std::size_t dimension, bool pair) const;
};

struct ObjectTrajectoryBindingOutput final { Tensor decision; Tensor weights; };

class QueryConditionedObjectTrajectoryBinding final {
public:
    explicit QueryConditionedObjectTrajectoryBinding(
        std::size_t dimension, ObjectTrajectoryBindingWeights weights);
    [[nodiscard]] ObjectTrajectoryBindingOutput forward(
        const Tensor& identity, const Tensor& event, const Tensor& delta,
        const Tensor& trajectory, const Tensor& query) const;
private:
    std::size_t dimension_{};
    ObjectTrajectoryBindingWeights weights_;
};

class QueryConditionedObjectPairTrajectoryBinding final {
public:
    explicit QueryConditionedObjectPairTrajectoryBinding(
        std::size_t dimension, ObjectTrajectoryBindingWeights weights);
    [[nodiscard]] ObjectTrajectoryBindingOutput forward(
        const Tensor& identity, const Tensor& event, const Tensor& delta,
        const Tensor& trajectory, const Tensor& queries) const;
private:
    std::size_t dimension_{};
    ObjectTrajectoryBindingWeights weights_;
};

}  // namespace swegca::world
