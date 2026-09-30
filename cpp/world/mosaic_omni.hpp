#pragma once

#include "world/cognitive_state.hpp"
#include "world/text_lm.hpp"
#include "world/world_state.hpp"

#include <cstdint>
#include <array>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace swegca::world {

struct MosaicOmniConfig final {
    std::size_t world_slots{32};
    std::size_t world_dim{256};
    std::size_t object_slots{8};
    std::size_t attention_heads{8};
    std::size_t gemma_hidden_dim{3840};
    std::size_t anima_conditioning_tokens{512};
    std::size_t anima_conditioning_dim{1024};
    void validate() const;
    [[nodiscard]] JsonValue::Object to_dict() const;
};

struct MosaicUnifiedConfig final {
    MosaicTextConfig text;
    MosaicOmniConfig omni;
    std::int64_t vision_patch_size{16};
    bool visual_semantic_encoder{false};
    bool visual_semantic_split_frontend{false};
    std::int64_t visual_semantic_rounds{2};
    bool image_visual_adapter{false};
    std::int64_t image_visual_adapter_rank{32};
    double image_visual_adapter_scale{1.0};
    bool visual_teacher_slot_bridge{false};
    std::int64_t visual_teacher_slot_rank{32};
    bool explicit_object_relation_grounder{false};
    std::int64_t explicit_object_relation_rank{64};
    std::int64_t explicit_relation_classes{6};
    std::int64_t audio_patch_samples{320};
    std::int64_t world_ffn_dim{1024};
    std::int64_t world_rounds{2};
    std::int64_t vision_teacher_dim{0};
    std::int64_t audio_teacher_dim{0};
    bool audio_temporal_encoder{false};
    bool audio_content_encoder{false};
    bool audio_spectral_content_frontend{false};
    std::int64_t audio_spectral_n_fft{400};
    std::int64_t audio_spectral_hop_samples{160};
    bool audio_event_slot_injection{false};
    bool audio_ctc_head{false};
    std::int64_t audio_grapheme_ctc_vocabulary_size{0};
    bool audio_text_retrieval_head{false};
    std::string audio_text_retrieval_text_source{"world_global"};
    bool cross_modal_evidence_head{false};
    std::int64_t cross_modal_evidence_rank{32};
    bool cross_modal_evidence_direct_features{false};
    bool cross_modal_text_query_pooling{false};
    bool cross_modal_text_sequence_pooling{false};
    bool cross_modal_text_contextual_pooling{false};
    bool narrative_evidence_head{false};
    std::int64_t narrative_evidence_hidden_dim{64};
    bool visual_text_retrieval_head{false};
    std::int64_t visual_text_retrieval_dim{512};
    bool audio_temporal_binary_head{false};
    bool video_object_temporal_encoder{false};
    bool video_object_frame_normalized_input{false};
    bool video_object_camera_invariant_residual{false};
    double video_object_frame_normalized_residual_scale{1.0};
    bool video_object_time_centered_input{false};
    bool video_object_activity_sorted_slots{false};
    bool video_object_dual_evidence{false};
    bool video_object_set_decision{false};
    bool video_object_identity_event_binding{false};
    bool video_object_learned_queries{false};
    bool video_object_spatial_coordinates{false};
    bool video_object_spatial_event_binding{false};
    bool video_spatial_temporal_moment{false};
    bool video_query_spatial_temporal_moment{false};
    bool video_spatial_temporal_y_moment{false};
    bool video_spatial_temporal_logit_head{false};
    bool video_spatial_temporal_bilinear_head{false};
    bool video_object_trajectory_binding{false};
    bool video_object_pair_trajectory_binding{false};
    bool video_descriptor_trajectory_binding{false};
    bool video_descriptor_pair_centered_queries{false};
    bool video_descriptor_persistent_identity_state{false};
    bool video_descriptor_object_memory{false};
    double video_descriptor_object_memory_scale{1.0};
    bool video_descriptor_object_memory_query_gate{false};
    bool video_descriptor_object_memory_reliability_gate{false};
    bool video_descriptor_object_memory_contrast_visibility{false};
    bool video_descriptor_object_memory_evidence_routing{false};
    double video_descriptor_object_memory_evidence_routing_margin{0.0};
    bool video_descriptor_object_memory_contrast_readout{false};
    bool video_descriptor_object_memory_temporal_relative_visibility{false};
    bool video_descriptor_object_memory_temporal_relative_readout{false};
    bool video_isolated_identity_descriptors{false};
    bool video_query_conditioned_head{false};
    bool video_camera_robustness_adapter{false};
    bool video_camera_robustness_nonlinear_gate{false};
    std::int64_t video_camera_pose_dim{0};
    std::int64_t video_spatial_relation_classes{0};
    std::int64_t video_action_dim{0};
    std::int64_t video_egomotion_classes{0};
    bool video_egomotion_validity_head{false};
    bool video_egomotion_evidence_gate{false};
    double video_egomotion_minimum_motion_evidence{1e-06};
    bool video_uses_visual_semantic_encoder{false};
    double video_visual_semantic_scale{1.0};
    bool video_explicit_temporal_delta{false};
    double video_explicit_temporal_delta_scale{1.0};
    bool video_separate_temporal_delta_projection{false};
    bool long_video_world_accumulator{false};
    bool long_video_transition_features{false};
    bool text_only_bridge_adapter{false};
    bool text_only_output_adapter{false};
    bool text_only_cross_memory_adapter{false};
    bool text_only_hidden_cross_memory_adapter{false};
    bool text_answerability_head{false};
    std::string text_answerability_mode{"pooled"};
    std::int64_t text_answerability_classes{2};
    bool text_epistemic_memory_adapter{false};
    std::int64_t text_epistemic_memory_slots{1};
    std::int64_t text_epistemic_output_rank{0};
    std::int64_t text_epistemic_supported_class{0};
    double text_epistemic_output_threshold{0.5};
    std::vector<std::int64_t> text_answerability_fallback_bytes{};
    double text_answerability_threshold{0.5};

