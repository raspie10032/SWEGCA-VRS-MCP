#include "world/mosaic_omni.hpp"

#include <algorithm>
#include <set>
#include <stdexcept>
#include <variant>

namespace swegca::world {
namespace {
const JsonValue* member(const JsonValue::Object& rows, std::string_view key) {
    const auto found=rows.find(key); return found==rows.end()?nullptr:&found->second;
}
std::int64_t integer(const JsonValue::Object& rows,std::string_view key,std::int64_t fallback){
    const auto* value=member(rows,key); if(!value)return fallback;
    const auto* stored=std::get_if<std::int64_t>(&value->storage());
    if(!stored)throw std::invalid_argument("unified config integer type changed"); return *stored;
}
bool boolean(const JsonValue::Object& rows,std::string_view key,bool fallback){
    const auto* value=member(rows,key); if(!value)return fallback;
    const auto* stored=std::get_if<bool>(&value->storage());
    if(!stored)throw std::invalid_argument("unified config boolean type changed"); return *stored;
}
double number(const JsonValue::Object& rows,std::string_view key,double fallback){
    const auto* value=member(rows,key); return value?value->as_number():fallback;
}
std::string text(const JsonValue::Object& rows,std::string_view key,std::string fallback){
    const auto* value=member(rows,key); return value?std::string(value->as_string()):std::move(fallback);
}
std::vector<std::int64_t> integers(const JsonValue::Object& rows,std::string_view key){
    const auto* value=member(rows,key); if(!value)return {};
    std::vector<std::int64_t> result; for(const auto& item:value->as_array()){
        const auto* stored=std::get_if<std::int64_t>(&item.storage());
        if(!stored)throw std::invalid_argument("unified config integer array changed"); result.push_back(*stored);
    } return result;
}
JsonValue::Array integer_array(const std::vector<std::int64_t>& values){
    JsonValue::Array result; for(const auto value:values)result.emplace_back(value); return result;
}
}

void MosaicOmniConfig::validate() const {
    if(!world_slots||!world_dim||!object_slots||!attention_heads||!gemma_hidden_dim||
       !anima_conditioning_tokens||!anima_conditioning_dim)
        throw std::invalid_argument("MOSAIC-OMNI configuration values must be positive");
    if(world_slots!=slot_roles.size())throw std::invalid_argument("world_slots must match frozen roles");
    if(object_slots>world_slots)throw std::invalid_argument("object slots cannot exceed world slots");
    if(world_dim%attention_heads)throw std::invalid_argument("world_dim must be divisible by attention_heads");
}
JsonValue::Object MosaicOmniConfig::to_dict() const { return {
    {"world_slots",static_cast<std::int64_t>(world_slots)}, {"world_dim",static_cast<std::int64_t>(world_dim)},
    {"object_slots",static_cast<std::int64_t>(object_slots)}, {"attention_heads",static_cast<std::int64_t>(attention_heads)},
    {"gemma_hidden_dim",static_cast<std::int64_t>(gemma_hidden_dim)},
    {"anima_conditioning_tokens",static_cast<std::int64_t>(anima_conditioning_tokens)},
    {"anima_conditioning_dim",static_cast<std::int64_t>(anima_conditioning_dim)}}; }

void MosaicUnifiedConfig::validate() const {
    text.validate(); omni.validate();
    const auto require = [](const bool condition, const char* message) {
        if (!condition) throw std::invalid_argument(message);
    };
    require(std::min({vision_patch_size, visual_semantic_rounds, image_visual_adapter_rank,
        visual_teacher_slot_rank, explicit_object_relation_rank, explicit_relation_classes,
        audio_patch_samples, audio_spectral_n_fft, audio_spectral_hop_samples,
        cross_modal_evidence_rank, narrative_evidence_hidden_dim, world_ffn_dim,
        world_rounds}) > 0, "unified model patch and world dimensions must be positive");
    require(vision_teacher_dim >= 0 && audio_teacher_dim >= 0,
        "teacher dimensions must not be negative");
    require(video_camera_pose_dim >= 0 && video_camera_pose_dim <= 32,
        "video camera pose dimension must be in [0, 32]");
    require(video_spatial_relation_classes >= 0 && video_spatial_relation_classes <= 16,
        "video spatial relation classes must be in [0, 16]");
    require(video_action_dim >= 0 && video_action_dim <= 64,
        "video action dimension must be in [0, 64]");
    require(video_egomotion_classes >= 0 && video_egomotion_classes <= 16,
        "video egomotion classes must be in [0, 16]");
    require(!video_egomotion_classes || video_action_dim,
        "video egomotion requires action input");
    require(!video_egomotion_validity_head || video_egomotion_classes,
        "egomotion validity requires egomotion classes");
    require(!video_egomotion_evidence_gate || video_egomotion_classes,
        "egomotion evidence gate requires egomotion classes");
    require(video_egomotion_minimum_motion_evidence >= 0,
        "minimum motion evidence must be non-negative");
    require(static_cast<int>(cross_modal_text_query_pooling) +
        static_cast<int>(cross_modal_text_sequence_pooling) +
        static_cast<int>(cross_modal_text_contextual_pooling) <= 1,
        "cross-modal text pooling modes are mutually exclusive");
    require(audio_grapheme_ctc_vocabulary_size >= 0,
        "grapheme CTC vocabulary size must not be negative");
    require(visual_text_retrieval_dim > 0, "visual/text retrieval dimension must be positive");
    require(!visual_teacher_slot_bridge || visual_teacher_slot_rank <= static_cast<std::int64_t>(omni.world_dim),
        "visual teacher slot rank must fit the world dimension");
    require(!explicit_object_relation_grounder || explicit_object_relation_rank <= static_cast<std::int64_t>(omni.world_dim),
        "object relation rank must fit the world dimension");
    require(!explicit_object_relation_grounder || (omni.object_slots >= 2 && omni.object_slots < omni.world_slots),
        "object relation grounding requires two object slots and one relation slot");
    require(!audio_grapheme_ctc_vocabulary_size || audio_content_encoder,
        "grapheme CTC head requires the content encoder");
    const std::set<std::string_view> answer_modes{"pooled", "token-cross", "contextual-cross",
        "consistency-cross", "body-cross", "core-body-cross", "core-compact-body-cross",
        "core-projected-compact-body-cross", "core-lexical-compact-body-cross",
        "core-lexical-consistency-cross"};
    require(answer_modes.contains(text_answerability_mode), "unsupported text answerability mode");
    require(std::ranges::all_of(text_answerability_fallback_bytes,
        [](const auto value) { return value >= 0 && value <= 255; }),
        "answerability fallback bytes must be raw bytes");
    require(text_answerability_classes >= 2 && text_answerability_classes <= 8,
        "answerability classes must be in [2, 8]");
    require(!text_epistemic_memory_adapter || text_answerability_head,
        "epistemic memory adapter requires answerability head");
    require(text_epistemic_memory_slots >= 1 &&
        text_epistemic_memory_slots <= static_cast<std::int64_t>(omni.world_slots),
        "epistemic memory slots must fit the world workspace");
    require(text_epistemic_output_rank >= 0 &&
        text_epistemic_output_rank <= static_cast<std::int64_t>(text.model_dim),
        "epistemic output rank must fit the text model");
    require(!text_epistemic_output_rank || text_answerability_head,
        "epistemic output adapter requires answerability head");
    require(!audio_temporal_binary_head || audio_temporal_encoder,
        "audio temporal binary head requires temporal audio encoder");
    require(!video_uses_visual_semantic_encoder || visual_semantic_encoder,
        "video semantic frames require the visual semantic encoder");
    require(!image_visual_adapter || (visual_semantic_encoder && visual_semantic_split_frontend),
        "image visual adapter requires the split visual semantic encoder");
    require(image_visual_adapter_scale > 0 && image_visual_adapter_scale <= 1,
        "image visual adapter scale must be in (0, 1]");
    require(video_visual_semantic_scale > 0 && video_visual_semantic_scale <= 1,
        "video visual semantic scale must be in (0, 1]");
    require(video_explicit_temporal_delta_scale > 0,
        "video explicit temporal delta scale must be positive");
    require(!video_separate_temporal_delta_projection || video_explicit_temporal_delta,
        "separate video delta projection requires explicit temporal delta");
    require(!long_video_transition_features || long_video_world_accumulator,
        "long-video transition features require the accumulator");
    require(!audio_spectral_content_frontend || audio_content_encoder,
        "spectral content frontend requires the content encoder");
    require(audio_spectral_hop_samples <= audio_spectral_n_fft,
        "spectral hop must not exceed the FFT window");
    require(audio_text_retrieval_text_source == "world_global" ||
        audio_text_retrieval_text_source == "text_token_mean",
        "unsupported audio/text retrieval text source");
    require(text_epistemic_supported_class >= 0 &&
        text_epistemic_supported_class < text_answerability_classes,
        "epistemic supported class is out of range");
    require(text_epistemic_output_threshold > 0 && text_epistemic_output_threshold <= 1,
        "epistemic output threshold must be in (0, 1]");
    require(text_answerability_threshold > 0 && text_answerability_threshold < 1,
        "answerability threshold must be in (0, 1)");
    const bool object_feature = video_object_frame_normalized_input ||
        video_object_camera_invariant_residual || video_object_time_centered_input ||
        video_object_activity_sorted_slots || video_object_dual_evidence ||
        video_object_learned_queries || video_object_spatial_coordinates ||
        video_object_spatial_event_binding || video_spatial_temporal_moment ||
        video_query_spatial_temporal_moment || video_spatial_temporal_y_moment ||
        video_spatial_temporal_logit_head || video_spatial_temporal_bilinear_head ||
        video_object_trajectory_binding || video_object_pair_trajectory_binding ||
        video_descriptor_trajectory_binding;
    require(!object_feature || video_object_temporal_encoder,
        "object-temporal invariance requires the object encoder");
    require(!video_object_spatial_coordinates || video_object_learned_queries,
        "object spatial coordinates require learned object queries");
    require(!video_object_spatial_event_binding ||
        (video_object_spatial_coordinates && video_object_identity_event_binding),
        "spatial event binding requires spatial queries and identity-event binding");
    require(!video_spatial_temporal_moment || video_object_dual_evidence,
        "spatial-temporal moment requires dual evidence");
    require(!video_query_spatial_temporal_moment ||
        (video_spatial_temporal_moment && video_query_conditioned_head),
        "query spatial-temporal moment requires query-conditioned video and moment");
    require(!video_spatial_temporal_y_moment ||
        (video_spatial_temporal_moment && video_query_spatial_temporal_moment),
        "y spatial-temporal moment requires query-gated x moment");
    require(!video_spatial_temporal_logit_head ||
        (video_spatial_temporal_y_moment && video_query_conditioned_head),
        "spatial-temporal logit head requires xy moments and query conditioning");
    require(!video_spatial_temporal_bilinear_head ||
        (video_spatial_temporal_y_moment && video_query_conditioned_head),
        "spatial-temporal bilinear head requires xy moments and query conditioning");
    const bool trajectory_basis = video_object_dual_evidence && video_object_learned_queries &&
        video_object_spatial_coordinates && video_query_conditioned_head;
    require(!video_object_trajectory_binding || trajectory_basis,
        "object trajectory binding requires dual evidence, spatial learned queries, and query conditioning");
    require(!video_object_pair_trajectory_binding || trajectory_basis,
        "object pair trajectory binding requires dual evidence, spatial learned queries, and query conditioning");
    require(!video_descriptor_trajectory_binding || video_query_conditioned_head,
        "descriptor trajectory binding requires query-conditioned video");
    require(!video_spatial_relation_classes || video_descriptor_trajectory_binding,
        "video spatial relations require descriptor trajectory binding");
    require(!video_descriptor_pair_centered_queries || video_descriptor_trajectory_binding,
        "descriptor pair centering requires descriptor trajectory binding");
    require(!video_descriptor_persistent_identity_state || video_descriptor_trajectory_binding,
        "persistent descriptor identity requires descriptor trajectory binding");
    require(!video_descriptor_object_memory || video_descriptor_trajectory_binding,
        "descriptor object memory requires descriptor trajectory binding");
    require(video_descriptor_object_memory_scale >= 0 && video_descriptor_object_memory_scale <= 1,
        "descriptor object memory scale must be in [0, 1]");
    require(!video_descriptor_object_memory_query_gate || video_descriptor_object_memory,
        "descriptor memory query gate requires object memory");
    require(!video_descriptor_object_memory_reliability_gate || video_descriptor_object_memory,
        "descriptor memory reliability gate requires object memory");
    require(!video_descriptor_object_memory_contrast_visibility || video_descriptor_object_memory,
        "descriptor memory contrast visibility requires object memory");
    require(!video_descriptor_object_memory_evidence_routing ||
        (video_descriptor_object_memory && video_object_dual_evidence),
        "descriptor memory evidence routing requires object memory and dual evidence");
    require(video_descriptor_object_memory_evidence_routing_margin >= 0 &&
        video_descriptor_object_memory_evidence_routing_margin < 1,
        "descriptor memory evidence routing margin must be in [0, 1)");
    require(!video_descriptor_object_memory_evidence_routing_margin ||
        video_descriptor_object_memory_evidence_routing,
        "descriptor memory evidence routing margin requires evidence routing");
    require(!video_descriptor_object_memory_contrast_readout ||
        (video_descriptor_object_memory && video_descriptor_object_memory_contrast_visibility),
        "descriptor memory contrast readout requires contrast visibility");
    require(!video_descriptor_object_memory_temporal_relative_visibility || video_descriptor_object_memory,
        "descriptor memory temporal relative visibility requires object memory");
    require(!video_descriptor_object_memory_temporal_relative_readout ||
        (video_descriptor_object_memory && video_descriptor_object_memory_evidence_routing),
        "descriptor memory temporal relative readout requires object memory and evidence routing");
    require(!(video_descriptor_object_memory_temporal_relative_readout &&
        video_descriptor_object_memory_temporal_relative_visibility),
        "descriptor memory temporal relative readout preserves the parent visibility path and cannot replace it");
    require(!(video_descriptor_object_memory_query_gate &&
        video_descriptor_object_memory_reliability_gate),
        "descriptor memory gates are mutually exclusive");
    require(!video_isolated_identity_descriptors ||
        (video_object_pair_trajectory_binding || video_descriptor_trajectory_binding),
        "isolated identity descriptors require pair or dense binding");
    require(!video_object_dual_evidence || video_query_conditioned_head,
        "dual object evidence requires query-conditioned video");
    require(!video_object_set_decision || video_object_dual_evidence,
        "object set decision requires dual evidence");
    require(!video_camera_robustness_nonlinear_gate || video_camera_robustness_adapter,
        "nonlinear camera gate requires camera robustness adapter");
    require(!video_object_identity_event_binding || video_object_dual_evidence,
        "identity-event binding requires dual evidence");
    require(video_object_frame_normalized_residual_scale >= 0 &&
        video_object_frame_normalized_residual_scale <= 1,
        "object frame-normalized residual scale must be in [0, 1]");
}

JsonValue::Object MosaicUnifiedConfig::to_dict() const {
    JsonValue::Object result{{"schema_version",mosaic_omni_schema_version},{"text",text.to_dict()},{"omni",omni.to_dict()}};
    result.emplace("vision_patch_size", vision_patch_size);
    result.emplace("visual_semantic_encoder", visual_semantic_encoder);
    result.emplace("visual_semantic_split_frontend", visual_semantic_split_frontend);
    result.emplace("visual_semantic_rounds", visual_semantic_rounds);
    result.emplace("image_visual_adapter", image_visual_adapter);
    result.emplace("image_visual_adapter_rank", image_visual_adapter_rank);
    result.emplace("image_visual_adapter_scale", image_visual_adapter_scale);
    result.emplace("visual_teacher_slot_bridge", visual_teacher_slot_bridge);
    result.emplace("visual_teacher_slot_rank", visual_teacher_slot_rank);
    result.emplace("explicit_object_relation_grounder", explicit_object_relation_grounder);
    result.emplace("explicit_object_relation_rank", explicit_object_relation_rank);
    result.emplace("explicit_relation_classes", explicit_relation_classes);
    result.emplace("audio_patch_samples", audio_patch_samples);
    result.emplace("world_ffn_dim", world_ffn_dim);
    result.emplace("world_rounds", world_rounds);
    result.emplace("vision_teacher_dim", vision_teacher_dim);
    result.emplace("audio_teacher_dim", audio_teacher_dim);
    result.emplace("audio_temporal_encoder", audio_temporal_encoder);
    result.emplace("audio_content_encoder", audio_content_encoder);
    result.emplace("audio_spectral_content_frontend", audio_spectral_content_frontend);
    result.emplace("audio_spectral_n_fft", audio_spectral_n_fft);
    result.emplace("audio_spectral_hop_samples", audio_spectral_hop_samples);
    result.emplace("audio_event_slot_injection", audio_event_slot_injection);
    result.emplace("audio_ctc_head", audio_ctc_head);
    result.emplace("audio_grapheme_ctc_vocabulary_size", audio_grapheme_ctc_vocabulary_size);
    result.emplace("audio_text_retrieval_head", audio_text_retrieval_head);
    result.emplace("audio_text_retrieval_text_source", audio_text_retrieval_text_source);
    result.emplace("cross_modal_evidence_head", cross_modal_evidence_head);
    result.emplace("cross_modal_evidence_rank", cross_modal_evidence_rank);
    result.emplace("cross_modal_evidence_direct_features", cross_modal_evidence_direct_features);
    result.emplace("cross_modal_text_query_pooling", cross_modal_text_query_pooling);
    result.emplace("cross_modal_text_sequence_pooling", cross_modal_text_sequence_pooling);
    result.emplace("cross_modal_text_contextual_pooling", cross_modal_text_contextual_pooling);
    result.emplace("narrative_evidence_head", narrative_evidence_head);
    result.emplace("narrative_evidence_hidden_dim", narrative_evidence_hidden_dim);
    result.emplace("visual_text_retrieval_head", visual_text_retrieval_head);
    result.emplace("visual_text_retrieval_dim", visual_text_retrieval_dim);
    result.emplace("audio_temporal_binary_head", audio_temporal_binary_head);
    result.emplace("video_object_temporal_encoder", video_object_temporal_encoder);
    result.emplace("video_object_frame_normalized_input", video_object_frame_normalized_input);
    result.emplace("video_object_camera_invariant_residual", video_object_camera_invariant_residual);
    result.emplace("video_object_frame_normalized_residual_scale", video_object_frame_normalized_residual_scale);
    result.emplace("video_object_time_centered_input", video_object_time_centered_input);
    result.emplace("video_object_activity_sorted_slots", video_object_activity_sorted_slots);
    result.emplace("video_object_dual_evidence", video_object_dual_evidence);
    result.emplace("video_object_set_decision", video_object_set_decision);
    result.emplace("video_object_identity_event_binding", video_object_identity_event_binding);
    result.emplace("video_object_learned_queries", video_object_learned_queries);
    result.emplace("video_object_spatial_coordinates", video_object_spatial_coordinates);
    result.emplace("video_object_spatial_event_binding", video_object_spatial_event_binding);
    result.emplace("video_spatial_temporal_moment", video_spatial_temporal_moment);
    result.emplace("video_query_spatial_temporal_moment", video_query_spatial_temporal_moment);
    result.emplace("video_spatial_temporal_y_moment", video_spatial_temporal_y_moment);
    result.emplace("video_spatial_temporal_logit_head", video_spatial_temporal_logit_head);
    result.emplace("video_spatial_temporal_bilinear_head", video_spatial_temporal_bilinear_head);
    result.emplace("video_object_trajectory_binding", video_object_trajectory_binding);
    result.emplace("video_object_pair_trajectory_binding", video_object_pair_trajectory_binding);
    result.emplace("video_descriptor_trajectory_binding", video_descriptor_trajectory_binding);
    result.emplace("video_descriptor_pair_centered_queries", video_descriptor_pair_centered_queries);
    result.emplace("video_descriptor_persistent_identity_state", video_descriptor_persistent_identity_state);
    result.emplace("video_descriptor_object_memory", video_descriptor_object_memory);
    result.emplace("video_descriptor_object_memory_scale", video_descriptor_object_memory_scale);
    result.emplace("video_descriptor_object_memory_query_gate", video_descriptor_object_memory_query_gate);
    result.emplace("video_descriptor_object_memory_reliability_gate", video_descriptor_object_memory_reliability_gate);
    result.emplace("video_descriptor_object_memory_contrast_visibility", video_descriptor_object_memory_contrast_visibility);
    result.emplace("video_descriptor_object_memory_evidence_routing", video_descriptor_object_memory_evidence_routing);
    result.emplace("video_descriptor_object_memory_evidence_routing_margin", video_descriptor_object_memory_evidence_routing_margin);
    result.emplace("video_descriptor_object_memory_contrast_readout", video_descriptor_object_memory_contrast_readout);
    result.emplace("video_descriptor_object_memory_temporal_relative_visibility", video_descriptor_object_memory_temporal_relative_visibility);
    result.emplace("video_descriptor_object_memory_temporal_relative_readout", video_descriptor_object_memory_temporal_relative_readout);
    result.emplace("video_isolated_identity_descriptors", video_isolated_identity_descriptors);
    result.emplace("video_query_conditioned_head", video_query_conditioned_head);
    result.emplace("video_camera_robustness_adapter", video_camera_robustness_adapter);
    result.emplace("video_camera_robustness_nonlinear_gate", video_camera_robustness_nonlinear_gate);
    result.emplace("video_camera_pose_dim", video_camera_pose_dim);
    result.emplace("video_spatial_relation_classes", video_spatial_relation_classes);
    result.emplace("video_action_dim", video_action_dim);
    result.emplace("video_egomotion_classes", video_egomotion_classes);
    result.emplace("video_egomotion_validity_head", video_egomotion_validity_head);
    result.emplace("video_egomotion_evidence_gate", video_egomotion_evidence_gate);
    result.emplace("video_egomotion_minimum_motion_evidence", video_egomotion_minimum_motion_evidence);
    result.emplace("video_uses_visual_semantic_encoder", video_uses_visual_semantic_encoder);
    result.emplace("video_visual_semantic_scale", video_visual_semantic_scale);
    result.emplace("video_explicit_temporal_delta", video_explicit_temporal_delta);
    result.emplace("video_explicit_temporal_delta_scale", video_explicit_temporal_delta_scale);
    result.emplace("video_separate_temporal_delta_projection", video_separate_temporal_delta_projection);
    result.emplace("long_video_world_accumulator", long_video_world_accumulator);
    result.emplace("long_video_transition_features", long_video_transition_features);
    result.emplace("text_only_bridge_adapter", text_only_bridge_adapter);
    result.emplace("text_only_output_adapter", text_only_output_adapter);
    result.emplace("text_only_cross_memory_adapter", text_only_cross_memory_adapter);
    result.emplace("text_only_hidden_cross_memory_adapter", text_only_hidden_cross_memory_adapter);
    result.emplace("text_answerability_head", text_answerability_head);
    result.emplace("text_answerability_mode", text_answerability_mode);
    result.emplace("text_answerability_classes", text_answerability_classes);
    result.emplace("text_epistemic_memory_adapter", text_epistemic_memory_adapter);
    result.emplace("text_epistemic_memory_slots", text_epistemic_memory_slots);
    result.emplace("text_epistemic_output_rank", text_epistemic_output_rank);
    result.emplace("text_epistemic_supported_class", text_epistemic_supported_class);
    result.emplace("text_epistemic_output_threshold", text_epistemic_output_threshold);
    result.emplace("text_answerability_fallback_bytes", integer_array(text_answerability_fallback_bytes));
    result.emplace("text_answerability_threshold", text_answerability_threshold);
    return result;
}

MosaicUnifiedConfig MosaicUnifiedConfig::from_dict(const JsonValue::Object& rows) {
    const auto* schema=member(rows,"schema_version"); if(!schema||schema->as_string()!=mosaic_omni_schema_version) throw std::invalid_argument("unsupported unified config schema");
    const auto* text_value=member(rows,"text"); const auto* omni_value=member(rows,"omni");
    if(!text_value||!omni_value)throw std::invalid_argument("unified config requires text and omni");
    MosaicUnifiedConfig result; result.text=MosaicTextConfig::from_dict(text_value->as_object());
    const auto& omni_rows=omni_value->as_object();
    result.omni={static_cast<std::size_t>(integer(omni_rows,"world_slots",32)),static_cast<std::size_t>(integer(omni_rows,"world_dim",256)),static_cast<std::size_t>(integer(omni_rows,"object_slots",8)),static_cast<std::size_t>(integer(omni_rows,"attention_heads",8)),static_cast<std::size_t>(integer(omni_rows,"gemma_hidden_dim",3840)),static_cast<std::size_t>(integer(omni_rows,"anima_conditioning_tokens",512)),static_cast<std::size_t>(integer(omni_rows,"anima_conditioning_dim",1024))};
    result.vision_patch_size=integer(rows,"vision_patch_size",16);
    result.visual_semantic_encoder=boolean(rows,"visual_semantic_encoder",false);
    result.visual_semantic_split_frontend=boolean(rows,"visual_semantic_split_frontend",false);
    result.visual_semantic_rounds=integer(rows,"visual_semantic_rounds",2);
    result.image_visual_adapter=boolean(rows,"image_visual_adapter",false);
    result.image_visual_adapter_rank=integer(rows,"image_visual_adapter_rank",32);
    result.image_visual_adapter_scale=number(rows,"image_visual_adapter_scale",1.0);
    result.visual_teacher_slot_bridge=boolean(rows,"visual_teacher_slot_bridge",false);
    result.visual_teacher_slot_rank=integer(rows,"visual_teacher_slot_rank",32);
    result.explicit_object_relation_grounder=boolean(rows,"explicit_object_relation_grounder",false);
    result.explicit_object_relation_rank=integer(rows,"explicit_object_relation_rank",64);
    result.explicit_relation_classes=integer(rows,"explicit_relation_classes",6);
    result.audio_patch_samples=integer(rows,"audio_patch_samples",320);
    result.world_ffn_dim=integer(rows,"world_ffn_dim",1024);
    result.world_rounds=integer(rows,"world_rounds",2);
    result.vision_teacher_dim=integer(rows,"vision_teacher_dim",0);
    result.audio_teacher_dim=integer(rows,"audio_teacher_dim",0);
    result.audio_temporal_encoder=boolean(rows,"audio_temporal_encoder",false);
    result.audio_content_encoder=boolean(rows,"audio_content_encoder",false);
    result.audio_spectral_content_frontend=boolean(rows,"audio_spectral_content_frontend",false);
    result.audio_spectral_n_fft=integer(rows,"audio_spectral_n_fft",400);
    result.audio_spectral_hop_samples=integer(rows,"audio_spectral_hop_samples",160);
    result.audio_event_slot_injection=boolean(rows,"audio_event_slot_injection",false);
    result.audio_ctc_head=boolean(rows,"audio_ctc_head",false);
    result.audio_grapheme_ctc_vocabulary_size=integer(rows,"audio_grapheme_ctc_vocabulary_size",0);
    result.audio_text_retrieval_head=boolean(rows,"audio_text_retrieval_head",false);
    result.audio_text_retrieval_text_source=text(rows,"audio_text_retrieval_text_source","world_global");
    result.cross_modal_evidence_head=boolean(rows,"cross_modal_evidence_head",false);
    result.cross_modal_evidence_rank=integer(rows,"cross_modal_evidence_rank",32);
    result.cross_modal_evidence_direct_features=boolean(rows,"cross_modal_evidence_direct_features",false);
    result.cross_modal_text_query_pooling=boolean(rows,"cross_modal_text_query_pooling",false);
    result.cross_modal_text_sequence_pooling=boolean(rows,"cross_modal_text_sequence_pooling",false);
    result.cross_modal_text_contextual_pooling=boolean(rows,"cross_modal_text_contextual_pooling",false);
    result.narrative_evidence_head=boolean(rows,"narrative_evidence_head",false);
    result.narrative_evidence_hidden_dim=integer(rows,"narrative_evidence_hidden_dim",64);
    result.visual_text_retrieval_head=boolean(rows,"visual_text_retrieval_head",false);
    result.visual_text_retrieval_dim=integer(rows,"visual_text_retrieval_dim",512);
    result.audio_temporal_binary_head=boolean(rows,"audio_temporal_binary_head",false);
    result.video_object_temporal_encoder=boolean(rows,"video_object_temporal_encoder",false);
    result.video_object_frame_normalized_input=boolean(rows,"video_object_frame_normalized_input",false);
    result.video_object_camera_invariant_residual=boolean(rows,"video_object_camera_invariant_residual",false);
    result.video_object_frame_normalized_residual_scale=number(rows,"video_object_frame_normalized_residual_scale",1.0);
    result.video_object_time_centered_input=boolean(rows,"video_object_time_centered_input",false);
    result.video_object_activity_sorted_slots=boolean(rows,"video_object_activity_sorted_slots",false);
    result.video_object_dual_evidence=boolean(rows,"video_object_dual_evidence",false);
    result.video_object_set_decision=boolean(rows,"video_object_set_decision",false);
    result.video_object_identity_event_binding=boolean(rows,"video_object_identity_event_binding",false);
    result.video_object_learned_queries=boolean(rows,"video_object_learned_queries",false);
    result.video_object_spatial_coordinates=boolean(rows,"video_object_spatial_coordinates",false);
    result.video_object_spatial_event_binding=boolean(rows,"video_object_spatial_event_binding",false);
    result.video_spatial_temporal_moment=boolean(rows,"video_spatial_temporal_moment",false);
    result.video_query_spatial_temporal_moment=boolean(rows,"video_query_spatial_temporal_moment",false);
    result.video_spatial_temporal_y_moment=boolean(rows,"video_spatial_temporal_y_moment",false);
    result.video_spatial_temporal_logit_head=boolean(rows,"video_spatial_temporal_logit_head",false);
    result.video_spatial_temporal_bilinear_head=boolean(rows,"video_spatial_temporal_bilinear_head",false);
    result.video_object_trajectory_binding=boolean(rows,"video_object_trajectory_binding",false);
    result.video_object_pair_trajectory_binding=boolean(rows,"video_object_pair_trajectory_binding",false);
    result.video_descriptor_trajectory_binding=boolean(rows,"video_descriptor_trajectory_binding",false);
    result.video_descriptor_pair_centered_queries=boolean(rows,"video_descriptor_pair_centered_queries",false);
    result.video_descriptor_persistent_identity_state=boolean(rows,"video_descriptor_persistent_identity_state",false);
    result.video_descriptor_object_memory=boolean(rows,"video_descriptor_object_memory",false);
    result.video_descriptor_object_memory_scale=number(rows,"video_descriptor_object_memory_scale",1.0);
    result.video_descriptor_object_memory_query_gate=boolean(rows,"video_descriptor_object_memory_query_gate",false);
    result.video_descriptor_object_memory_reliability_gate=boolean(rows,"video_descriptor_object_memory_reliability_gate",false);
    result.video_descriptor_object_memory_contrast_visibility=boolean(rows,"video_descriptor_object_memory_contrast_visibility",false);
    result.video_descriptor_object_memory_evidence_routing=boolean(rows,"video_descriptor_object_memory_evidence_routing",false);
    result.video_descriptor_object_memory_evidence_routing_margin=number(rows,"video_descriptor_object_memory_evidence_routing_margin",0.0);
    result.video_descriptor_object_memory_contrast_readout=boolean(rows,"video_descriptor_object_memory_contrast_readout",false);
    result.video_descriptor_object_memory_temporal_relative_visibility=boolean(rows,"video_descriptor_object_memory_temporal_relative_visibility",false);
    result.video_descriptor_object_memory_temporal_relative_readout=boolean(rows,"video_descriptor_object_memory_temporal_relative_readout",false);
    result.video_isolated_identity_descriptors=boolean(rows,"video_isolated_identity_descriptors",false);
    result.video_query_conditioned_head=boolean(rows,"video_query_conditioned_head",false);
    result.video_camera_robustness_adapter=boolean(rows,"video_camera_robustness_adapter",false);
    result.video_camera_robustness_nonlinear_gate=boolean(rows,"video_camera_robustness_nonlinear_gate",false);
    result.video_camera_pose_dim=integer(rows,"video_camera_pose_dim",0);
    result.video_spatial_relation_classes=integer(rows,"video_spatial_relation_classes",0);
    result.video_action_dim=integer(rows,"video_action_dim",0);
    result.video_egomotion_classes=integer(rows,"video_egomotion_classes",0);
    result.video_egomotion_validity_head=boolean(rows,"video_egomotion_validity_head",false);
    result.video_egomotion_evidence_gate=boolean(rows,"video_egomotion_evidence_gate",false);
    result.video_egomotion_minimum_motion_evidence=number(rows,"video_egomotion_minimum_motion_evidence",1e-06);
    result.video_uses_visual_semantic_encoder=boolean(rows,"video_uses_visual_semantic_encoder",false);
    result.video_visual_semantic_scale=number(rows,"video_visual_semantic_scale",1.0);
    result.video_explicit_temporal_delta=boolean(rows,"video_explicit_temporal_delta",false);
    result.video_explicit_temporal_delta_scale=number(rows,"video_explicit_temporal_delta_scale",1.0);
    result.video_separate_temporal_delta_projection=boolean(rows,"video_separate_temporal_delta_projection",false);
    result.long_video_world_accumulator=boolean(rows,"long_video_world_accumulator",false);
    result.long_video_transition_features=boolean(rows,"long_video_transition_features",false);
    result.text_only_bridge_adapter=boolean(rows,"text_only_bridge_adapter",false);
    result.text_only_output_adapter=boolean(rows,"text_only_output_adapter",false);
    result.text_only_cross_memory_adapter=boolean(rows,"text_only_cross_memory_adapter",false);
    result.text_only_hidden_cross_memory_adapter=boolean(rows,"text_only_hidden_cross_memory_adapter",false);
    result.text_answerability_head=boolean(rows,"text_answerability_head",false);
    result.text_answerability_mode=text(rows,"text_answerability_mode","pooled");
    result.text_answerability_classes=integer(rows,"text_answerability_classes",2);
    result.text_epistemic_memory_adapter=boolean(rows,"text_epistemic_memory_adapter",false);
    result.text_epistemic_memory_slots=integer(rows,"text_epistemic_memory_slots",1);
    result.text_epistemic_output_rank=integer(rows,"text_epistemic_output_rank",0);
    result.text_epistemic_supported_class=integer(rows,"text_epistemic_supported_class",0);
    result.text_epistemic_output_threshold=number(rows,"text_epistemic_output_threshold",0.5);
    result.text_answerability_fallback_bytes=integers(rows,"text_answerability_fallback_bytes");
    result.text_answerability_threshold=number(rows,"text_answerability_threshold",0.5);
    result.validate(); return result;
}

}  // namespace swegca::world
