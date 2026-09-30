#pragma once

#include "world/mosaic_omni.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace swegca::world {

inline constexpr std::string_view mosaic_omni_output_source_sha256 =
    "a06f7a85128827b036a7c1801920e570e187c0163df6c3851d85ef2dd3bb5e20";
inline constexpr std::string_view mosaic_omni_output_source_lines = "5467-5621";

// All projections and conditional selections in the source forward pass are
// completed before this boundary. This structure carries only final values.
struct MosaicOmniOutputInputs final {
    MosaicTextOutput text;
    WorldState world_state;
    std::vector<std::string> modalities;
    std::optional<Tensor> video_order_logits;
    std::optional<Tensor> video_object_evidence_weights;
    std::optional<Tensor> video_object_attention;
    std::optional<Tensor> video_object_trajectory_weights;
    std::optional<Tensor> video_object_pair_trajectory_weights;
    std::optional<Tensor> video_descriptor_trajectory_attention;
    std::optional<Tensor> video_descriptor_visibility_logits;
    std::optional<Tensor> video_descriptor_memory_margin;
    std::optional<Tensor> video_descriptor_memory_reliability_logits;
    std::optional<Tensor> video_camera_robustness_gate;
    std::optional<Tensor> video_spatial_relation_logits;
    std::optional<Tensor> video_egomotion_logits;
    std::optional<Tensor> video_egomotion_validity_logits;
    std::optional<Tensor> video_egomotion_motion_evidence;
    std::optional<Tensor> video_egomotion_sufficient_mask;
    std::optional<Tensor> audio_temporal_logits;
    std::optional<Tensor> video_embedding;
    std::optional<Tensor> audio_embedding;
    std::optional<Tensor> visual_embedding;
    std::optional<Tensor> text_retrieval_embedding;
    std::optional<Tensor> video_teacher_embedding;
    std::optional<Tensor> audio_teacher_embedding;
    std::optional<Tensor> audio_teacher_temporal_states;
    std::optional<Tensor> audio_world_teacher_embedding;
    std::optional<Tensor> audio_ctc_logits;
    std::optional<Tensor> audio_grapheme_ctc_logits;
    std::optional<Tensor> audio_text_retrieval_embedding;
    std::optional<Tensor> text_audio_retrieval_embedding;
    std::optional<Tensor> cross_modal_evidence_logits;
    std::optional<Tensor> cross_modal_evidence_delta;
    std::optional<Tensor> visual_text_retrieval_embedding;
    std::optional<Tensor> text_visual_retrieval_embedding;
    std::optional<Tensor> answerability_logits;
    std::optional<Tensor> answerability_loss;
    std::optional<Tensor> explicit_relation_logits;
    std::optional<Tensor> explicit_object_attention;
};

[[nodiscard]] MosaicUnifiedOutput assemble_mosaic_omni_output(
    MosaicOmniOutputInputs inputs);

}  // namespace swegca::world
