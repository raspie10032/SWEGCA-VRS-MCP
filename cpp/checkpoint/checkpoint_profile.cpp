#include "checkpoint/checkpoint_profile.hpp"

#include <array>
#include <limits>

namespace swegca::checkpoint {
namespace {

constexpr std::array<std::uint64_t, 2> kTeacherShape{512ULL, 256ULL};
constexpr std::array<std::int64_t, 2> kTeacherStride{1, 512};

constexpr std::array<std::string_view, 368> kModelKeys{{
    "text_core.workspace",
    "text_core.bos_patch",
    "text_core.round_embedding",
    "text_core.byte_embedding.weight",
    "text_core.patch_projection.weight",
    "text_core.patch_projection.bias",
    "text_core.patch_norm.weight",
    "text_core.patch_norm.bias",
    "text_core.segment_embedding.weight",
    "text_core.retriever_projection.weight",
    "text_core.retriever_projection.bias",
    "text_core.blocks.0.self_attn.in_proj_weight",
    "text_core.blocks.0.self_attn.in_proj_bias",
    "text_core.blocks.0.self_attn.out_proj.weight",
    "text_core.blocks.0.self_attn.out_proj.bias",
    "text_core.blocks.0.linear1.weight",
    "text_core.blocks.0.linear1.bias",
    "text_core.blocks.0.linear2.weight",
    "text_core.blocks.0.linear2.bias",
    "text_core.blocks.0.norm1.weight",
    "text_core.blocks.0.norm1.bias",
    "text_core.blocks.0.norm2.weight",
    "text_core.blocks.0.norm2.bias",
    "text_core.blocks.1.self_attn.in_proj_weight",
    "text_core.blocks.1.self_attn.in_proj_bias",
    "text_core.blocks.1.self_attn.out_proj.weight",
    "text_core.blocks.1.self_attn.out_proj.bias",
    "text_core.blocks.1.linear1.weight",
    "text_core.blocks.1.linear1.bias",
    "text_core.blocks.1.linear2.weight",
    "text_core.blocks.1.linear2.bias",
    "text_core.blocks.1.norm1.weight",
    "text_core.blocks.1.norm1.bias",
    "text_core.blocks.1.norm2.weight",
    "text_core.blocks.1.norm2.bias",
    "text_core.blocks.2.self_attn.in_proj_weight",
    "text_core.blocks.2.self_attn.in_proj_bias",
    "text_core.blocks.2.self_attn.out_proj.weight",
    "text_core.blocks.2.self_attn.out_proj.bias",
    "text_core.blocks.2.linear1.weight",
    "text_core.blocks.2.linear1.bias",
    "text_core.blocks.2.linear2.weight",
    "text_core.blocks.2.linear2.bias",
    "text_core.blocks.2.norm1.weight",
    "text_core.blocks.2.norm1.bias",
    "text_core.blocks.2.norm2.weight",
    "text_core.blocks.2.norm2.bias",
    "text_core.blocks.3.self_attn.in_proj_weight",
    "text_core.blocks.3.self_attn.in_proj_bias",
    "text_core.blocks.3.self_attn.out_proj.weight",
    "text_core.blocks.3.self_attn.out_proj.bias",
    "text_core.blocks.3.linear1.weight",
    "text_core.blocks.3.linear1.bias",
    "text_core.blocks.3.linear2.weight",
    "text_core.blocks.3.linear2.bias",
    "text_core.blocks.3.norm1.weight",
    "text_core.blocks.3.norm1.bias",
    "text_core.blocks.3.norm2.weight",
    "text_core.blocks.3.norm2.bias",
    "text_core.blocks.4.self_attn.in_proj_weight",
    "text_core.blocks.4.self_attn.in_proj_bias",
    "text_core.blocks.4.self_attn.out_proj.weight",
    "text_core.blocks.4.self_attn.out_proj.bias",
    "text_core.blocks.4.linear1.weight",
    "text_core.blocks.4.linear1.bias",
    "text_core.blocks.4.linear2.weight",
    "text_core.blocks.4.linear2.bias",
    "text_core.blocks.4.norm1.weight",
    "text_core.blocks.4.norm1.bias",
    "text_core.blocks.4.norm2.weight",
    "text_core.blocks.4.norm2.bias",
    "text_core.blocks.5.self_attn.in_proj_weight",
    "text_core.blocks.5.self_attn.in_proj_bias",
    "text_core.blocks.5.self_attn.out_proj.weight",
    "text_core.blocks.5.self_attn.out_proj.bias",
    "text_core.blocks.5.linear1.weight",
    "text_core.blocks.5.linear1.bias",
    "text_core.blocks.5.linear2.weight",
    "text_core.blocks.5.linear2.bias",
    "text_core.blocks.5.norm1.weight",
    "text_core.blocks.5.norm1.bias",
    "text_core.blocks.5.norm2.weight",
    "text_core.blocks.5.norm2.bias",
    "text_core.blocks.6.self_attn.in_proj_weight",
    "text_core.blocks.6.self_attn.in_proj_bias",
    "text_core.blocks.6.self_attn.out_proj.weight",
    "text_core.blocks.6.self_attn.out_proj.bias",
    "text_core.blocks.6.linear1.weight",
    "text_core.blocks.6.linear1.bias",
    "text_core.blocks.6.linear2.weight",
    "text_core.blocks.6.linear2.bias",
    "text_core.blocks.6.norm1.weight",
    "text_core.blocks.6.norm1.bias",
    "text_core.blocks.6.norm2.weight",
    "text_core.blocks.6.norm2.bias",
    "text_core.blocks.7.self_attn.in_proj_weight",
    "text_core.blocks.7.self_attn.in_proj_bias",
    "text_core.blocks.7.self_attn.out_proj.weight",
    "text_core.blocks.7.self_attn.out_proj.bias",
    "text_core.blocks.7.linear1.weight",
    "text_core.blocks.7.linear1.bias",
    "text_core.blocks.7.linear2.weight",
    "text_core.blocks.7.linear2.bias",
    "text_core.blocks.7.norm1.weight",
    "text_core.blocks.7.norm1.bias",
    "text_core.blocks.7.norm2.weight",
    "text_core.blocks.7.norm2.bias",
    "text_core.operator_adapter.left",
    "text_core.operator_adapter.right",
    "text_core.local_decoder.weight_ih",
    "text_core.local_decoder.weight_hh",
    "text_core.local_decoder.bias_ih",
    "text_core.local_decoder.bias_hh",
    "text_core.output_norm.weight",
    "text_core.output_norm.bias",
    "text_core.lm_head.weight",
    "text_core.lm_head.bias",
    "text_to_world.weight",
    "text_to_world.bias",
    "text_only_to_world.weight",
    "text_only_to_world.bias",
    "vision_frontend.weight",
    "vision_frontend.bias",
    "visual_semantic_frontend.weight",
    "visual_semantic_frontend.bias",
    "visual_position.weight",
    "visual_semantic_cell.self_attn.in_proj_weight",
    "visual_semantic_cell.self_attn.in_proj_bias",
    "visual_semantic_cell.self_attn.out_proj.weight",
    "visual_semantic_cell.self_attn.out_proj.bias",
    "visual_semantic_cell.linear1.weight",
    "visual_semantic_cell.linear1.bias",
    "visual_semantic_cell.linear2.weight",
    "visual_semantic_cell.linear2.bias",
    "visual_semantic_cell.norm1.weight",
    "visual_semantic_cell.norm1.bias",
    "visual_semantic_cell.norm2.weight",
    "visual_semantic_cell.norm2.bias",
    "visual_semantic_norm.weight",
    "visual_semantic_norm.bias",
    "explicit_object_relation_grounder.descriptor_norm.weight",
    "explicit_object_relation_grounder.descriptor_norm.bias",
    "explicit_object_relation_grounder.patch_norm.weight",
    "explicit_object_relation_grounder.patch_norm.bias",
    "explicit_object_relation_grounder.query.weight",
    "explicit_object_relation_grounder.key.weight",
    "explicit_object_relation_grounder.value.weight",
    "explicit_object_relation_grounder.value.bias",
    "explicit_object_relation_grounder.object_up.weight",
    "explicit_object_relation_grounder.object_up.bias",
    "explicit_object_relation_grounder.relation_norm.weight",
    "explicit_object_relation_grounder.relation_norm.bias",
    "explicit_object_relation_grounder.relation_down.weight",
    "explicit_object_relation_grounder.relation_down.bias",
    "explicit_object_relation_grounder.relation_up.weight",
    "explicit_object_relation_grounder.relation_up.bias",
    "explicit_relation_head.norm.weight",
    "explicit_relation_head.norm.bias",
    "explicit_relation_head.output.weight",
    "explicit_relation_head.output.bias",
    "audio_frontend.weight",
    "audio_frontend.bias",
    "audio_temporal_cell.weight_ih_l0",
    "audio_temporal_cell.weight_hh_l0",
    "audio_temporal_cell.bias_ih_l0",
    "audio_temporal_cell.bias_hh_l0",
    "audio_temporal_to_world.weight",
    "audio_temporal_to_world.bias",
    "audio_content_temporal_cell.weight_ih_l0",
    "audio_content_temporal_cell.weight_hh_l0",
    "audio_content_temporal_cell.bias_ih_l0",
    "audio_content_temporal_cell.bias_hh_l0",
    "audio_content_to_world.weight",
    "audio_content_to_world.bias",
    "audio_spectral_projection.weight",
    "audio_spectral_projection.bias",
    "audio_event_slot_projection.weight",
    "audio_event_slot_projection.bias",
    "audio_ctc_projection.weight",
    "audio_ctc_projection.bias",
    "audio_grapheme_ctc_projection.weight",
    "audio_grapheme_ctc_projection.bias",
    "audio_text_retrieval_projection.weight",
    "text_audio_retrieval_projection.weight",
    "narrative_continuity_head.0.weight",
    "narrative_continuity_head.0.bias",
    "narrative_continuity_head.2.weight",
    "narrative_continuity_head.2.bias",
    "narrative_evidence_to_world.weight",
    "audio_temporal_head.0.weight",
    "audio_temporal_head.0.bias",
    "audio_temporal_head.1.weight",
    "audio_temporal_head.1.bias",
    "video_time.weight",
    "video_temporal_mixer.weight",
    "video_temporal_cell.weight_ih_l0",
    "video_temporal_cell.weight_hh_l0",
    "video_temporal_cell.bias_ih_l0",
    "video_temporal_cell.bias_hh_l0",
    "video_temporal_to_world.weight",
    "video_temporal_to_world.bias",
    "video_temporal_delta_to_world.weight",
    "video_object_temporal_cell.weight_ih_l0",
    "video_object_temporal_cell.weight_hh_l0",
    "video_object_temporal_cell.bias_ih_l0",
    "video_object_temporal_cell.bias_hh_l0",
    "video_object_frontend.weight",
    "video_object_frontend.bias",
    "video_object_to_world.weight",
    "video_object_to_world.bias",
    "video_object_statistics.weight",
    "video_object_statistics.bias",
    "video_object_decision.0.weight",
    "video_object_decision.0.bias",
    "video_object_decision.1.weight",
    "video_object_decision.1.bias",
    "video_query_conditioning.weight",
    "video_query_conditioning.bias",
    "modality_embedding.weight",
    "to_world.world_queries",
    "to_world.source_projection.0.weight",
    "to_world.source_projection.0.bias",
    "to_world.source_projection.1.weight",
    "to_world.source_projection.1.bias",
    "to_world.cross_attention.in_proj_weight",
    "to_world.cross_attention.in_proj_bias",
    "to_world.cross_attention.out_proj.weight",
    "to_world.cross_attention.out_proj.bias",
    "to_world.output_norm.weight",
    "to_world.output_norm.bias",
    "world_cell.self_attn.in_proj_weight",
    "world_cell.self_attn.in_proj_bias",
    "world_cell.self_attn.out_proj.weight",
    "world_cell.self_attn.out_proj.bias",
    "world_cell.linear1.weight",
    "world_cell.linear1.bias",
    "world_cell.linear2.weight",
    "world_cell.linear2.bias",
    "world_cell.norm1.weight",
    "world_cell.norm1.bias",
    "world_cell.norm2.weight",
    "world_cell.norm2.bias",
    "world_norm.weight",
    "world_norm.bias",
    "world_to_text_memory.weight",
    "world_to_text_memory.bias",
    "text_only_world_to_text_memory.weight",
    "text_only_world_to_text_memory.bias",
    "text_only_logit_adapter.0.weight",
    "text_only_logit_adapter.0.bias",
    "text_only_logit_adapter.2.weight",
    "text_only_logit_adapter.2.bias",
    "text_only_cross_memory.query.weight",
    "text_only_cross_memory.query.bias",
    "text_only_cross_memory.cross_attention.in_proj_weight",
    "text_only_cross_memory.cross_attention.in_proj_bias",
    "text_only_cross_memory.cross_attention.out_proj.weight",
    "text_only_cross_memory.cross_attention.out_proj.bias",
    "text_only_cross_memory.output.weight",
    "text_only_cross_memory.output.bias",
    "text_answerability_head.input_projection.0.weight",
    "text_answerability_head.input_projection.0.bias",
    "text_answerability_head.input_projection.1.weight",
    "text_answerability_head.input_projection.1.bias",
    "text_answerability_head.question_norm.weight",
    "text_answerability_head.question_norm.bias",
    "text_answerability_head.evidence_norm.weight",
    "text_answerability_head.evidence_norm.bias",
    "text_answerability_head.cross_attention.in_proj_weight",
    "text_answerability_head.cross_attention.in_proj_bias",
    "text_answerability_head.cross_attention.out_proj.weight",
    "text_answerability_head.cross_attention.out_proj.bias",
    "text_answerability_head.token_projection.0.weight",
    "text_answerability_head.token_projection.0.bias",
    "text_answerability_head.token_projection.1.weight",
    "text_answerability_head.token_projection.1.bias",
    "text_answerability_head.output.0.weight",
    "text_answerability_head.output.0.bias",
    "text_answerability_head.output.1.weight",
    "text_answerability_head.output.1.bias",
    "text_answerability_head.output.3.weight",
    "text_answerability_head.output.3.bias",
    "video_order_head.0.weight",
    "video_order_head.0.bias",
    "video_order_head.1.weight",
    "video_order_head.1.bias",
    "video_teacher_projection.weight",
    "video_teacher_projection.bias",
    "audio_teacher_projection.weight",
    "audio_teacher_projection.bias",
    "video_object_tracker.queries",
    "video_object_tracker.query_norm.weight",
    "video_object_tracker.query_norm.bias",
    "video_object_tracker.token_norm.weight",
    "video_object_tracker.token_norm.bias",
    "video_object_tracker.position_projection.weight",
    "video_object_binding_decision.0.weight",
    "video_object_binding_decision.0.bias",
    "video_object_binding_decision.1.weight",
    "video_object_binding_decision.1.bias",
    "video_spatial_temporal_moment_to_world.weight",
    "video_spatial_temporal_moment_gate.weight",
    "video_spatial_temporal_moment_gate.bias",
    "video_spatial_temporal_y_moment_to_world.weight",
    "video_spatial_temporal_y_moment_gate.weight",
    "video_spatial_temporal_y_moment_gate.bias",
    "video_descriptor_trajectory_binding.feature_norm.weight",
    "video_descriptor_trajectory_binding.feature_norm.bias",
    "video_descriptor_trajectory_binding.query.weight",
    "video_descriptor_trajectory_binding.key.weight",
    "video_descriptor_trajectory_binding.trajectory_norm.weight",
    "video_descriptor_trajectory_binding.trajectory_norm.bias",
    "video_descriptor_trajectory_binding.value.weight",
    "video_descriptor_trajectory_binding.output.weight",
    "video_descriptor_trajectory_binding.memory_visibility.weight",
    "video_descriptor_trajectory_binding.memory_visibility.bias",
    "video_descriptor_trajectory_binding.memory_output.weight",
    "video_descriptor_trajectory_binding.memory_temporal_relative_output.weight",
    "video_object_evidence_gate.weight",
    "video_object_evidence_gate.bias",
    "video_camera_pose_encoder.weight",
    "video_camera_pose_encoder.bias",
    "video_spatial_geometry_reasoner.frame.0.weight",
    "video_spatial_geometry_reasoner.frame.0.bias",
    "video_spatial_geometry_reasoner.frame.1.weight",
    "video_spatial_geometry_reasoner.frame.1.bias",
    "video_spatial_geometry_reasoner.frame.3.weight",
    "video_spatial_geometry_reasoner.frame.3.bias",
    "video_spatial_geometry_reasoner.stereo_pair.0.weight",
    "video_spatial_geometry_reasoner.stereo_pair.0.bias",
    "video_spatial_geometry_reasoner.stereo_pair.1.weight",
    "video_spatial_geometry_reasoner.stereo_pair.1.bias",
    "video_spatial_geometry_reasoner.stereo_pair.3.weight",
    "video_spatial_geometry_reasoner.stereo_pair.3.bias",
    "video_spatial_relation_head.0.weight",
    "video_spatial_relation_head.0.bias",
    "video_spatial_relation_head.1.weight",
    "video_spatial_relation_head.1.bias",
    "video_spatial_relation_head.3.weight",
    "video_spatial_relation_head.3.bias",
    "video_action_encoder.weight",
    "video_action_encoder.bias",
    "video_egomotion_reasoner.frontend.0.weight",
    "video_egomotion_reasoner.frontend.0.bias",
    "video_egomotion_reasoner.frontend.2.weight",
    "video_egomotion_reasoner.frontend.2.bias",
    "video_egomotion_reasoner.frontend.4.weight",
    "video_egomotion_reasoner.frontend.4.bias",
    "video_egomotion_reasoner.output.weight",
    "video_egomotion_reasoner.output.bias",
    "video_egomotion_head.0.weight",
    "video_egomotion_head.0.bias",
    "video_egomotion_head.1.weight",
    "video_egomotion_head.1.bias",
    "video_egomotion_head.3.weight",
    "video_egomotion_head.3.bias",
    "cross_modal_text_projection.weight",
    "cross_modal_audio_projection.weight",
    "cross_modal_video_projection.weight",
    "cross_modal_evidence_to_world.weight",
    "cross_modal_evidence_to_world.bias",
    "cross_modal_evidence_norm.running_mean",
    "cross_modal_evidence_norm.running_var",
    "cross_modal_evidence_norm.num_batches_tracked",
    "cross_modal_evidence_head.0.weight",
    "cross_modal_evidence_head.0.bias",
    "cross_modal_evidence_head.1.weight",
    "cross_modal_evidence_head.1.bias"
}};

constexpr std::array<DTypeProfile, 2> kNarrativeDTypes{{
    DTypeProfile{TensorDType::float16, 1ULL, 131072ULL},
    DTypeProfile{TensorDType::float32, 290ULL, 134084034ULL}
}};

constexpr std::array<DTypeProfile, 3> kCognitionDTypes{{
    DTypeProfile{TensorDType::float16, 1ULL, 131072ULL},
    DTypeProfile{TensorDType::float32, 259ULL, 131859648ULL},
    DTypeProfile{TensorDType::bfloat16, 97ULL, 3657050ULL}
}};

constexpr std::array<DTypeProfile, 4> kSynapseDTypes{{
    DTypeProfile{TensorDType::float16, 1ULL, 131072ULL},
    DTypeProfile{TensorDType::float32, 270ULL, 131935042ULL},
    DTypeProfile{TensorDType::bfloat16, 97ULL, 3657050ULL},
    DTypeProfile{TensorDType::int64, 1ULL, 1ULL}
}};

constexpr std::array<CheckpointProfile, 3> kProfiles{{
    CheckpointProfile{
        "mosaic_unified_narrative_evidence.pt",
        "/var/home/raspie/Documents/Codex/tinylm slicer/outputs/rozephine_mosaic_v778_narrative_evidence_v794/mosaic_unified_narrative_evidence.pt",
        "041dfd03d9b89608ef093e31b95b7527f8151b794b4d3ffdd45fb45d4bf46847",
        536734499ULL,
        "f697da47da95cf3fafeb14aa1521a184aeb1733b211b0b7e5c44456b10919c05",
        "1105179219259957249211771682924932388579",
        "fb5e469ec8a630335b4058c31f8e869cac7bf668841a3d7814af552a2a03ea3e",
        "758b6ada7b139a4a6a151de1f0a96672046c2a1cfed8b07c6333da17815adc0c",
        std::span<const std::string_view>{kModelKeys.data(), 290},
        kNarrativeDTypes,
        290ULL,
        291ULL,
        134084034ULL,
        291ULL,
        TensorLayout{TensorDType::float16, kTeacherShape, kTeacherStride, 0ULL, 131072ULL},
    },
    CheckpointProfile{
        "rozephine_single_state.pt",
        "/var/home/raspie/Documents/Codex/tinylm slicer/outputs/rozephine/integration/single_state_cognition_2026-08-02/rozephine_single_state.pt",
        "56e25cf3cb60bbffe4351efca80cfb79abb9e858fb32d9931273de509c4715eb",
        535171130ULL,
        "3ed1e4ea9eb63823128171cd8a066b8aced10dedd6f0f037459beef4836fc225",
        "0108919483468148159004597956934962839827",
        "452c6151b14392aba08aab317677c9128d9e68abb90f89a7774905c203f8c8e0",
        "e609ad724b739eefc4b0300d8ac7495af96bf2823b05e3420fe91880a8655602",
        std::span<const std::string_view>{kModelKeys.data(), 356},
        kCognitionDTypes,
        356ULL,
        357ULL,
        135516698ULL,
        357ULL,
        TensorLayout{TensorDType::float16, kTeacherShape, kTeacherStride, 0ULL, 131072ULL},
    },
    CheckpointProfile{
        "rozephine_single_state_synapse.pt",
        "/var/home/raspie/Documents/Codex/tinylm slicer/outputs/rozephine/integration/single_state_synapse_2026-08-02/rozephine_single_state_synapse.pt",
        "f922cea14b9cd1061ab5c24a5f54025948c288ee24a6a746577efbf14d5c1b9b",
        535479950ULL,
        "021ee616286f3339002ba8cdd0b75f3dcc4fec9029a0efa7962befcd90ab4c6d",
        "1160916860417045845105910716154717562051",
        "d08834ca25fd4bb67b1c4697561e1cf593922b6abfa016f99cab62732d5f2cc4",
        "b0a67b495d4fe9a798cdcc999c9024cd865900115dd79f250317249a0f9aa6d0",
        std::span<const std::string_view>{kModelKeys.data(), 368},
        kSynapseDTypes,
        368ULL,
        369ULL,
        135592093ULL,
        369ULL,
        TensorLayout{TensorDType::float16, kTeacherShape, kTeacherStride, 0ULL, 131072ULL},
    }
}};

[[nodiscard]] constexpr bool multiply(std::uint64_t left, std::uint64_t right,
                                      std::uint64_t& result) noexcept {
    if (left != 0 && right > std::numeric_limits<std::uint64_t>::max() / left) {
        return false;
    }
    result = left * right;
    return true;
}

[[nodiscard]] constexpr bool add(std::uint64_t left, std::uint64_t right,
                                 std::uint64_t& result) noexcept {
    if (right > std::numeric_limits<std::uint64_t>::max() - left) {
        return false;
    }
    result = left + right;
    return true;
}

[[nodiscard]] constexpr bool hex_digest(std::string_view value) noexcept {
    if (value.size() != 64) {
        return false;
    }
    for (const char c : value) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            return false;
        }
    }
    return true;
}

}  // namespace

