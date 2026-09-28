#include "world/recurrent_cognition.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <unordered_set>

namespace swegca::world {
namespace {

#ifdef SWEGCA_RECURRENT_USE_CBLAS
extern "C" void cblas_sgemm(int layout, int transpose_a, int transpose_b,
                             int rows, int columns, int shared,
                             float alpha, const float* left, int leading_left,
                             const float* right, int leading_right,
                             float beta, float* output, int leading_output);
constexpr int cblas_row_major = 101;
constexpr int cblas_no_transpose = 111;
constexpr int cblas_transpose = 112;
#endif

constexpr float layer_norm_epsilon = 1.0e-5F;

[[nodiscard]] std::size_t checked_product(const std::size_t left,
                                          const std::size_t right) {
    if (left != 0 && right > std::numeric_limits<std::size_t>::max() / left) {
        throw std::overflow_error("recurrent cognition tensor size overflow");
    }
    return left * right;
}

[[nodiscard]] std::size_t checked_add(const std::size_t left,
                                      const std::size_t right) {
    if (right > std::numeric_limits<std::size_t>::max() - left) {
        throw std::overflow_error("recurrent cognition tensor size overflow");
    }
    return left + right;
}

[[nodiscard]] std::size_t checked_size(const std::uint64_t value) {
    if (value > std::numeric_limits<std::size_t>::max()) {
        throw std::overflow_error("recurrent cognition dimension exceeds size_t");
    }
    return static_cast<std::size_t>(value);
}

void require_size(const std::span<const float> value, const std::size_t expected,
                  const char* name) {
    if (value.size() != expected) {
        throw std::invalid_argument(std::string(name) + " shape mismatch");
    }
}

[[nodiscard]] float sigmoid(const float value) noexcept {
    if (value >= 0.0F) {
        const float exponential = std::exp(-value);
        return 1.0F / (1.0F + exponential);
    }
    const float exponential = std::exp(value);
    return exponential / (1.0F + exponential);
}

[[nodiscard]] float softplus(const float value) noexcept {
    return std::max(value, 0.0F) + std::log1p(std::exp(-std::abs(value)));
}

void layer_norm(const std::span<const float> input, const std::span<const float> weight,
                const std::span<const float> bias, const std::size_t rows,
                const std::size_t hidden, std::span<float> output) {
    for (std::size_t row = 0; row != rows; ++row) {
        const auto offset = row * hidden;
        float mean = 0.0F;
        for (std::size_t column = 0; column != hidden; ++column) {
            mean += input[offset + column];
        }
        mean /= static_cast<float>(hidden);
        float variance = 0.0F;
        for (std::size_t column = 0; column != hidden; ++column) {
            const float difference = input[offset + column] - mean;
            variance += difference * difference;
        }
        variance /= static_cast<float>(hidden);
        const float inverse = 1.0F / std::sqrt(variance + layer_norm_epsilon);
        for (std::size_t column = 0; column != hidden; ++column) {
            output[offset + column] =
                (input[offset + column] - mean) * inverse * weight[column] + bias[column];
        }
    }
}

void linear(const std::span<const float> input, const std::span<const float> weight,
            const std::span<const float> bias, const std::size_t rows,
            const std::size_t input_size, const std::size_t output_size,
            std::span<float> output) {
#ifdef SWEGCA_RECURRENT_USE_CBLAS
    constexpr auto int_max = static_cast<std::size_t>(std::numeric_limits<int>::max());
    if (rows > int_max || input_size > int_max || output_size > int_max) {
        throw std::length_error("recurrent cognition matrix exceeds CBLAS integer range");
    }
    cblas_sgemm(cblas_row_major, cblas_no_transpose, cblas_transpose,
                static_cast<int>(rows), static_cast<int>(output_size),
                static_cast<int>(input_size), 1.0F, input.data(),
                static_cast<int>(input_size), weight.data(),
                static_cast<int>(input_size), 0.0F, output.data(),
                static_cast<int>(output_size));
    if (!bias.empty()) {
        for (std::size_t row = 0; row != rows; ++row) {
            for (std::size_t out = 0; out != output_size; ++out) {
                output[row * output_size + out] += bias[out];
            }
        }
    }
#else
    for (std::size_t row = 0; row != rows; ++row) {
        for (std::size_t out = 0; out != output_size; ++out) {
            float value = bias.empty() ? 0.0F : bias[out];
            const auto input_offset = row * input_size;
            const auto weight_offset = out * input_size;
            for (std::size_t in = 0; in != input_size; ++in) {
                value += input[input_offset + in] * weight[weight_offset + in];
            }
            output[row * output_size + out] = value;
        }
    }
#endif
}

struct DenseBatch final {
    std::size_t batch{};
    std::size_t tokens{};
    std::size_t hidden{};
    std::vector<float> values;
};

[[nodiscard]] DenseBatch tensor_values(const Tensor& tensor, const char* name) {
    if (tensor.dtype() != TensorDType::float32 || tensor.device() != "cpu" ||
        tensor.shape().size() != 3) {
        throw std::invalid_argument(std::string(name) + " must be CPU float32 [batch,tokens,hidden]");
    }
    DenseBatch result{checked_size(tensor.shape()[0]),
                      checked_size(tensor.shape()[1]),
                      checked_size(tensor.shape()[2]), {}};
    const auto expected = checked_product(checked_product(result.batch, result.tokens),
                                          result.hidden);
    if (tensor.values().size() != expected) {
        throw std::invalid_argument(std::string(name) + " storage size mismatch");
    }
    result.values.reserve(tensor.values().size());
    for (const auto value : tensor.values()) result.values.push_back(static_cast<float>(value));
    return result;
}

[[nodiscard]] Tensor tensor_from_values(const DenseBatch& value, const std::size_t begin,
                                        const std::size_t count) {
    std::vector<double> output;
    output.reserve(checked_product(checked_product(value.batch, count), value.hidden));
    for (std::size_t batch = 0; batch != value.batch; ++batch) {
        const auto offset = checked_product(
            checked_add(checked_product(batch, value.tokens), begin), value.hidden);
        const auto copy_count = checked_product(count, value.hidden);
        for (std::size_t index = 0; index != copy_count; ++index) {
            output.push_back(static_cast<double>(value.values[offset + index]));
        }
    }
    return Tensor(TensorDType::float32,
                  {value.batch, count, value.hidden}, std::move(output), "cpu");
}

[[nodiscard]] DenseBatch join_state(const CognitiveState& state,
                                    const RecurrentCognitionConfig& config,
                                    const RecurrentCognitionWeights& weights) {
    state.validate(config.state);
    const auto semantic = tensor_values(state.semantic_slots(), "semantic slots");
    const auto executive = tensor_values(state.executive_slots(), "executive slots");
    const auto scratch = tensor_values(state.scratch_slots(), "scratch slots");
    if (semantic.batch != executive.batch || semantic.batch != scratch.batch ||
        semantic.hidden != config.state.hidden_dim ||
        executive.hidden != semantic.hidden || scratch.hidden != semantic.hidden) {
        throw std::invalid_argument("cognitive state partition mismatch");
    }
    const auto total_tokens = checked_add(
        checked_add(semantic.tokens, executive.tokens), scratch.tokens);
    DenseBatch joined{semantic.batch, total_tokens, semantic.hidden,
                      std::vector<float>(checked_product(
                          checked_product(semantic.batch, total_tokens), semantic.hidden))};
    const std::array<const DenseBatch*, 3> groups{&semantic, &executive, &scratch};
    std::size_t token_base = 0;
    for (std::size_t role = 0; role != groups.size(); ++role) {
        const auto& group = *groups[role];
        for (std::size_t batch = 0; batch != joined.batch; ++batch) {
            for (std::size_t token = 0; token != group.tokens; ++token) {
                for (std::size_t hidden = 0; hidden != joined.hidden; ++hidden) {
                    joined.values[((batch * joined.tokens + token_base + token) * joined.hidden) + hidden] =
                        group.values[((batch * group.tokens + token) * group.hidden) + hidden] +
                        weights.role_embeddings[role * joined.hidden + hidden];
                }
            }
        }
        token_base = checked_add(token_base, group.tokens);
    }
    return joined;
}

[[nodiscard]] DenseBatch shared_cell(
    const DenseBatch& tokens, const std::span<const std::uint8_t> active_mask,
    const std::span<const float> attention_key_weights,
    const RecurrentCognitionConfig& config, const RecurrentCognitionWeights& weights) {
    const auto rows = checked_product(tokens.batch, tokens.tokens);
    const auto hidden = tokens.hidden;
    const auto heads = static_cast<std::size_t>(config.attention_heads);
    const auto head_dim = hidden / heads;
    const auto row_values = checked_product(rows, hidden);
    std::vector<float> normalized(row_values);
    layer_norm(tokens.values, weights.attention_norm_weight, weights.attention_norm_bias,
               rows, hidden, normalized);

    const auto packed_hidden = checked_product(hidden, std::size_t{3});
    std::vector<float> packed(checked_product(rows, packed_hidden));
    linear(normalized, weights.attention_in_projection_weight,
           weights.attention_in_projection_bias, rows, hidden, packed_hidden, packed);
    std::vector<float> attention(row_values, 0.0F);
    std::vector<float> scores(tokens.tokens);
    std::vector<float> probabilities(tokens.tokens);
    const float scale = 1.0F / std::sqrt(static_cast<float>(head_dim));
    for (std::size_t batch = 0; batch != tokens.batch; ++batch) {
        for (std::size_t head = 0; head != heads; ++head) {
            for (std::size_t query = 0; query != tokens.tokens; ++query) {
                float maximum = -std::numeric_limits<float>::infinity();
                bool has_active_key = false;
                bool has_nan_active_score = false;
                for (std::size_t key = 0; key != tokens.tokens; ++key) {
                    float score = -std::numeric_limits<float>::infinity();
                    if (active_mask[batch * tokens.tokens + key] != 0) {
                        has_active_key = true;
                        score = 0.0F;
                        for (std::size_t element = 0; element != head_dim; ++element) {
                            const auto q = ((batch * tokens.tokens + query) * hidden * 3) +
                                           head * head_dim + element;
                            const auto k = ((batch * tokens.tokens + key) * hidden * 3) + hidden +
                                           head * head_dim + element;
                            score += packed[q] * packed[k];
                        }
                        score *= scale;
                        if (!attention_key_weights.empty()) {
                            score += std::log(attention_key_weights[batch * tokens.tokens + key]);
                        }
                        has_nan_active_score = has_nan_active_score || std::isnan(score);
                    }
                    scores[key] = score;
                    maximum = std::max(maximum, score);
                }
                float denominator = 0.0F;
                if (!has_active_key) {
                    std::fill(probabilities.begin(), probabilities.end(), 0.0F);
                } else if (has_nan_active_score || !std::isfinite(maximum)) {
                    std::fill(probabilities.begin(), probabilities.end(),
                              std::numeric_limits<float>::quiet_NaN());
                } else {
                    for (std::size_t key = 0; key != tokens.tokens; ++key) {
                        probabilities[key] = active_mask[batch * tokens.tokens + key] != 0
                            ? std::exp(scores[key] - maximum) : 0.0F;
                        denominator += probabilities[key];
                    }
                }
                if (denominator != 0.0F) {
                    for (auto& probability : probabilities) probability /= denominator;
                }
                for (std::size_t element = 0; element != head_dim; ++element) {
                    float value = 0.0F;
                    for (std::size_t key = 0; key != tokens.tokens; ++key) {
                        const auto v = ((batch * tokens.tokens + key) * hidden * 3) + hidden * 2 +
                                       head * head_dim + element;
                        value += probabilities[key] * packed[v];
                    }
                    attention[(batch * tokens.tokens + query) * hidden + head * head_dim + element] = value;
                }
            }
        }
    }
    std::vector<float> projected(row_values);
    linear(attention, weights.attention_out_projection_weight,
           weights.attention_out_projection_bias, rows, hidden, hidden, projected);
    attention.swap(projected);

    std::vector<float> attended(row_values);
    for (std::size_t index = 0; index != attended.size(); ++index) {
        attended[index] = tokens.values[index] + attention[index];
    }
    std::vector<float> mlp_normalized(row_values);
    layer_norm(attended, weights.mlp_norm_weight, weights.mlp_norm_bias,
               rows, hidden, mlp_normalized);
    const auto mlp_hidden = static_cast<std::size_t>(config.mlp_hidden_dim);
    const auto packed_mlp = checked_product(mlp_hidden, std::size_t{2});
    std::vector<float> mlp_packed(checked_product(rows, packed_mlp));
    linear(mlp_normalized, weights.mlp_in_weight, {}, rows, hidden,
           packed_mlp, mlp_packed);
    std::vector<float> gated(checked_product(rows, mlp_hidden));
    for (std::size_t row = 0; row != rows; ++row) {
        for (std::size_t index = 0; index != mlp_hidden; ++index) {
            const float left = mlp_packed[row * mlp_hidden * 2 + index];
            const float right = mlp_packed[row * mlp_hidden * 2 + mlp_hidden + index];
            gated[row * mlp_hidden + index] = left * sigmoid(left) * right;
        }
    }
    std::vector<float> mlp(row_values);
    linear(gated, weights.mlp_out_weight, {}, rows, mlp_hidden, hidden, mlp);

    // The source invokes mlp_norm(attended) again for update_gate.
    std::vector<float> gate_normalized(row_values);
    layer_norm(attended, weights.mlp_norm_weight, weights.mlp_norm_bias,
               rows, hidden, gate_normalized);
    std::vector<float> gate(rows);
    linear(gate_normalized, weights.update_gate_weight, weights.update_gate_bias,
           rows, hidden, 1, gate);
    DenseBatch result{tokens.batch, tokens.tokens, tokens.hidden,
                      std::vector<float>(row_values)};
    for (std::size_t row = 0; row != rows; ++row) {
        const float gate_value = sigmoid(gate[row]);
        for (std::size_t column = 0; column != hidden; ++column) {
            const auto index = row * hidden + column;
            const float proposal = std::clamp(attention[index] + mlp[index],
                                              -config.maximum_update,
                                              config.maximum_update);
            result.values[index] = tokens.values[index] + gate_value * proposal;
        }
    }
    return result;
}

[[nodiscard]] bool python_space(const std::uint32_t value) noexcept {
    return (value >= 0x09U && value <= 0x0dU) ||
           (value >= 0x1cU && value <= 0x20U) || value == 0x85U ||
           value == 0xa0U || value == 0x1680U ||
           (value >= 0x2000U && value <= 0x200aU) ||
           value == 0x2028U || value == 0x2029U || value == 0x202fU ||
           value == 0x205fU || value == 0x3000U;
}

[[nodiscard]] bool blank_utf8(const std::string& value) {
    for (std::size_t index = 0; index < value.size();) {
        const auto first = static_cast<unsigned char>(value[index]);
        std::uint32_t codepoint = 0;
        std::size_t length = 0;
        if (first < 0x80U) {
            codepoint = first;
            length = 1;
        } else if (first >= 0xc2U && first <= 0xdfU) {
            codepoint = first & 0x1fU;
            length = 2;
        } else if (first >= 0xe0U && first <= 0xefU) {
            codepoint = first & 0x0fU;
            length = 3;
        } else if (first >= 0xf0U && first <= 0xf4U) {
            codepoint = first & 0x07U;
            length = 4;
        } else {
            throw std::invalid_argument("evidence_refs must contain valid UTF-8");
        }
        if (index + length > value.size()) {
            throw std::invalid_argument("evidence_refs must contain valid UTF-8");
        }
        for (std::size_t part = 1; part < length; ++part) {
            const auto continuation = static_cast<unsigned char>(value[index + part]);
            if ((continuation & 0xc0U) != 0x80U) {
                throw std::invalid_argument("evidence_refs must contain valid UTF-8");
            }
            codepoint = (codepoint << 6U) | (continuation & 0x3fU);
        }
        const bool overlong = (length == 2 && codepoint < 0x80U) ||
                              (length == 3 && codepoint < 0x800U) ||
                              (length == 4 && codepoint < 0x10000U);
        if (overlong || (codepoint >= 0xd800U && codepoint <= 0xdfffU) ||
            codepoint > 0x10ffffU) {
            throw std::invalid_argument("evidence_refs must contain valid UTF-8");
        }
        if (!python_space(codepoint)) return false;
        index += length;
    }
    return true;
}

}  // namespace

void RecurrentCognitionConfig::validate() const {
    state.validate();
    if (attention_heads == 0 || mlp_hidden_dim == 0) {
        throw std::invalid_argument("attention_heads and mlp_hidden_dim must be positive");
    }
    if (state.hidden_dim % attention_heads != 0) {
        throw std::invalid_argument("state hidden_dim must be divisible by attention_heads");
    }
    if (minimum_cycles == 0 || minimum_cycles > maximum_cycles) {
        throw std::invalid_argument("cycle limits must satisfy 1 <= minimum <= maximum");
    }
    if (!(halt_threshold > 0.0F && halt_threshold <= 1.0F)) {
        throw std::invalid_argument("halt_threshold must be within (0, 1]");
    }
    if (!(evidence_logit_epsilon > 0.0F && evidence_logit_epsilon < 0.5F)) {
        throw std::invalid_argument("evidence_logit_epsilon must be within (0, 0.5)");
    }
    if (!std::isfinite(maximum_update) || maximum_update <= 0.0F) {
        throw std::invalid_argument("maximum_update must be finite and positive");
    }
}

void RecurrentCognitionWeights::validate(const RecurrentCognitionConfig& config) const {
    config.validate();
    const auto hidden = static_cast<std::size_t>(config.state.hidden_dim);
    const auto mlp = static_cast<std::size_t>(config.mlp_hidden_dim);
    require_size(attention_norm_weight, hidden, "attention_norm.weight");
    require_size(attention_norm_bias, hidden, "attention_norm.bias");
    const auto packed_hidden = checked_product(hidden, std::size_t{3});
    const auto packed_mlp = checked_product(mlp, std::size_t{2});
    require_size(attention_in_projection_weight, checked_product(packed_hidden, hidden),
                 "attention.in_proj_weight");
    require_size(attention_in_projection_bias, packed_hidden, "attention.in_proj_bias");
    require_size(attention_out_projection_weight, checked_product(hidden, hidden),
                 "attention.out_proj.weight");
    require_size(attention_out_projection_bias, hidden, "attention.out_proj.bias");
    require_size(mlp_norm_weight, hidden, "mlp_norm.weight");
    require_size(mlp_norm_bias, hidden, "mlp_norm.bias");
    require_size(mlp_in_weight, checked_product(packed_mlp, hidden), "mlp_in.weight");
    require_size(mlp_out_weight, checked_product(hidden, mlp), "mlp_out.weight");
    require_size(update_gate_weight, hidden, "update_gate.weight");
    require_size(update_gate_bias, 1, "update_gate.bias");
    require_size(role_embeddings, packed_hidden, "role_embeddings");
    require_size(final_norm_weight, hidden, "final_norm.weight");
    require_size(final_norm_bias, hidden, "final_norm.bias");
    require_size(halt_head_weight, hidden, "halt_head.weight");
    require_size(halt_head_bias, 1, "halt_head.bias");
}

RecurrentCognitionCore::RecurrentCognitionCore(RecurrentCognitionConfig config,
                                               RecurrentCognitionWeights weights)
    : config_(std::move(config)), weights_(std::move(weights)) {
    weights_.validate(config_);
}

RecurrentCognitionOutput RecurrentCognitionCore::run(
    const CognitiveState& state, const RecurrentEvidence* evidence,
    const RecurrentExecutionMode mode) const {
    if (mode != RecurrentExecutionMode::evaluation &&
        mode != RecurrentExecutionMode::training_selection) {
        throw std::invalid_argument("invalid recurrent execution mode");
    }
    auto slots = join_state(state, config_, weights_);
    const auto batch = slots.batch;
    const auto state_tokens = slots.tokens;
    DenseBatch evidence_values{batch, 0, slots.hidden, {}};
    std::vector<std::uint8_t> evidence_mask;
    std::vector<float> coverage(batch, 0.0F);
    std::vector<float> sequence_weights;
    if (evidence != nullptr) {
        if (evidence->tokens.has_value()) {
            evidence_values = tensor_values(*evidence->tokens, "evidence_tokens");
            if (!evidence->mask.has_value() || evidence_values.batch != batch ||
                evidence_values.hidden != slots.hidden ||
                evidence->mask->shape().size() != 2 ||
                checked_size(evidence->mask->shape()[0]) != batch ||
                checked_size(evidence->mask->shape()[1]) != evidence_values.tokens) {
                throw std::invalid_argument(
                    "evidence tokens and mask must match state batch and hidden dimensions");
            }
            evidence_mask.assign(evidence->mask->values().begin(),
                                 evidence->mask->values().end());
        }
        if (evidence->coverage.has_value()) {
            if (evidence->coverage->size() != batch) {
                throw std::invalid_argument("evidence_coverage must have shape [batch]");
            }
            coverage = *evidence->coverage;
        } else if (evidence_values.tokens != 0) {
            for (std::size_t row = 0; row != batch; ++row) {
                std::size_t active = 0;
                for (std::size_t token = 0; token != evidence_values.tokens; ++token) {
                    active += evidence_mask[row * evidence_values.tokens + token] != 0 ? 1 : 0;
                }
                coverage[row] = static_cast<float>(active) /
                                static_cast<float>(evidence_values.tokens);
            }
        }
        if (evidence->confidence.has_value()) {
            if (evidence->confidence->size() != batch) {
                throw std::invalid_argument("evidence_confidence must have shape [batch]");
            }
            for (std::size_t row = 0; row != batch; ++row) {
                const auto confidence = (*evidence->confidence)[row];
                if (!std::isfinite(confidence) || confidence < 0.0F || confidence > 1.0F) {
                    throw std::invalid_argument("evidence_confidence must be finite within [0, 1]");
                }
                coverage[row] *= confidence;
            }
        }
        if (evidence->attention_key_weights.has_value()) {
            const auto evidence_weight_count = checked_product(batch, evidence_values.tokens);
            if (evidence->attention_key_weights->size() != evidence_weight_count) {
                throw std::invalid_argument("evidence_attention_weight must match evidence_mask");
            }
            const auto weighted_tokens = checked_add(state_tokens, evidence_values.tokens);
            sequence_weights.assign(checked_product(batch, weighted_tokens), 1.0F);
            for (std::size_t row = 0; row != batch; ++row) {
                for (std::size_t token = 0; token != evidence_values.tokens; ++token) {
                    const auto value = (*evidence->attention_key_weights)[
                        row * evidence_values.tokens + token];
                    if (!std::isfinite(value) || value <= 0.0F) {
                        throw std::invalid_argument(
                            "evidence_attention_weight must be finite and positive");
                    }
                    sequence_weights[row * weighted_tokens +
                                     state_tokens + token] = value;
                }
            }
        }
    }

    const auto total_tokens = checked_add(state_tokens, evidence_values.tokens);
    const auto sequence_elements = checked_product(batch, total_tokens);
    std::vector<std::uint8_t> sequence_mask(sequence_elements, 1);
    for (std::size_t row = 0; row != batch; ++row) {
        for (std::size_t token = 0; token != evidence_values.tokens; ++token) {
            sequence_mask[row * total_tokens + state_tokens + token] =
                evidence_mask[row * evidence_values.tokens + token];
        }
    }
    std::vector<std::uint8_t> finished(batch, 0);
    std::vector<std::uint8_t> has_selected(batch, 0);
    std::vector<std::uint64_t> cycles_used(batch, 0);
    auto selected = slots.values;
    CognitionTrace trace;
    const auto hidden = slots.hidden;
    const auto semantic_end = static_cast<std::size_t>(config_.state.semantic_slots);
    const auto executive_end = checked_add(
        semantic_end, checked_size(config_.state.executive_slots));
    for (std::uint64_t cycle = 0; cycle != config_.maximum_cycles; ++cycle) {
        DenseBatch sequence{batch, total_tokens, hidden,
                            std::vector<float>(checked_product(sequence_elements, hidden))};
        const auto state_row_values = checked_product(state_tokens, hidden);
        const auto total_row_values = checked_product(total_tokens, hidden);
        const auto evidence_row_values = checked_product(evidence_values.tokens, hidden);
        for (std::size_t row = 0; row != batch; ++row) {
            std::copy_n(slots.values.begin() + static_cast<std::ptrdiff_t>(
                            checked_product(row, state_row_values)),
                        state_row_values,
                        sequence.values.begin() + static_cast<std::ptrdiff_t>(
                            checked_product(row, total_row_values)));
            std::copy_n(evidence_values.values.begin() + static_cast<std::ptrdiff_t>(
                            checked_product(row, evidence_row_values)),
                        evidence_row_values,
                        sequence.values.begin() + static_cast<std::ptrdiff_t>(checked_product(
                            checked_add(checked_product(row, total_tokens), state_tokens), hidden)));
        }
        const auto proposal = shared_cell(sequence, sequence_mask, sequence_weights,
                                          config_, weights_);
        for (std::size_t row = 0; row != batch; ++row) {
            if (mode == RecurrentExecutionMode::evaluation && finished[row] != 0) continue;
            std::copy_n(proposal.values.begin() + static_cast<std::ptrdiff_t>(row * total_tokens * hidden),
                        state_row_values,
                        slots.values.begin() + static_cast<std::ptrdiff_t>(
                            checked_product(row, state_row_values)));
        }
        for (std::size_t row = 0; row != batch; ++row) cycles_used[row] += finished[row] == 0 ? 1 : 0;

        std::vector<float> logits(batch);
        std::vector<float> probabilities(batch);
        for (std::size_t row = 0; row != batch; ++row) {
            float head = weights_.halt_head_bias.front();
            for (std::size_t column = 0; column != hidden; ++column) {
                float mean = 0.0F;
                for (std::size_t token = semantic_end; token != executive_end; ++token) {
                    mean += slots.values[(row * state_tokens + token) * hidden + column];
                }
                mean /= static_cast<float>(config_.state.executive_slots);
                head += mean * weights_.halt_head_weight[column];
            }
            const float bounded = std::clamp(coverage[row], config_.evidence_logit_epsilon,
                                             1.0F - config_.evidence_logit_epsilon);
            logits[row] = head + softplus(weights_.halt_evidence_scale) *
                                 (std::log(bounded) - std::log1p(-bounded));
            probabilities[row] = sigmoid(logits[row]);
        }
        trace.halt_logits.push_back(logits);
        trace.halt_probabilities.push_back(probabilities);
        if (cycle + 1 >= config_.minimum_cycles) {
            bool all_finished = true;
            for (std::size_t row = 0; row != batch; ++row) {
                const bool newly_finished = finished[row] == 0 &&
                    probabilities[row] >= config_.halt_threshold;
                if (mode == RecurrentExecutionMode::training_selection && newly_finished) {
                    std::copy_n(slots.values.begin() + static_cast<std::ptrdiff_t>(row * state_tokens * hidden),
                                state_row_values,
                                selected.begin() + static_cast<std::ptrdiff_t>(
                                    checked_product(row, state_row_values)));
                    has_selected[row] = 1;
                }
                if (probabilities[row] >= config_.halt_threshold) finished[row] = 1;
                all_finished = all_finished && finished[row] != 0;
            }
            if (mode == RecurrentExecutionMode::evaluation && all_finished) break;
        }
    }
    if (mode == RecurrentExecutionMode::training_selection) {
        for (std::size_t row = 0; row != batch; ++row) {
            if (has_selected[row] == 0) continue;
            std::copy_n(selected.begin() + static_cast<std::ptrdiff_t>(row * state_tokens * hidden),
                        checked_product(state_tokens, hidden),
                        slots.values.begin() + static_cast<std::ptrdiff_t>(checked_product(
                            row, checked_product(state_tokens, hidden))));
        }
    }
    std::vector<float> normalized(slots.values.size());
    layer_norm(slots.values, weights_.final_norm_weight, weights_.final_norm_bias,
               checked_product(batch, state_tokens), hidden, normalized);
    slots.values.swap(normalized);
    trace.cycles_used = std::move(cycles_used);
    const auto semantic_count = checked_size(config_.state.semantic_slots);
    const auto executive_count = checked_size(config_.state.executive_slots);
    const auto scratch_count = checked_size(config_.state.scratch_slots);
    CognitiveState updated(
        tensor_from_values(slots, 0, semantic_count),
        tensor_from_values(slots, semantic_count, executive_count),
        tensor_from_values(slots, checked_add(semantic_count, executive_count), scratch_count),
        state.structured_world_graph(),
        std::vector<std::string>(state.evidence_refs().begin(), state.evidence_refs().end()),
        state.goal_state(), state.value_state(), state.self_state(),
        std::string(state.owner_id()));
    return {std::move(updated), std::move(trace)};
}

RecurrentCognitionOutput integrate_authorized_evidence(
    const RecurrentCognitionCore& core, const CognitiveState& state,
    const RecurrentEvidence& evidence, const std::span<const std::string> evidence_refs,
    const bool authorized, const std::uint64_t maximum_evidence_refs,
    const RecurrentExecutionMode mode) {
    if (maximum_evidence_refs == 0) {
        throw std::invalid_argument("maximum_evidence_refs must be positive");
    }
    for (const auto& reference : evidence_refs) {
        if (blank_utf8(reference)) {
            throw std::invalid_argument("evidence_refs must not contain empty addresses");
        }
    }
    if (!evidence.tokens.has_value() || !evidence.mask.has_value() ||
        !evidence.coverage.has_value() || !evidence.confidence.has_value()) {
        throw std::invalid_argument(
            "authorized evidence adapter requires tokens, mask, coverage, and confidence");
    }
    // Validate rank before reading any dimension, on both authorized and denied paths.
    if (evidence.tokens->shape().size() != 3 || evidence.mask->shape().size() != 2) {
        throw std::invalid_argument("authorized evidence adapter shape mismatch");
    }
    RecurrentEvidence routed = evidence;
    if (!authorized) {
        routed.mask = BooleanMask(
            std::vector<std::uint64_t>(evidence.mask->shape().begin(),
                                       evidence.mask->shape().end()),
            std::vector<std::uint8_t>(evidence.mask->values().size(), 0));
        const auto routed_batch = checked_size(evidence.tokens->shape()[0]);
        routed.coverage = std::vector<float>(routed_batch, 0.0F);
        routed.confidence = std::vector<float>(routed_batch, 0.0F);
    }
    auto output = core.run(state, &routed, mode);
    if (!authorized) return output;

    std::vector<std::string> merged;
    std::unordered_set<std::string> seen;
    for (const auto& reference : state.evidence_refs()) {
        if (seen.insert(reference).second) merged.push_back(reference);
    }
    for (const auto& reference : evidence_refs) {
        if (seen.insert(reference).second) merged.push_back(reference);
    }
    if (merged.size() > maximum_evidence_refs) {
        merged.erase(merged.begin(), merged.end() - static_cast<std::ptrdiff_t>(maximum_evidence_refs));
    }
    output.state = CognitiveState(
        output.state.semantic_slots().clone(), output.state.executive_slots().clone(),
        output.state.scratch_slots().clone(), output.state.structured_world_graph(),
        std::move(merged), output.state.goal_state(), output.state.value_state(),
        output.state.self_state(), std::string(output.state.owner_id()));
    return output;
}

}  // namespace swegca::world
