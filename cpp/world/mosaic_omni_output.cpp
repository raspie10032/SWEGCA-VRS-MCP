#include "world/mosaic_omni_output.hpp"

#include <utility>

namespace swegca::world {

MosaicUnifiedOutput assemble_mosaic_omni_output(MosaicOmniOutputInputs inputs) {
    return MosaicUnifiedOutput{
        .text = std::move(inputs.text),
        .world_state = std::move(inputs.world_state),
        .modalities = std::move(inputs.modalities),
        .video_order_logits = std::move(inputs.video_order_logits),
        .video_object_evidence_weights = std::move(inputs.video_object_evidence_weights),
        .video_object_attention = std::move(inputs.video_object_attention),
        .video_object_trajectory_weights = std::move(inputs.video_object_trajectory_weights),
        .video_object_pair_trajectory_weights =
            std::move(inputs.video_object_pair_trajectory_weights),
        .video_descriptor_trajectory_attention =
            std::move(inputs.video_descriptor_trajectory_attention),
        .video_descriptor_visibility_logits =
            std::move(inputs.video_descriptor_visibility_logits),
        .video_descriptor_memory_margin =
            std::move(inputs.video_descriptor_memory_margin),
        .video_descriptor_memory_reliability_logits =
            std::move(inputs.video_descriptor_memory_reliability_logits),
        .video_camera_robustness_gate = std::move(inputs.video_camera_robustness_gate),
        .video_spatial_relation_logits = std::move(inputs.video_spatial_relation_logits),
        .video_egomotion_logits = std::move(inputs.video_egomotion_logits),
        .video_egomotion_validity_logits =
            std::move(inputs.video_egomotion_validity_logits),
        .video_egomotion_motion_evidence =
            std::move(inputs.video_egomotion_motion_evidence),
        .video_egomotion_sufficient_mask =
            std::move(inputs.video_egomotion_sufficient_mask),
        .audio_temporal_logits = std::move(inputs.audio_temporal_logits),
        .video_embedding = std::move(inputs.video_embedding),
        .audio_embedding = std::move(inputs.audio_embedding),
        .visual_embedding = std::move(inputs.visual_embedding),
        .text_retrieval_embedding = std::move(inputs.text_retrieval_embedding),
        .video_teacher_embedding = std::move(inputs.video_teacher_embedding),
        .audio_teacher_embedding = std::move(inputs.audio_teacher_embedding),
        .audio_teacher_temporal_states =
            std::move(inputs.audio_teacher_temporal_states),
        .audio_world_teacher_embedding =
            std::move(inputs.audio_world_teacher_embedding),
        .audio_ctc_logits = std::move(inputs.audio_ctc_logits),
        .audio_grapheme_ctc_logits = std::move(inputs.audio_grapheme_ctc_logits),
        .audio_text_retrieval_embedding =
            std::move(inputs.audio_text_retrieval_embedding),
        .text_audio_retrieval_embedding =
            std::move(inputs.text_audio_retrieval_embedding),
        .cross_modal_evidence_logits = std::move(inputs.cross_modal_evidence_logits),
        .cross_modal_evidence_delta = std::move(inputs.cross_modal_evidence_delta),
        .visual_text_retrieval_embedding =
            std::move(inputs.visual_text_retrieval_embedding),
        .text_visual_retrieval_embedding =
            std::move(inputs.text_visual_retrieval_embedding),
        .answerability_logits = std::move(inputs.answerability_logits),
        .answerability_loss = std::move(inputs.answerability_loss),
        .explicit_relation_logits = std::move(inputs.explicit_relation_logits),
        .explicit_object_attention = std::move(inputs.explicit_object_attention),
    };
}

}  // namespace swegca::world