    void validate() const;
    [[nodiscard]] JsonValue::Object to_dict() const;
    [[nodiscard]] static MosaicUnifiedConfig from_dict(const JsonValue::Object& values);
};

inline constexpr std::string_view mosaic_omni_schema_version = "mosaic-unified-config-v0";

inline constexpr std::array<std::size_t, 3> answerability_ngram_widths{4, 8, 12};

[[nodiscard]] MosaicTokenBatch compact_body_input_ids(const MosaicTokenBatch& input_ids);
[[nodiscard]] Tensor byte_ngram_overlap_features(
    const MosaicTokenBatch& question_input_ids,
    const MosaicTokenBatch& evidence_input_ids,
    std::span<const std::size_t> widths = answerability_ngram_widths);
[[nodiscard]] Tensor time_center_object_frame_grid(const Tensor& frame_grid);
[[nodiscard]] Tensor select_object_temporal_evidence(
    const Tensor& frame_grid, bool time_centered, bool dual_evidence,
    std::size_t raw_rows);
[[nodiscard]] Tensor normalize_object_frontend_frames(const Tensor& frames);
[[nodiscard]] Tensor camera_invariant_object_frames(const Tensor& frames);
[[nodiscard]] Tensor video_camera_statistics(const Tensor& video);
[[nodiscard]] std::pair<Tensor, Tensor> cross_modal_late_summaries(
    const Tensor& text_tokens, const BooleanMask& text_mask,
    const Tensor& video_tokens);
struct CrossModalQuerySummaryWeights final {
    std::vector<float> score_weight;
    float score_bias{};
    void validate(std::size_t dimension) const;
};
[[nodiscard]] Tensor cross_modal_query_summary(
    const Tensor& tokens, const BooleanMask& mask,
    const CrossModalQuerySummaryWeights& weights);
struct CrossModalSequenceSummaryWeights final {
    std::vector<float> input_weight, recurrent_weight;
    std::vector<float> input_bias, recurrent_bias;
    void validate(std::size_t dimension) const;
};
[[nodiscard]] Tensor cross_modal_sequence_summary(
    const Tensor& tokens, const BooleanMask& mask,
    const CrossModalSequenceSummaryWeights& weights);
