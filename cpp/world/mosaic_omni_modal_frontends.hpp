#pragma once

#include "world/mosaic_omni.hpp"

#include <cstddef>
#include <optional>
#include <string_view>
#include <vector>

namespace swegca::world {

inline constexpr std::string_view mosaic_omni_modal_frontends_source_sha256 =
    "a06f7a85128827b036a7c1801920e570e187c0163df6c3851d85ef2dd3bb5e20";

struct ModalLinearWeights final { std::vector<double> weight, bias; };
struct ModalConvWeights final { std::vector<double> weight, bias; };
struct ModalGruWeights final {
    std::vector<double> weight_ih, weight_hh, bias_ih, bias_hh;
};
struct VisualSemanticCellWeights final {
    std::vector<double> norm1_weight, norm1_bias;
    std::vector<double> attention_in_weight, attention_in_bias;
    std::vector<double> attention_out_weight, attention_out_bias;
    std::vector<double> norm2_weight, norm2_bias;
    std::vector<double> linear1_weight, linear1_bias;
    std::vector<double> linear2_weight, linear2_bias;
};

struct MosaicOmniModalFrontendConfig final {
    std::size_t world_dim{};
    std::size_t attention_heads{};
    std::size_t world_ffn_dim{};
    std::size_t vision_patch_size{};
    bool visual_semantic_encoder{};
    bool visual_semantic_split_frontend{};
    std::size_t visual_semantic_rounds{};
    bool image_visual_adapter{};
    std::size_t image_visual_adapter_rank{};
    double image_visual_adapter_scale{1.0};
    std::size_t audio_patch_samples{};
    bool audio_temporal_encoder{};
    bool audio_content_encoder{};
    bool audio_spectral_content_frontend{};
    std::size_t audio_spectral_n_fft{};
    std::size_t audio_spectral_hop_samples{};
    bool audio_event_slot_injection{};
    bool audio_ctc_head{};
    std::size_t audio_grapheme_ctc_vocabulary_size{};
    bool audio_text_retrieval_head{};
    std::size_t audio_teacher_dim{};
    bool audio_temporal_binary_head{};

    [[nodiscard]] static MosaicOmniModalFrontendConfig from_unified(
        const MosaicUnifiedConfig& config);
    void validate() const;
};

struct MosaicOmniModalFrontendWeights final {
    ModalConvWeights vision_frontend;
    std::optional<ModalConvWeights> visual_semantic_frontend;
    std::optional<ModalLinearWeights> visual_position;
    std::optional<VisualSemanticCellWeights> visual_semantic_cell;
    std::optional<ModalLinearWeights> visual_semantic_norm;
    std::optional<ModalConvWeights> image_visual_adapter_down;
    std::optional<ModalConvWeights> image_visual_adapter_up;
    ModalConvWeights audio_frontend;
    std::optional<ModalGruWeights> audio_temporal_cell;
    std::optional<ModalLinearWeights> audio_temporal_to_world;
    std::optional<ModalGruWeights> audio_content_temporal_cell;
    std::optional<ModalLinearWeights> audio_content_to_world;
    std::optional<ModalLinearWeights> audio_spectral_projection;
    std::optional<ModalLinearWeights> audio_event_slot_projection;
    std::optional<ModalLinearWeights> audio_ctc_projection;
    std::optional<ModalLinearWeights> audio_grapheme_ctc_projection;
    std::optional<ModalLinearWeights> audio_text_retrieval_projection;
    std::optional<ModalLinearWeights> text_audio_retrieval_projection;
    std::optional<ModalLinearWeights> audio_teacher_projection;
    std::optional<ModalLinearWeights> audio_temporal_head_norm;
    std::optional<ModalLinearWeights> audio_temporal_head_output;
    std::vector<double> image_modality_embedding;
    std::vector<double> audio_modality_embedding;

    void validate(const MosaicOmniModalFrontendConfig& config) const;
};

struct ImageFrontendOutput final {
    Tensor tokens;
    BooleanMask mask;
    Tensor summary;
    Tensor patch_tokens;
    std::size_t patch_height{};
    std::size_t patch_width{};
};

struct AudioFrontendOutput final {
    Tensor tokens;
    BooleanMask mask;
    Tensor summary;
    // Pre-modality sequence used by the original cross-modal evidence path.
    Tensor evidence_tokens;
    std::optional<Tensor> world_summary;
    std::optional<Tensor> temporal_states;
    std::optional<Tensor> temporal_features;
    std::optional<Tensor> event_slots;
    std::optional<Tensor> projected_event_slots;
    std::optional<Tensor> content_summary;
    std::optional<Tensor> content_features;
    std::optional<Tensor> content_event_slots;
    std::optional<Tensor> projected_content_event_slots;
    std::optional<Tensor> temporal_logits;
    std::optional<Tensor> teacher_embedding;
    std::optional<Tensor> teacher_temporal_states;
    std::optional<Tensor> world_teacher_embedding;
    std::optional<Tensor> ctc_logits;
    std::optional<Tensor> grapheme_ctc_logits;
    std::optional<Tensor> audio_text_retrieval_embedding;
    std::optional<Tensor> text_audio_retrieval_embedding;
};

class MosaicOmniModalFrontends final {
public:
    MosaicOmniModalFrontends(MosaicOmniModalFrontendConfig config,
                             MosaicOmniModalFrontendWeights weights);

    [[nodiscard]] Tensor encode_visual_map(const Tensor& normalized_pixels) const;
    [[nodiscard]] Tensor apply_visual_semantic_encoder(const Tensor& visual) const;
    [[nodiscard]] Tensor encode_visual_semantic_map(const Tensor& normalized_pixels) const;
    [[nodiscard]] Tensor encode_image_visual_semantic_map(const Tensor& normalized_pixels) const;
    [[nodiscard]] Tensor encode_visual_summary(const Tensor& pixel_values) const;
    [[nodiscard]] Tensor encode_visual_regions(const Tensor& pixel_values,
                                               const Tensor& boxes) const;
    [[nodiscard]] Tensor encode_visual_region_grids(const Tensor& pixel_values,
                                                    const Tensor& boxes,
                                                    std::size_t grid_size = 2) const;
    [[nodiscard]] Tensor frontend_input(const Tensor& values) const;
    void validate_image(const Tensor& values, std::size_t batch,
                        std::string_view name) const;
    [[nodiscard]] static BooleanMask full_mask(const Tensor& tokens);
    [[nodiscard]] ImageFrontendOutput forward_image(const Tensor& pixel_values,
                                                    std::size_t batch) const;
    [[nodiscard]] AudioFrontendOutput forward_audio(
        const Tensor& audio_values, std::size_t batch,
        const Tensor* text_retrieval_source = nullptr,
        const Tensor* world_audio_event_slots = nullptr) const;
    [[nodiscard]] Tensor project_text_audio_retrieval(
        const Tensor& text_retrieval_source) const;

private:
    MosaicOmniModalFrontendConfig config_;
    MosaicOmniModalFrontendWeights weights_;
};

}  // namespace swegca::world
