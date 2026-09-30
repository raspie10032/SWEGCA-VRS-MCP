#pragma once

#include "world/modal_to_world.hpp"
#include "world/mosaic_omni_cross_modal.hpp"
#include "world/mosaic_omni_modal_frontends.hpp"
#include "world/mosaic_omni_narrative.hpp"
#include "world/mosaic_omni_text_pipeline.hpp"
#include "world/mosaic_omni_visual.hpp"
#include "world/mosaic_omni_video_pipeline.hpp"
#include "world/mosaic_omni_world_pipeline.hpp"

#include <map>
#include <string>
#include <string_view>

namespace swegca::world {

using MosaicStateDict = std::map<std::string, Tensor, std::less<>>;

inline constexpr std::string_view mosaic_unified_materialization_scope =
    "all state-dict-owned unified modules, including the video pipeline";

[[nodiscard]] MosaicOmniModalFrontendWeights materialize_modal_frontends(
    const MosaicStateDict&, const MosaicOmniModalFrontendConfig&);
[[nodiscard]] ModalToWorldWeights materialize_modal_to_world(
    const MosaicStateDict&, const ModalToWorldConfig&, std::string_view prefix = "to_world");
[[nodiscard]] VisualTeacherSlotBridgeWeights materialize_visual_teacher_slot_bridge(
    const MosaicStateDict&, std::size_t world_dim, std::size_t rank);
[[nodiscard]] ExplicitObjectRelationGrounderWeights materialize_explicit_grounder(
    const MosaicStateDict&, std::size_t world_dim, std::size_t rank);
[[nodiscard]] ExplicitRelationHeadWeights materialize_explicit_relation_head(
    const MosaicStateDict&, std::size_t dimension, std::size_t classes);
[[nodiscard]] MosaicOmniWorldPipelineWeights materialize_world_pipeline(
    const MosaicStateDict&, const MosaicUnifiedConfig&,
    const MosaicOmniWorldPipelineConfig&);
[[nodiscard]] CrossModalEvidenceWeights materialize_cross_modal_evidence(
    const MosaicStateDict&, const MosaicUnifiedConfig&);
[[nodiscard]] MosaicOmniTextPipelineWeights materialize_text_pipeline(
    const MosaicStateDict&, const MosaicUnifiedConfig&,
    const MosaicOmniTextPipelineConfig&);
[[nodiscard]] TextOnlyCrossMemoryWeights materialize_text_cross_memory(
    const MosaicStateDict&, std::size_t model_dim);
[[nodiscard]] TextOnlyHiddenCrossMemoryWeights materialize_text_hidden_cross_memory(
    const MosaicStateDict&, std::size_t model_dim);
[[nodiscard]] TextEpistemicOutputWeights materialize_text_epistemic_output(
    const MosaicStateDict&, std::size_t model_dim, std::size_t rank);
[[nodiscard]] TextAnswerabilityWeights materialize_text_answerability(
    const MosaicStateDict&, const TextAnswerabilityConfig&);
[[nodiscard]] NarrativeContinuityWeights materialize_narrative_weights(
    const MosaicStateDict&, std::size_t text_dim, std::size_t hidden_dim,
    std::size_t world_dim);
[[nodiscard]] LongVideoWorldWeights materialize_long_video_weights(
    const MosaicStateDict&, const MosaicOmniConfig&, bool transition_features);
[[nodiscard]] WorldToAnimaWeights materialize_world_to_anima_weights(
    const MosaicStateDict&, const MosaicOmniConfig&,
    std::string_view prefix = {});

struct VideoPipelineMaterializedWeights final {
    VideoPipelineWeights pipeline;
    std::optional<VideoObjectTrackerWeights> object_tracker;
    std::optional<ObjectTrajectoryBindingWeights> trajectory;
    std::optional<ObjectTrajectoryBindingWeights> pair_trajectory;
    std::optional<DescriptorConditionedDenseTrajectoryWeights> descriptor_trajectory;
    std::optional<VideoSpatialGeometryWeights> spatial_geometry;
    std::optional<VideoEgomotionWeights> egomotion;
};

[[nodiscard]] VideoPipelineMaterializedWeights materialize_video_pipeline(
    const MosaicStateDict&, const MosaicUnifiedConfig&);

struct MosaicUnifiedMaterializedWeights final {
    struct RuntimeBoundaryWeights final {
        std::vector<double> world_norm_weight;
        std::vector<double> world_norm_bias;
        std::optional<ModalLinearWeights> visual_text_retrieval_projection;
        std::optional<ModalLinearWeights> text_visual_retrieval_projection;
    } runtime;
    MosaicOmniModalFrontendWeights frontends;
    std::optional<ModalToWorldWeights> to_world;
    std::optional<VisualTeacherSlotBridgeWeights> visual_teacher;
    std::optional<ExplicitObjectRelationGrounderWeights> relation_grounder;
    std::optional<ExplicitRelationHeadWeights> relation_head;
    MosaicOmniWorldPipelineWeights world_pipeline;
    std::optional<CrossModalEvidenceWeights> cross_modal;
    MosaicOmniTextPipelineWeights text_pipeline;
    std::optional<TextOnlyCrossMemoryWeights> text_cross_memory;
    std::optional<TextOnlyHiddenCrossMemoryWeights> text_hidden_cross_memory;
    std::optional<TextEpistemicOutputWeights> text_epistemic_output;
    std::optional<TextAnswerabilityWeights> text_answerability;
    std::optional<NarrativeContinuityWeights> narrative;
    std::optional<LongVideoWorldWeights> long_video;
    VideoPipelineMaterializedWeights video;
};

// Single topology-aware entry point. Every enabled module is materialized and
// every disabled module prefix is rejected before the bundle is returned.
[[nodiscard]] MosaicUnifiedMaterializedWeights materialize_mosaic_unified(
    const MosaicStateDict&, const MosaicUnifiedConfig&);

void require_disabled_state_dict_prefix(const MosaicStateDict&, std::string_view prefix);

}  // namespace swegca::world
