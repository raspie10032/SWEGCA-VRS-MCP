#pragma once

#include "world/mosaic_omni_text_adapters.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace swegca::world {

inline constexpr std::string_view mosaic_omni_text_pipeline_source_sha256 =
    "a06f7a85128827b036a7c1801920e570e187c0163df6c3851d85ef2dd3bb5e20";

struct PooledAnswerabilityWeights final {
    TextAdapterNormWeights norm;
    TextAdapterLinearWeights hidden, output;
};
struct MosaicOmniTextPipelineWeights final {
    TextAdapterLinearWeights text_to_world, world_to_text_memory;
    std::vector<double> text_modality_embedding;
    std::optional<TextAdapterLinearWeights> text_only_to_world;
    std::optional<TextAdapterLinearWeights> text_only_world_to_text_memory;
    std::optional<TextAdapterLinearWeights> logit_hidden, logit_output;
    std::optional<PooledAnswerabilityWeights> pooled_answerability;
    std::optional<TextAdapterLinearWeights> epistemic_memory; // bias-free
};
struct MosaicOmniTextPipelineConfig final {
    std::size_t world_dim{}, answerability_classes{2}, epistemic_memory_slots{1};
    std::size_t epistemic_supported_class{};
    double epistemic_output_threshold{0.5};
    std::string answerability_mode{"pooled"};
    bool answerability_head{};
    void validate(const MosaicTextConfig&) const;
};
struct MosaicOmniTextSourceInputs final {
    MosaicTokenBatch world_input_ids;
    bool text_only{};
};
struct MosaicOmniTextSourceOutput final {
    Tensor text_states;
    Tensor text_world;
    Tensor source_tokens;
    BooleanMask source_mask;
    Tensor retrieval_summary;
    std::string modality;
};
struct MosaicOmniTextPipelineInputs final {
    MosaicTokenBatch input_ids;
    MosaicTokenBatch world_input_ids; // empty means source default: input_ids
    std::optional<MosaicTokenBatch> question_input_ids;
    std::optional<MosaicTokenBatch> targets;
    std::optional<std::vector<std::int64_t>> answerability_labels;
    std::optional<std::size_t> text_rounds;
    bool text_only{};
    Tensor world;
    Tensor text_states; // upstream [B,1+evidence patches,text_dim]
    BooleanMask text_mask; // upstream evidence patch mask, excludes BOS
};
struct MosaicOmniTextPipelineOutput final {
    MosaicTextOutput text;
    std::optional<Tensor> answerability_logits;
    std::optional<double> answerability_loss;
    std::optional<std::vector<std::uint8_t>> epistemic_output_active;
    Tensor text_memory;
};
struct MosaicOmniTextPipelineAdapters final {
    const TextAnswerabilityVerifier* answerability{};
    const TextOnlyCrossMemoryAdapter* cross_memory{};
    const TextOnlyHiddenCrossMemoryAdapter* hidden_cross_memory{};
    const TextEpistemicOutputAdapter* epistemic_output{};
};

class MosaicOmniTextPipeline final {
public:
    MosaicOmniTextPipeline(const MosaicTextLM&, MosaicOmniTextPipelineConfig,
        MosaicOmniTextPipelineWeights, MosaicOmniTextPipelineAdapters = {});
    [[nodiscard]] MosaicOmniTextSourceOutput prepare_text_source(
        const MosaicOmniTextSourceInputs&) const;
    [[nodiscard]] MosaicOmniTextPipelineOutput forward(
        const MosaicOmniTextPipelineInputs&) const;
private:
    const MosaicTextLM* text_core_;
    MosaicOmniTextPipelineConfig config_;
    MosaicOmniTextPipelineWeights weights_;
    MosaicOmniTextPipelineAdapters adapters_;
};

} // namespace swegca::world
