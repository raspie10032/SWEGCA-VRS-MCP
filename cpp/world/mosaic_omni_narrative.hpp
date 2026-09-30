#pragma once

#include "world/text_lm.hpp"

#include <cstddef>
#include <string_view>
#include <vector>

namespace swegca::world {

inline constexpr std::string_view mosaic_omni_narrative_source_sha256 =
    "a06f7a85128827b036a7c1801920e570e187c0163df6c3851d85ef2dd3bb5e20";

struct NarrativeDescriptorProjection final {
    std::size_t world_dim{};
    std::vector<float> weight; // [world_dim, text model_dim]
    std::vector<float> bias;   // [world_dim]
    void validate(std::size_t text_dim) const;
};

struct NarrativeContinuityWeights final {
    std::size_t hidden_dim{}, world_dim{};
    std::vector<float> hidden_weight, hidden_bias; // [hidden_dim, 8 * text_dim]
    std::vector<float> score_weight, score_bias;   // [1, hidden_dim], [1]
    std::vector<float> world_delta_weight;         // [world_dim, 1], bias=False
    void validate(std::size_t text_dim) const;
    [[nodiscard]] static std::vector<float> zero_world_delta(std::size_t world_dim);
};

struct NarrativeContinuityOutput final {
    Tensor score;       // [batch]
    Tensor world_delta; // [batch, world_dim]
};

[[nodiscard]] Tensor encode_text_descriptor(const MosaicTextLM& text_core,
    const MosaicTokenBatch& input_ids, const NarrativeDescriptorProjection& projection);
[[nodiscard]] Tensor narrative_text_summary(const MosaicTextLM& text_core,
    const MosaicTokenBatch& input_ids);

class NarrativeContinuityScorer final {
public:
    NarrativeContinuityScorer(const MosaicTextLM& text_core, NarrativeContinuityWeights weights);
    [[nodiscard]] NarrativeContinuityOutput score_narrative_continuity(
        const MosaicTokenBatch& anchor_input_ids,
        const MosaicTokenBatch& candidate_input_ids) const;
private:
    const MosaicTextLM* text_core_;
    NarrativeContinuityWeights weights_;
};

} // namespace swegca::world