[[nodiscard]] Tensor cross_modal_last_summary(
    const Tensor& tokens, const BooleanMask& mask);
[[nodiscard]] Tensor spatial_temporal_moment(
    const Tensor& feature_map, std::size_t batch, std::size_t frames,
    char axis = 'x');
[[nodiscard]] Tensor spatial_event_features(
    const Tensor& frame_grid, const Tensor& attention);
[[nodiscard]] Tensor object_attention_trajectory(
    const Tensor& attention, const Tensor* match_logits = nullptr);
[[nodiscard]] Tensor confidence_gated_sequence(
    const Tensor& values, const Tensor& match_logits);
[[nodiscard]] Tensor temporal_relative_visibility(const Tensor& visibility_logits);
[[nodiscard]] Tensor contrast_memory_summary(
    const Tensor& cosine_peak, const Tensor& cosine_margin);
[[nodiscard]] Tensor normalized_evidence_preference(
    const Tensor& weights, double margin = 0.0);
struct DescriptorObjectMemoryOutput final { Tensor features; Tensor margin; };
[[nodiscard]] DescriptorObjectMemoryOutput descriptor_object_memory(
    const Tensor& attention, const Tensor& visibility_logits,
    bool temporal_relative_visibility = false);
[[nodiscard]] Tensor sort_object_slots_by_temporal_activity(
    const Tensor& object_slots, const Tensor& frame_grid);

struct MosaicUnifiedOutput final {
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

    [[nodiscard]] const Tensor& logits() const noexcept { return text.logits; }
    [[nodiscard]] const std::optional<double>& loss() const noexcept { return text.loss; }
};

struct NarrativeContinuityOutput final {
    Tensor score;
    Tensor world_delta;
};

struct OmniLocalContractPaths final {
    std::optional<std::filesystem::path> gemma_config;
    std::optional<std::filesystem::path> gemma_processor;
    std::optional<std::filesystem::path> raw_image_smoke_log;
    std::optional<std::filesystem::path> anima_q4;
    std::optional<std::filesystem::path> anima_bf16;
    std::optional<std::filesystem::path> anima_vae;
    std::optional<std::filesystem::path> anima_text_encoder;
    std::optional<std::filesystem::path> gemma_q4;
};

[[nodiscard]] JsonValue::Object inspect_omni_local_contract(
    const OmniLocalContractPaths& paths);

struct WorldToAnimaWeights final {
    std::vector<float> conditioning_queries;
    std::vector<float> attention_in_weight, attention_in_bias;
    std::vector<float> attention_out_weight, attention_out_bias;
    std::vector<float> output_norm_weight, output_norm_bias;
    std::vector<float> output_weight;
    void validate(const MosaicOmniConfig& config) const;
};

class WorldToAnimaConditioning final {
public:
    WorldToAnimaConditioning(MosaicOmniConfig config, WorldToAnimaWeights weights);
    [[nodiscard]] Tensor forward(const WorldState& world_state) const;
private:
    MosaicOmniConfig config_;
    WorldToAnimaWeights weights_;
};

struct LongVideoWorldWeights final {
    std::vector<float> position_weight;
    std::vector<float> transition_weight, transition_bias;
    std::vector<float> gru_weight_ih, gru_weight_hh, gru_bias_ih, gru_bias_hh;
    std::vector<float> norm_weight, norm_bias;
    std::vector<float> order_norm_weight, order_norm_bias;
    std::vector<float> order_weight, order_bias;
    void validate(const MosaicOmniConfig& config, bool transition_features) const;
};

struct LongVideoWorldOutput final {
    WorldState world_state;
    Tensor order_logits;
    Tensor checkpoint_states;
};

class LongVideoWorldAccumulator final {
public:
    LongVideoWorldAccumulator(MosaicOmniConfig config, bool transition_features,
        LongVideoWorldWeights weights);
    [[nodiscard]] LongVideoWorldOutput forward(
        const Tensor& clip_world_states, const BooleanMask& clip_mask,
        const WorldState* initial_state = nullptr) const;
private:
    MosaicOmniConfig config_;
    bool transition_features_{};
    LongVideoWorldWeights weights_;
};

}  // namespace swegca::world