std::uint64_t dtype_size(const TensorDType dtype) noexcept {
    switch (dtype) {
    case TensorDType::float16:
    case TensorDType::bfloat16:
        return 2;
    case TensorDType::float32:
        return 4;
    case TensorDType::int64:
        return 8;
    }
    return 0;
}

CheckedTensorSpan check_tensor_span(const TensorLayout& layout) noexcept {
    CheckedTensorSpan checked{};
    const auto item_size = dtype_size(layout.dtype);
    if (item_size == 0) {
        checked.error = TensorLayoutError::unsupported_dtype;
        return checked;
    }
    if (layout.shape.size() != layout.stride.size()) {
        checked.error = TensorLayoutError::rank_mismatch;
        return checked;
    }
    for (const auto stride : layout.stride) {
        if (stride < 0) {
            checked.error = TensorLayoutError::negative_stride;
            return checked;
        }
    }

    std::uint64_t start_byte{};
    if (!multiply(layout.storage_offset_elements, item_size, start_byte)) {
        checked.error = TensorLayoutError::arithmetic_overflow;
        return checked;
    }
    checked.storage_span_start_byte = start_byte;

    bool empty = false;
    std::uint64_t numel = 1;
    for (const auto dimension : layout.shape) {
        if (dimension == 0) {
            empty = true;
            numel = 0;
            break;
        }
        if (!multiply(numel, dimension, numel)) {
            checked.error = TensorLayoutError::arithmetic_overflow;
            return checked;
        }
    }
    checked.numel = numel;
    if (!multiply(numel, item_size, checked.logical_bytes)) {
        checked.error = TensorLayoutError::arithmetic_overflow;
        return checked;
    }

    if (empty) {
        if (layout.storage_offset_elements > layout.storage_elements) {
            checked.error = TensorLayoutError::storage_out_of_bounds;
            return checked;
        }
        checked.storage_span_end_byte_exclusive = start_byte;
        checked.storage_span_bytes = 0;
        checked.contiguous = true;
        return checked;
    }

    std::uint64_t last_element = layout.storage_offset_elements;
    for (std::size_t index = 0; index < layout.shape.size(); ++index) {
        std::uint64_t contribution{};
        if (!multiply(layout.shape[index] - 1,
                      static_cast<std::uint64_t>(layout.stride[index]), contribution) ||
            !add(last_element, contribution, last_element)) {
            checked.error = TensorLayoutError::arithmetic_overflow;
            return checked;
        }
    }
    if (last_element >= layout.storage_elements) {
        checked.error = TensorLayoutError::storage_out_of_bounds;
        return checked;
    }
    std::uint64_t end_element{};
    if (!add(last_element, 1, end_element) ||
        !multiply(end_element, item_size, checked.storage_span_end_byte_exclusive)) {
        checked.error = TensorLayoutError::arithmetic_overflow;
        return checked;
    }
    checked.storage_span_bytes = checked.storage_span_end_byte_exclusive - start_byte;

    checked.contiguous = true;
    std::uint64_t expected_stride = 1;
    for (std::size_t index = layout.shape.size(); index-- > 0;) {
        const auto dimension = layout.shape[index];
        if (dimension > 1 && static_cast<std::uint64_t>(layout.stride[index]) != expected_stride) {
            checked.contiguous = false;
        }
        if (!multiply(expected_stride, dimension, expected_stride)) {
            checked.error = TensorLayoutError::arithmetic_overflow;
            return checked;
        }
    }
    return checked;
}

