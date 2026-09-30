#pragma once

#include "world/mosaic_omni_dense_trajectory.hpp"
#include "world/mosaic_omni_geometry.hpp"
#include "world/mosaic_omni_modal_frontends.hpp"
#include "world/mosaic_omni_video.hpp"

#include <cstddef>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

namespace swegca::world {

inline constexpr std::string_view mosaic_omni_video_pipeline_source_sha256 =
    "a06f7a85128827b036a7c1801920e570e187c0163df6c3851d85ef2dd3bb5e20";

struct VideoPipelineConfig final {
    std::size_t world_dim{};
    std::size_t world_slots{};
    std::size_t object_slots{};
    std::size_t vision_patch_size{};
    std::size_t vision_teacher_dim{};
    std::size_t camera_pose_dim{};
    std::size_t spatial_relation_classes{};
    std::size_t action_dim{};
    std::size_t egomotion_classes{};

    bool uses_visual_semantic_encoder{};
    double visual_semantic_scale{1.0};
    bool explicit_temporal_delta{};
    double explicit_temporal_delta_scale{1.0};
    bool separate_temporal_delta_projection{};
    bool object_temporal_encoder{};
    bool object_frame_normalized_input{};
    bool object_camera_invariant_residual{};
    double object_frame_normalized_residual_scale{1.0};
    bool object_time_centered_input{};
    bool object_activity_sorted_slots{};
    bool object_dual_evidence{};
    bool object_set_decision{};
    bool object_identity_event_binding{};
    bool object_learned_queries{};
    bool object_spatial_event_binding{};
    bool spatial_temporal_moment{};
    bool query_spatial_temporal_moment{};
    bool spatial_temporal_y_moment{};
    bool spatial_temporal_logit_head{};
    bool spatial_temporal_bilinear_head{};
    bool object_trajectory_binding{};
    bool object_pair_trajectory_binding{};
    bool descriptor_trajectory_binding{};
    bool descriptor_memory_evidence_routing{};
    double descriptor_memory_evidence_routing_margin{};
    bool query_conditioned_head{};
    bool camera_robustness_adapter{};
    bool camera_robustness_nonlinear_gate{};
    bool egomotion_validity_head{};
    bool egomotion_evidence_gate{};
    double egomotion_minimum_motion_evidence{1.0e-6};

    [[nodiscard]] static VideoPipelineConfig from_unified(
        const MosaicUnifiedConfig& config);
    void validate() const;
};

struct VideoNormLinear final {
    std::vector<double> norm_weight;
    std::vector<double> norm_bias;
    ModalLinearWeights linear;
};

struct VideoNormMlp final {
    std::vector<double> norm_weight;
    std::vector<double> norm_bias;
    ModalLinearWeights hidden;
    ModalLinearWeights output;
};

struct VideoMlp final {
    ModalLinearWeights hidden;
    ModalLinearWeights output;
};

struct VideoPipelineWeights final {
    ModalLinearWeights video_time;
    std::vector<double> temporal_mixer_weight;
    ModalGruWeights temporal_cell;
    ModalLinearWeights temporal_to_world;
    std::optional<ModalLinearWeights> temporal_delta_to_world;

    std::optional<ModalGruWeights> object_temporal_cell;
    std::optional<ModalConvWeights> object_frontend;
    std::optional<ModalConvWeights> object_camera_invariant_frontend;
    std::optional<ModalLinearWeights> object_to_world;
    std::optional<ModalLinearWeights> object_statistics;
    std::optional<VideoNormLinear> object_decision;
    std::optional<VideoNormLinear> object_set_decision;
    std::optional<VideoNormLinear> object_binding_decision;
    std::optional<ModalLinearWeights> spatial_moment_to_world;
    std::optional<ModalLinearWeights> spatial_moment_gate;
    std::optional<ModalLinearWeights> spatial_y_moment_to_world;
    std::optional<ModalLinearWeights> spatial_y_moment_gate;
    std::optional<ModalLinearWeights> spatial_logit_head;
    std::optional<ModalLinearWeights> spatial_bilinear_head;
    std::optional<ModalLinearWeights> query_conditioning;
    std::optional<ModalLinearWeights> object_evidence_gate;

