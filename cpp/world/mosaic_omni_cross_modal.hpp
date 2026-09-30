#pragma once

#include "world/mosaic_omni.hpp"

#include <optional>
#include <vector>

namespace swegca::world {

struct CrossModalEvidenceWeights final {
    std::vector<float> text_projection, audio_projection, video_projection;
    std::optional<CrossModalQuerySummaryWeights> text_query;
    std::optional<CrossModalSequenceSummaryWeights> text_sequence;
    std::vector<float> normalization_mean, normalization_variance;
    std::vector<float> to_world_weight, to_world_bias;
    std::vector<float> head_norm_weight, head_norm_bias;
    std::vector<float> head_weight, head_bias;
    void validate(const MosaicUnifiedConfig& config) const;
};

struct CrossModalEvidenceInput final {
    Tensor text_summary;
    Tensor text_tokens;
    BooleanMask text_mask;
    std::optional<Tensor> audio_summary;
    std::optional<Tensor> audio_tokens;
    std::optional<Tensor> video_summary;
    std::optional<Tensor> video_tokens;
};

struct CrossModalEvidenceOutput final {
    Tensor world_delta;
    Tensor logits;
};

class CrossModalEvidenceFusion final {
public:
    CrossModalEvidenceFusion(MosaicUnifiedConfig config, CrossModalEvidenceWeights weights);
    [[nodiscard]] CrossModalEvidenceOutput forward(
        const CrossModalEvidenceInput& input) const;
private:
    MosaicUnifiedConfig config_;
    CrossModalEvidenceWeights weights_;
};

}  // namespace swegca::world