std::span<const CheckpointProfile> checkpoint_profiles() noexcept {
    return kProfiles;
}

const CheckpointProfile* find_checkpoint_profile(
    const std::string_view checkpoint_sha256) noexcept {
    for (const auto& profile : kProfiles) {
        if (profile.checkpoint_sha256 == checkpoint_sha256) {
            return &profile;
        }
    }
    return nullptr;
}

bool contains_model_key(const CheckpointProfile& profile, const std::string_view key) noexcept {
    for (const auto candidate : profile.model_state_keys) {
        if (candidate == key) {
            return true;
        }
    }
    return false;
}

bool validate_checkpoint_profile(const CheckpointProfile& profile) noexcept {
    if (profile.file_name.empty() || profile.original_path.empty() ||
        !hex_digest(profile.checkpoint_sha256) ||
        !hex_digest(profile.data_pickle_sha256) ||
        profile.serialization_id.size() != 40 ||
        !hex_digest(profile.model_config_canonical_sha256) ||
        !hex_digest(profile.tensor_manifest_sha256) ||
        profile.checkpoint_bytes == 0 ||
        profile.model_state_keys.size() != profile.model_tensor_count ||
        profile.all_tensor_count != profile.model_tensor_count + 1 ||
        profile.storage_record_count != profile.all_tensor_count) {
        return false;
    }
    for (const char digit : profile.serialization_id) {
        if (digit < '0' || digit > '9') return false;
    }

    std::uint64_t tensor_count{};
    std::uint64_t element_count{};
    for (std::size_t index = 0; index < profile.dtype_profile.size(); ++index) {
        const auto& dtype = profile.dtype_profile[index];
        if (dtype_size(dtype.dtype) == 0 || dtype.tensor_count == 0 ||
            !add(tensor_count, dtype.tensor_count, tensor_count) ||
            !add(element_count, dtype.element_count, element_count)) {
            return false;
        }
        for (std::size_t previous = 0; previous < index; ++previous) {
            if (profile.dtype_profile[previous].dtype == dtype.dtype) {
                return false;
            }
        }
    }
    const auto teacher = check_tensor_span(profile.fixed_teacher_projection);
    if (!teacher || tensor_count != profile.all_tensor_count) {
        return false;
    }
    const auto& teacher_layout = profile.fixed_teacher_projection;
    if (teacher_layout.dtype != TensorDType::float16 || teacher_layout.shape.size() != 2 ||
        teacher_layout.shape[0] != 512 || teacher_layout.shape[1] != 256 ||
        teacher_layout.stride.size() != 2 || teacher_layout.stride[0] != 1 ||
        teacher_layout.stride[1] != 512 || teacher_layout.storage_offset_elements != 0 ||
        teacher_layout.storage_elements != 131072 || teacher.numel != 131072 ||
        teacher.logical_bytes != 262144 || teacher.storage_span_start_byte != 0 ||
        teacher.storage_span_end_byte_exclusive != 262144 || teacher.contiguous) {
        return false;
    }
    std::uint64_t expected_elements{};
    if (!add(profile.model_numel, teacher.numel, expected_elements) ||
        element_count != expected_elements) {
        return false;
    }
    for (std::size_t left = 0; left < profile.model_state_keys.size(); ++left) {
        if (profile.model_state_keys[left].empty()) {
            return false;
        }
        for (std::size_t right = left + 1; right < profile.model_state_keys.size(); ++right) {
            if (profile.model_state_keys[left] == profile.model_state_keys[right]) {
                return false;
            }
        }
    }
    return true;
}

}  // namespace swegca::checkpoint