    std::vector<double> video_modality_embedding;
    VideoNormLinear order_head;
    std::optional<ModalLinearWeights> camera_robustness_gate_linear;
    std::optional<VideoMlp> camera_robustness_gate_nonlinear;
    std::optional<ModalLinearWeights> camera_robustness_head;
    std::optional<ModalLinearWeights> camera_pose_encoder;
    std::optional<VideoNormMlp> spatial_relation_head;
    std::optional<ModalLinearWeights> action_encoder;
    std::optional<VideoNormMlp> egomotion_head;
    std::optional<VideoNormMlp> egomotion_validity_head_weights;
    std::optional<ModalLinearWeights> teacher_projection;

    void validate(const VideoPipelineConfig& config) const;
};

struct VideoPipelineModules final {
    std::shared_ptr<const MosaicOmniModalFrontends> frontends;
    std::shared_ptr<const LearnedVideoObjectTracker> object_tracker;
    std::shared_ptr<const QueryConditionedObjectTrajectoryBinding> trajectory_binding;
    std::shared_ptr<const QueryConditionedObjectPairTrajectoryBinding> pair_trajectory_binding;
    std::shared_ptr<const DescriptorConditionedDenseTrajectoryBinding> descriptor_binding;
    std::shared_ptr<const VideoSpatialGeometryReasoner> spatial_geometry;
    std::shared_ptr<const VideoEgomotionReasoner> egomotion;
};

struct VideoPipelineInput final {
    Tensor video_values;
    std::optional<Tensor> camera_pose_values;
    std::optional<Tensor> action_values;
};

struct VideoPrepared final {
    Tensor source_tokens;
    BooleanMask source_mask;
    Tensor temporal_states;
    Tensor temporal_summary;
    Tensor evidence_tokens;
    std::optional<Tensor> temporal_delta;
    std::optional<Tensor> temporal_delta_world;
    std::optional<Tensor> object_slots;
    std::optional<Tensor> normalized_object_slots;
    // Exact values added to object World slots by mosaic_omni.py:4495-4499.
    std::optional<Tensor> object_world_slots;
    std::optional<Tensor> object_binding_features;
    std::optional<Tensor> trajectory_identity;
    std::optional<Tensor> trajectory_event;
    std::optional<Tensor> trajectory_delta;
    std::optional<Tensor> trajectory_geometry;
    std::optional<Tensor> object_attention;
    std::vector<Tensor> dense_object_features;
    std::optional<Tensor> spatial_moment;
    std::optional<Tensor> spatial_y_moment;
    std::optional<Tensor> spatial_features;
    std::optional<Tensor> camera_statistics;
    std::optional<Tensor> camera_pose_states;
    std::optional<Tensor> camera_pose_summary;
    std::optional<Tensor> action_summary;
    std::optional<Tensor> egomotion_summary;
    Tensor world_summary;
};

struct VideoDecisionInput final {
    Tensor post_world;
    std::optional<Tensor> text_query;
    std::optional<Tensor> descriptor_queries; // [B,2,D]
};

struct VideoPipelineOutput final {
    Tensor decision_state;
    Tensor order_logits;
    std::optional<Tensor> object_evidence_weights;
    std::optional<Tensor> object_attention;
    std::optional<Tensor> object_trajectory_weights;
    std::optional<Tensor> object_pair_trajectory_weights;
    std::optional<Tensor> descriptor_trajectory_attention;
    std::optional<Tensor> descriptor_visibility_logits;
    std::optional<Tensor> descriptor_memory_margin;
    std::optional<Tensor> descriptor_memory_reliability_logits;
    std::optional<Tensor> camera_robustness_gate;
    std::optional<Tensor> spatial_relation_logits;
    std::optional<Tensor> egomotion_logits;
    std::optional<Tensor> egomotion_validity_logits;
    std::optional<Tensor> egomotion_motion_evidence;
    std::optional<BooleanMask> egomotion_sufficient_mask;
    std::optional<Tensor> teacher_embedding;
};

class MosaicOmniVideoPipeline final {
public:
    MosaicOmniVideoPipeline(VideoPipelineConfig config,
                            VideoPipelineWeights weights,
                            VideoPipelineModules modules);

    [[nodiscard]] VideoPrepared prepare(const VideoPipelineInput& input) const;
    [[nodiscard]] VideoPipelineOutput decide(
        const VideoPipelineInput& source, const VideoPrepared& prepared,
        const VideoDecisionInput& decision) const;

private:
    VideoPipelineConfig config_;
    VideoPipelineWeights weights_;
    VideoPipelineModules modules_;
};

}  // namespace swegca::world
