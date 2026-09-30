#pragma once

#include "world/mosaic_omni.hpp"

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace swegca::world {

inline constexpr std::string_view mosaic_omni_text_adapters_source_sha256 =
    "a06f7a85128827b036a7c1801920e570e187c0163df6c3851d85ef2dd3bb5e20";

struct TextAdapterLinearWeights final {
    std::vector<double> weight, bias; // PyTorch [out,in], then [out]
};
struct TextAdapterNormWeights final { std::vector<double> weight, bias; };
struct TextAdapterAttentionWeights final {
    TextAdapterLinearWeights in_projection, out_projection; // packed Q,K,V
};
struct TextAdapterEncoderWeights final {
    TextAdapterNormWeights norm1, norm2;
    TextAdapterAttentionWeights attention;
    TextAdapterLinearWeights feedforward_in, feedforward_out;
};

struct TextOnlyCrossMemoryWeights final {
    TextAdapterLinearWeights query;
    TextAdapterAttentionWeights cross_attention;
    TextAdapterLinearWeights output;
};
class TextOnlyCrossMemoryAdapter final {
public:
    TextOnlyCrossMemoryAdapter(std::size_t model_dim, std::size_t attention_heads,
        TextOnlyCrossMemoryWeights weights);
    [[nodiscard]] Tensor forward(const Tensor& logits, const Tensor& evidence_states,
        const BooleanMask& evidence_mask) const;
private:
    std::size_t dim_, heads_; TextOnlyCrossMemoryWeights weights_;
};

struct TextOnlyHiddenCrossMemoryWeights final {
    TextAdapterNormWeights query_norm;
    TextAdapterLinearWeights query;
    TextAdapterAttentionWeights cross_attention;
    TextAdapterLinearWeights output;
};
class TextOnlyHiddenCrossMemoryAdapter final {
public:
    TextOnlyHiddenCrossMemoryAdapter(std::size_t model_dim, std::size_t attention_heads,
        TextOnlyHiddenCrossMemoryWeights weights);
    [[nodiscard]] Tensor forward(const Tensor& decoder_states, const Tensor& evidence_states,
        const BooleanMask& evidence_mask) const;
private:
    std::size_t dim_, heads_; TextOnlyHiddenCrossMemoryWeights weights_;
};

struct TextEpistemicOutputWeights final {
    TextAdapterLinearWeights down, output;
};
class TextEpistemicOutputAdapter final {
public:
    TextEpistemicOutputAdapter(std::size_t model_dim, std::size_t rank,
        TextEpistemicOutputWeights weights);
    [[nodiscard]] Tensor forward(const Tensor& decoder_states,
        std::span<const std::uint8_t> active) const;
private:
    std::size_t dim_, rank_; TextEpistemicOutputWeights weights_;
};

struct TextAnswerabilityConfig final {
    std::size_t world_dim{}, attention_heads{}, source_dim{}, extra_feature_dim{};
    std::size_t output_classes{2};
    bool contextual{}, evidence_consistency{}, question_body_only{};
    void validate() const;
};
struct TextAnswerabilityWeights final {
    std::optional<TextAdapterNormWeights> input_norm;
    std::optional<TextAdapterLinearWeights> input_projection;
    std::optional<TextAdapterEncoderWeights> context_encoder;
    TextAdapterNormWeights question_norm, evidence_norm;
    TextAdapterAttentionWeights cross_attention;
    TextAdapterNormWeights token_norm;
    TextAdapterLinearWeights token_projection;
    TextAdapterNormWeights output_norm;
    TextAdapterLinearWeights output_hidden, output;
};
class TextAnswerabilityVerifier final {
public:
    TextAnswerabilityVerifier(TextAnswerabilityConfig, TextAnswerabilityWeights);
    [[nodiscard]] Tensor forward(const Tensor& question_states, const BooleanMask& question_mask,
        const Tensor& evidence_states, const BooleanMask& evidence_mask,
        const BooleanMask* evidence_title_mask = nullptr,
        const BooleanMask* evidence_body_mask = nullptr,
        const Tensor* extra_features = nullptr) const;
private:
    TextAnswerabilityConfig config_; TextAnswerabilityWeights weights_;
};

} // namespace swegca::world
