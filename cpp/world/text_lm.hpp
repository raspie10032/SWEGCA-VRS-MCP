#pragma once

#include "world/cognitive_state.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace swegca::world {

inline constexpr std::int64_t mosaic_pad_id = 256;
inline constexpr std::int64_t mosaic_bos_id = 257;
inline constexpr std::int64_t mosaic_eos_id = 258;
inline constexpr std::int64_t mosaic_vocab_size = 259;
inline constexpr std::int64_t mosaic_ignore_index = -100;

struct MosaicTextConfig final {
    std::size_t patch_size{8};
    std::size_t byte_embedding_dim{64};
    std::size_t model_dim{128};
    std::size_t attention_heads{4};
    std::size_t ffn_dim{512};
    std::size_t physical_layers{2};
    std::size_t workspace_slots{4};
    std::size_t retriever_dim{128};
    std::size_t operator_basis_count{16};
    std::size_t operator_rank{2};
    std::size_t maximum_recurrent_depth{4};
    double maximum_operator_update{0.25};
    double dropout{};

    void validate() const;
    [[nodiscard]] JsonValue::Object to_dict() const;
    [[nodiscard]] static MosaicTextConfig from_dict(const JsonValue::Object& values);
};

class BytePatchCodec final {
public:
    explicit BytePatchCodec(std::size_t patch_size = 8);
    [[nodiscard]] std::vector<std::int64_t> encode(
        std::string_view text, bool add_bos = true, bool add_eos = true) const;
    [[nodiscard]] std::string decode(
        const std::vector<std::int64_t>& ids, bool replace_invalid_utf8 = false) const;
    [[nodiscard]] std::vector<std::vector<std::int64_t>> pack(
        const std::vector<std::int64_t>& ids) const;
    [[nodiscard]] std::vector<std::int64_t> unpack(
        const std::vector<std::vector<std::int64_t>>& patches) const;
    [[nodiscard]] std::size_t patch_size() const noexcept { return patch_size_; }
private:
    std::size_t patch_size_;
};

class SparseOperatorAdapter final {
public:
    SparseOperatorAdapter(Tensor left, Tensor right, double maximum_update);
    [[nodiscard]] Tensor forward(const Tensor& state,
        const Tensor& coefficients) const;
    [[nodiscard]] const Tensor& left() const noexcept { return left_; }
    [[nodiscard]] const Tensor& right() const noexcept { return right_; }
private:
    Tensor left_;
    Tensor right_;
    double maximum_update_{};
};

struct MosaicTextModelProfile final {
    std::uint64_t parameter_count{};
    double raw_weight_mib_bf16{};
    double raw_weight_mib_fp32{};
    bool recurrent_depth_parameter_invariant{true};
    [[nodiscard]] JsonValue::Object receipt(const MosaicTextConfig& config) const;
};

using MosaicTokenBatch = std::vector<std::vector<std::int64_t>>;

struct MosaicTransformerBlockWeights final {
    std::vector<float> attention_norm_weight, attention_norm_bias;
    std::vector<float> attention_in_projection_weight, attention_in_projection_bias;
    std::vector<float> attention_out_projection_weight, attention_out_projection_bias;
    std::vector<float> feedforward_norm_weight, feedforward_norm_bias;
    std::vector<float> feedforward_in_weight, feedforward_in_bias;
    std::vector<float> feedforward_out_weight, feedforward_out_bias;
    void validate(const MosaicTextConfig& config) const;
};

// PyTorch state_dict topology of MosaicTextLM. Matrices retain nn.Linear's
// row-major [out_features, in_features] layout and GRU gates use r,z,n order.
struct MosaicTextWeights final {
    std::vector<float> byte_embedding;
    std::vector<float> patch_projection_weight, patch_projection_bias;
    std::vector<float> patch_norm_weight, patch_norm_bias;
    std::vector<float> segment_embedding;
    std::vector<float> workspace, bos_patch;
    std::vector<float> retriever_projection_weight, retriever_projection_bias;
    std::vector<float> round_embedding;
    std::vector<MosaicTransformerBlockWeights> blocks;
    std::vector<float> operator_left, operator_right;
    std::vector<float> decoder_weight_ih, decoder_weight_hh;
    std::vector<float> decoder_bias_ih, decoder_bias_hh;
    std::vector<float> output_norm_weight, output_norm_bias;
    std::vector<float> lm_head_weight, lm_head_bias;
    void validate(const MosaicTextConfig& config) const;
};

struct MosaicTextOutput final {
    Tensor logits;
    std::optional<double> loss;
    std::size_t rounds{};
    std::vector<std::uint8_t> target_mask;
    Tensor decoder_states;
    Tensor context_states;
};

// Exact public form of MosaicTextLM._pad_tokens + _encode_patches used by the
// unified model before its recurrent text decoder runs.  `states` includes the
// learned BOS patch at index zero; `patch_mask` describes body patches only.
struct MosaicEncodedTextSource final {
    Tensor states;
    BooleanMask patch_mask;
};

class MosaicTextLM final {
public:
    MosaicTextLM(MosaicTextConfig config, MosaicTextWeights weights);
    [[nodiscard]] const MosaicTextConfig& config() const noexcept { return config_; }
    [[nodiscard]] const MosaicTextWeights& weights() const noexcept { return weights_; }
    [[nodiscard]] MosaicEncodedTextSource encode_unified_source(
        const MosaicTokenBatch& input_ids) const;
    [[nodiscard]] MosaicTextOutput forward(
        const MosaicTokenBatch& input_ids,
        const MosaicTokenBatch* targets = nullptr,
        std::optional<std::size_t> rounds = std::nullopt,
        const MosaicTokenBatch* memory_ids = nullptr,
        const Tensor* memory_summary = nullptr,
        const Tensor* operator_coefficients = nullptr) const;
    [[nodiscard]] MosaicTokenBatch generate(
        const MosaicTokenBatch& input_ids,
        std::size_t max_new_bytes,
        std::optional<std::size_t> rounds = std::nullopt,
        const MosaicTokenBatch* memory_ids = nullptr,
        const Tensor* memory_summary = nullptr,
        const Tensor* operator_coefficients = nullptr) const;
private:
    MosaicTextConfig config_;
    MosaicTextWeights weights_;
};

[[nodiscard]] MosaicTextModelProfile profile_mosaic_text_model(
    const MosaicTextConfig& config);

inline constexpr std::string_view mosaic_text_lm_source_sha256 =
    "19d9c4a9cbb8af540534af51e97fe88791752dec06e5f3cb5421be45d5cb140e";

}  // namespace swegca::world
