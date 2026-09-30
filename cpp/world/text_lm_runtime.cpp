#include "world/text_lm.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <span>
#include <stdexcept>

namespace swegca::world {
namespace {

std::size_t product(const std::initializer_list<std::size_t> dimensions) {
    std::size_t result = 1;
    for (const auto dimension : dimensions) {
        if (dimension && result > std::numeric_limits<std::size_t>::max() / dimension)
            throw std::overflow_error("MOSAIC text tensor size overflow");
        result *= dimension;
    }
    return result;
}

void require_size(const std::vector<float>& values, const std::size_t expected,
                  const char* name) {
    if (values.size() != expected)
        throw std::invalid_argument(std::string(name) + " shape mismatch");
}

void validate_tokens(const MosaicTokenBatch& tokens, const char* name,
                     const bool require_bos = false) {
    if (tokens.empty() || tokens.front().empty())
        throw std::invalid_argument(std::string(name) + " must be a nonempty rank-2 tensor");
    const auto width = tokens.front().size();
    for (const auto& row : tokens) {
        if (row.size() != width) throw std::invalid_argument(std::string(name) + " must be rectangular");
        if (require_bos && row.front() != mosaic_bos_id)
            throw std::invalid_argument("every input sequence must begin with BOS_ID");
        for (const auto value : row)
            if (value < 0 || value >= mosaic_vocab_size)
                throw std::invalid_argument(std::string(name) + " contains ids outside the byte vocabulary");
    }
}

struct Dense final {
    std::size_t batch{}, tokens{}, hidden{};
    std::vector<float> values;
    float& at(const std::size_t b, const std::size_t t, const std::size_t d) {
        return values[(b * tokens + t) * hidden + d];
    }
    const float& at(const std::size_t b, const std::size_t t, const std::size_t d) const {
        return values[(b * tokens + t) * hidden + d];
    }
};

struct PatchBatch final {
    std::size_t batch{}, patches{}, size{};
    std::vector<std::int64_t> values;
    std::int64_t& at(const std::size_t b, const std::size_t p, const std::size_t s) {
        return values[(b * patches + p) * size + s];
    }
    const std::int64_t& at(const std::size_t b, const std::size_t p, const std::size_t s) const {
        return values[(b * patches + p) * size + s];
    }
};

PatchBatch pad_tokens(const MosaicTokenBatch& tokens, const std::size_t patch_size,
                      const std::int64_t fill) {
    const auto patch_count = std::max<std::size_t>(
        1, (tokens.front().size() + patch_size - 1) / patch_size);
    PatchBatch result{tokens.size(), patch_count, patch_size,
        std::vector<std::int64_t>(product({tokens.size(), patch_count, patch_size}), fill)};
    for (std::size_t batch = 0; batch < tokens.size(); ++batch)
        for (std::size_t index = 0; index < tokens[batch].size(); ++index)
            result.at(batch, index / patch_size, index % patch_size) = tokens[batch][index];
    return result;
}

void linear(const std::span<const float> input, const std::span<const float> weight,
            const std::span<const float> bias, const std::size_t rows,
            const std::size_t in, const std::size_t out, std::span<float> output) {
    for (std::size_t row = 0; row < rows; ++row) {
        for (std::size_t destination = 0; destination < out; ++destination) {
            float value = bias.empty() ? 0.0F : bias[destination];
            for (std::size_t source = 0; source < in; ++source)
                value += input[row * in + source] * weight[destination * in + source];
            output[row * out + destination] = value;
        }
    }
}

void layer_norm(const std::span<const float> input, const std::span<const float> weight,
                const std::span<const float> bias, const std::size_t rows,
                const std::size_t hidden, std::span<float> output) {
    constexpr float epsilon = 1.0e-5F;
    for (std::size_t row = 0; row < rows; ++row) {
        float mean = 0.0F;
        for (std::size_t column = 0; column < hidden; ++column) mean += input[row * hidden + column];
        mean /= static_cast<float>(hidden);
        float variance = 0.0F;
        for (std::size_t column = 0; column < hidden; ++column) {
            const auto difference = input[row * hidden + column] - mean;
            variance += difference * difference;
        }
        variance /= static_cast<float>(hidden);
        const auto inverse = 1.0F / std::sqrt(variance + epsilon);
        for (std::size_t column = 0; column < hidden; ++column)
            output[row * hidden + column] =
                (input[row * hidden + column] - mean) * inverse * weight[column] + bias[column];
    }
}

Dense encode_patches(const PatchBatch& patches, const MosaicTextConfig& config,
                     const MosaicTextWeights& weights) {
    const auto rows = patches.batch * patches.patches;
    const auto flattened = config.patch_size * config.byte_embedding_dim;
    std::vector<float> embedded(product({rows, flattened}));
    for (std::size_t batch = 0; batch < patches.batch; ++batch)
        for (std::size_t patch = 0; patch < patches.patches; ++patch)
            for (std::size_t offset = 0; offset < patches.size; ++offset) {
                const auto token = static_cast<std::size_t>(patches.at(batch, patch, offset));
                for (std::size_t dim = 0; dim < config.byte_embedding_dim; ++dim)
                    embedded[((batch * patches.patches + patch) * flattened) +
                        offset * config.byte_embedding_dim + dim] =
                        weights.byte_embedding[token * config.byte_embedding_dim + dim];
            }
    Dense projected{patches.batch, patches.patches, config.model_dim,
        std::vector<float>(product({rows, config.model_dim}))};
    linear(embedded, weights.patch_projection_weight, weights.patch_projection_bias,
        rows, flattened, config.model_dim, projected.values);
    std::vector<float> normalized(projected.values.size());
    layer_norm(projected.values, weights.patch_norm_weight, weights.patch_norm_bias,
        rows, config.model_dim, normalized);
    projected.values = std::move(normalized);
    return projected;
}

float gelu(const float value) {
    return 0.5F * value * (1.0F + std::erf(value / std::sqrt(2.0F)));
}

void transformer_block(Dense& state, const std::span<const std::uint8_t> valid,
                       const MosaicTextConfig& config,
                       const MosaicTransformerBlockWeights& weights) {
    const auto rows = state.batch * state.tokens;
    const auto d = state.hidden;
    std::vector<float> normalized(state.values.size());
    layer_norm(state.values, weights.attention_norm_weight, weights.attention_norm_bias,
        rows, d, normalized);
    std::vector<float> qkv(product({rows, 3, d}));
    linear(normalized, weights.attention_in_projection_weight,
        weights.attention_in_projection_bias, rows, d, 3 * d, qkv);
    std::vector<float> attended(state.values.size());
    const auto head_dim = d / config.attention_heads;
    std::vector<float> scores(state.tokens), probabilities(state.tokens);
    const auto scale = 1.0F / std::sqrt(static_cast<float>(head_dim));
    for (std::size_t batch = 0; batch < state.batch; ++batch) {
        for (std::size_t head = 0; head < config.attention_heads; ++head) {
            for (std::size_t query = 0; query < state.tokens; ++query) {
                float maximum = -std::numeric_limits<float>::infinity();
                for (std::size_t key = 0; key < state.tokens; ++key) {
                    float score = -std::numeric_limits<float>::infinity();
                    if (key <= query && valid[batch * state.tokens + key]) {
                        score = 0.0F;
                        for (std::size_t inner = 0; inner < head_dim; ++inner) {
                            const auto q = ((batch * state.tokens + query) * 3 * d) + head * head_dim + inner;
                            const auto k = ((batch * state.tokens + key) * 3 * d) + d + head * head_dim + inner;
                            score += qkv[q] * qkv[k];
                        }
                        score *= scale;
                    }
                    scores[key] = score;
                    maximum = std::max(maximum, score);
                }
                float denominator = 0.0F;
                for (std::size_t key = 0; key < state.tokens; ++key) {
                    probabilities[key] = std::isfinite(scores[key]) ? std::exp(scores[key] - maximum) : 0.0F;
                    denominator += probabilities[key];
                }
                if (denominator > 0.0F) for (auto& value : probabilities) value /= denominator;
                for (std::size_t inner = 0; inner < head_dim; ++inner) {
                    float value = 0.0F;
                    for (std::size_t key = 0; key < state.tokens; ++key) {
                        const auto v = ((batch * state.tokens + key) * 3 * d) + 2 * d + head * head_dim + inner;
                        value += probabilities[key] * qkv[v];
                    }
                    attended[(batch * state.tokens + query) * d + head * head_dim + inner] = value;
                }
            }
        }
    }
    std::vector<float> attention_output(state.values.size());
    linear(attended, weights.attention_out_projection_weight,
        weights.attention_out_projection_bias, rows, d, d, attention_output);
    for (std::size_t index = 0; index < state.values.size(); ++index)
        state.values[index] += attention_output[index];

    layer_norm(state.values, weights.feedforward_norm_weight, weights.feedforward_norm_bias,
        rows, d, normalized);
    std::vector<float> hidden(product({rows, config.ffn_dim}));
    linear(normalized, weights.feedforward_in_weight, weights.feedforward_in_bias,
        rows, d, config.ffn_dim, hidden);
    for (auto& value : hidden) value = gelu(value);
    std::vector<float> output(state.values.size());
    linear(hidden, weights.feedforward_out_weight, weights.feedforward_out_bias,
        rows, config.ffn_dim, d, output);
    for (std::size_t index = 0; index < state.values.size(); ++index) state.values[index] += output[index];
}

void operator_adapter(Dense& state, const Tensor& coefficients,
                      const MosaicTextConfig& config, const MosaicTextWeights& weights) {
    if (coefficients.shape().size() != 2 || coefficients.shape()[0] != state.batch ||
        coefficients.shape()[1] != config.operator_basis_count)
        throw std::invalid_argument("operator_coefficients shape mismatch");
    std::vector<float> delta(config.model_dim), projected(config.operator_rank);
    for (std::size_t batch = 0; batch < state.batch; ++batch)
        for (std::size_t token = 0; token < state.tokens; ++token) {
            std::ranges::fill(delta, 0.0F);
            for (std::size_t basis = 0; basis < config.operator_basis_count; ++basis) {
                const auto coefficient = static_cast<float>(std::clamp(
                    coefficients.values()[batch * config.operator_basis_count + basis], -1.0, 1.0));
                for (std::size_t rank = 0; rank < config.operator_rank; ++rank) {
                    float value = 0.0F;
                    for (std::size_t dim = 0; dim < config.model_dim; ++dim)
                        value += state.at(batch, token, dim) *
                            weights.operator_right[(basis * config.operator_rank + rank) * config.model_dim + dim];
                    projected[rank] = value;
                }
                for (std::size_t dim = 0; dim < config.model_dim; ++dim)
                    for (std::size_t rank = 0; rank < config.operator_rank; ++rank)
                        delta[dim] += projected[rank] *
                            weights.operator_left[(basis * config.model_dim + dim) * config.operator_rank + rank] *
                            coefficient;
            }
            float squared = 0.0F;
            const auto divisor = std::sqrt(static_cast<float>(config.operator_rank));
            for (auto& value : delta) { value /= divisor; squared += value * value; }
            const auto norm = std::max(std::sqrt(squared), 1.0e-6F);
            const auto limit = std::min(1.0F, static_cast<float>(config.maximum_operator_update) / norm);
            for (std::size_t dim = 0; dim < config.model_dim; ++dim)
                state.at(batch, token, dim) += delta[dim] * limit;
        }
}

Tensor as_tensor(const Dense& dense) {
    std::vector<double> values;
    values.reserve(dense.values.size());
    for (const auto value : dense.values) values.push_back(value);
    return Tensor(TensorDType::float32, {dense.batch, dense.tokens, dense.hidden},
        std::move(values), "cpu");
}

float sigmoid(const float value) { return 1.0F / (1.0F + std::exp(-value)); }

}  // namespace

void MosaicTransformerBlockWeights::validate(const MosaicTextConfig& config) const {
    const auto d = config.model_dim, f = config.ffn_dim;
    require_size(attention_norm_weight, d, "attention norm weight");
    require_size(attention_norm_bias, d, "attention norm bias");
    require_size(attention_in_projection_weight, 3 * d * d, "attention input projection weight");
    require_size(attention_in_projection_bias, 3 * d, "attention input projection bias");
    require_size(attention_out_projection_weight, d * d, "attention output projection weight");
    require_size(attention_out_projection_bias, d, "attention output projection bias");
    require_size(feedforward_norm_weight, d, "feedforward norm weight");
    require_size(feedforward_norm_bias, d, "feedforward norm bias");
    require_size(feedforward_in_weight, f * d, "feedforward input weight");
    require_size(feedforward_in_bias, f, "feedforward input bias");
    require_size(feedforward_out_weight, d * f, "feedforward output weight");
    require_size(feedforward_out_bias, d, "feedforward output bias");
}

void MosaicTextWeights::validate(const MosaicTextConfig& config) const {
    config.validate();
    const auto p = config.patch_size, e = config.byte_embedding_dim, d = config.model_dim;
    require_size(byte_embedding, mosaic_vocab_size * e, "byte embedding");
    require_size(patch_projection_weight, d * p * e, "patch projection weight");
    require_size(patch_projection_bias, d, "patch projection bias");
    require_size(patch_norm_weight, d, "patch norm weight"); require_size(patch_norm_bias, d, "patch norm bias");
    require_size(segment_embedding, 3 * d, "segment embedding");
    require_size(workspace, config.workspace_slots * d, "workspace"); require_size(bos_patch, d, "BOS patch");
    require_size(retriever_projection_weight, d * config.retriever_dim, "retriever projection weight");
    require_size(retriever_projection_bias, d, "retriever projection bias");
    require_size(round_embedding, config.maximum_recurrent_depth * d, "round embedding");
    if (blocks.size() != config.physical_layers) throw std::invalid_argument("transformer block count mismatch");
    for (const auto& block : blocks) block.validate(config);
    require_size(operator_left, config.operator_basis_count * d * config.operator_rank, "operator left");
    require_size(operator_right, config.operator_basis_count * config.operator_rank * d, "operator right");
    require_size(decoder_weight_ih, 3 * d * e, "decoder input weight");
    require_size(decoder_weight_hh, 3 * d * d, "decoder hidden weight");
    require_size(decoder_bias_ih, 3 * d, "decoder input bias"); require_size(decoder_bias_hh, 3 * d, "decoder hidden bias");
    require_size(output_norm_weight, d, "output norm weight"); require_size(output_norm_bias, d, "output norm bias");
    require_size(lm_head_weight, mosaic_vocab_size * d, "LM head weight");
    require_size(lm_head_bias, mosaic_vocab_size, "LM head bias");
}

MosaicTextLM::MosaicTextLM(MosaicTextConfig config, MosaicTextWeights weights)
    : config_(std::move(config)), weights_(std::move(weights)) { weights_.validate(config_); }

MosaicEncodedTextSource MosaicTextLM::encode_unified_source(
    const MosaicTokenBatch& input_ids) const {
    validate_tokens(input_ids, "world_input_ids", true);
    MosaicTokenBatch body(input_ids.size());
    for (std::size_t batch = 0; batch < input_ids.size(); ++batch)
        body[batch] = {input_ids[batch].begin() + 1, input_ids[batch].end()};
    const auto patches = pad_tokens(body, config_.patch_size, mosaic_pad_id);
    const auto encoded = encode_patches(patches, config_, weights_);

    const auto batch_count = input_ids.size();
    const auto patch_count = patches.patches;
    const auto dimension = config_.model_dim;
    std::vector<double> states(batch_count * (patch_count + 1) * dimension);
    std::vector<std::uint8_t> mask(batch_count * patch_count);
    for (std::size_t batch = 0; batch < batch_count; ++batch) {
        for (std::size_t dim = 0; dim < dimension; ++dim)
            states[(batch * (patch_count + 1)) * dimension + dim] =
                weights_.bos_patch[dim];
        for (std::size_t patch = 0; patch < patch_count; ++patch) {
            bool active = false;
            for (std::size_t offset = 0; offset < config_.patch_size; ++offset)
                active = active || patches.at(batch, patch, offset) != mosaic_pad_id;
            mask[batch * patch_count + patch] = static_cast<std::uint8_t>(active);
            for (std::size_t dim = 0; dim < dimension; ++dim)
                states[((batch * (patch_count + 1) + patch + 1) * dimension) + dim] =
                    encoded.at(batch, patch, dim);
        }
    }
    return {
        Tensor(TensorDType::float32, {batch_count, patch_count + 1, dimension},
               std::move(states), "cpu"),
        BooleanMask({batch_count, patch_count}, std::move(mask)),
    };
}

MosaicTextOutput MosaicTextLM::forward(
    const MosaicTokenBatch& input_ids, const MosaicTokenBatch* targets,
    const std::optional<std::size_t> rounds, const MosaicTokenBatch* memory_ids,
    const Tensor* memory_summary, const Tensor* operator_coefficients) const {
    validate_tokens(input_ids, "input_ids", true);
    const auto recurrent_rounds = rounds.value_or(1);
    if (!recurrent_rounds || recurrent_rounds > config_.maximum_recurrent_depth)
        throw std::invalid_argument("rounds exceed configured recurrent depth");
    const auto batch_count = input_ids.size();
    MosaicTokenBatch raw_targets(batch_count);
    for (std::size_t batch = 0; batch < batch_count; ++batch)
        raw_targets[batch] = {input_ids[batch].begin() + 1, input_ids[batch].end()};
    const auto target_patches = pad_tokens(raw_targets, config_.patch_size, mosaic_pad_id);
    const auto encoded_targets = encode_patches(target_patches, config_, weights_);
    const auto patch_count = target_patches.patches;
    Dense predictor{batch_count, patch_count, config_.model_dim,
        std::vector<float>(product({batch_count, patch_count, config_.model_dim}))};
    for (std::size_t batch = 0; batch < batch_count; ++batch)
        for (std::size_t patch = 0; patch < patch_count; ++patch)
            for (std::size_t dim = 0; dim < config_.model_dim; ++dim)
                predictor.at(batch, patch, dim) = patch == 0 ? weights_.bos_patch[dim]
                    : encoded_targets.at(batch, patch - 1, dim);

    std::optional<PatchBatch> memory_patches;
    std::optional<Dense> encoded_memory;
    if (memory_ids) {
        validate_tokens(*memory_ids, "memory_ids");
        if (memory_ids->size() != batch_count) throw std::invalid_argument("memory_ids batch must match input_ids");
        memory_patches = pad_tokens(*memory_ids, config_.patch_size, mosaic_pad_id);
        encoded_memory = encode_patches(*memory_patches, config_, weights_);
    }
    std::size_t summary_items = 0;
    if (memory_summary) {
        const auto shape = memory_summary->shape();
        if (shape.size() == 2) {
            if (shape[0] != batch_count || shape[1] != config_.retriever_dim)
                throw std::invalid_argument("memory_summary must be [batch, items, retriever_dim]");
            summary_items = 1;
        } else if (shape.size() == 3) {
            if (shape[0] != batch_count || shape[2] != config_.retriever_dim)
                throw std::invalid_argument("memory_summary must be [batch, items, retriever_dim]");
            summary_items = shape[1];
        } else throw std::invalid_argument("memory_summary must be [batch, items, retriever_dim]");
    }
    const auto memory_count = encoded_memory ? encoded_memory->tokens : 0;
    const auto total_tokens = memory_count + summary_items + config_.workspace_slots + patch_count;
    const auto text_start = total_tokens - patch_count;
    Dense state{batch_count, total_tokens, config_.model_dim,
        std::vector<float>(product({batch_count, total_tokens, config_.model_dim}))};
    std::vector<std::uint8_t> valid(batch_count * total_tokens, 0);
    std::vector<std::uint8_t> segments(batch_count * total_tokens, 0);
    for (std::size_t batch = 0; batch < batch_count; ++batch) {
        std::size_t cursor = 0;
        if (encoded_memory) {
            for (std::size_t token = 0; token < memory_count; ++token) {
                for (std::size_t dim = 0; dim < config_.model_dim; ++dim)
                    state.at(batch, cursor + token, dim) = encoded_memory->at(batch, token, dim);
                bool active = false;
                for (std::size_t offset = 0; offset < config_.patch_size; ++offset)
                    active = active || memory_patches->at(batch, token, offset) != mosaic_pad_id;
                valid[batch * total_tokens + cursor + token] = active;
            }
            cursor += memory_count;
        }
        if (memory_summary) {
            for (std::size_t item = 0; item < summary_items; ++item) {
                for (std::size_t out = 0; out < config_.model_dim; ++out) {
                    float value = weights_.retriever_projection_bias[out];
                    for (std::size_t in = 0; in < config_.retriever_dim; ++in) {
                        const auto source = (batch * summary_items + item) * config_.retriever_dim + in;
                        value += static_cast<float>(memory_summary->values()[source]) *
                            weights_.retriever_projection_weight[out * config_.retriever_dim + in];
                    }
                    state.at(batch, cursor + item, out) = value;
                }
                valid[batch * total_tokens + cursor + item] = 1;
            }
            cursor += summary_items;
        }
        for (std::size_t token = 0; token < config_.workspace_slots; ++token) {
            for (std::size_t dim = 0; dim < config_.model_dim; ++dim)
                state.at(batch, cursor + token, dim) = weights_.workspace[token * config_.model_dim + dim];
            valid[batch * total_tokens + cursor + token] = 1;
            segments[batch * total_tokens + cursor + token] = 1;
        }
        cursor += config_.workspace_slots;
        for (std::size_t patch = 0; patch < patch_count; ++patch) {
            for (std::size_t dim = 0; dim < config_.model_dim; ++dim)
                state.at(batch, cursor + patch, dim) = predictor.at(batch, patch, dim);
            bool active = false;
            for (std::size_t offset = 0; offset < config_.patch_size; ++offset)
                active = active || target_patches.at(batch, patch, offset) != mosaic_pad_id;
            valid[batch * total_tokens + cursor + patch] = active;
            segments[batch * total_tokens + cursor + patch] = 2;
        }
    }
    for (std::size_t batch = 0; batch < batch_count; ++batch)
        for (std::size_t token = 0; token < total_tokens; ++token)
            for (std::size_t dim = 0; dim < config_.model_dim; ++dim) {
                const auto segment = segments[batch * total_tokens + token];
                const auto exponent = static_cast<float>((dim / 2) * 2) / static_cast<float>(config_.model_dim);
                const auto angle = static_cast<float>(token) * std::exp(-std::log(10'000.0F) * exponent);
                state.at(batch, token, dim) += weights_.segment_embedding[segment * config_.model_dim + dim]
                    + (dim % 2 ? std::cos(angle) : std::sin(angle));
            }
    for (std::size_t round = 0; round < recurrent_rounds; ++round) {
        for (std::size_t batch = 0; batch < batch_count; ++batch)
            for (std::size_t token = 0; token < total_tokens; ++token)
                for (std::size_t dim = 0; dim < config_.model_dim; ++dim)
                    state.at(batch, token, dim) += weights_.round_embedding[round * config_.model_dim + dim];
        for (const auto& block : weights_.blocks) transformer_block(state, valid, config_, block);
        if (operator_coefficients) operator_adapter(state, *operator_coefficients, config_, weights_);
    }
    Dense contexts{batch_count, patch_count, config_.model_dim,
        std::vector<float>(product({batch_count, patch_count, config_.model_dim}))};
    for (std::size_t batch = 0; batch < batch_count; ++batch)
        for (std::size_t patch = 0; patch < patch_count; ++patch)
            for (std::size_t dim = 0; dim < config_.model_dim; ++dim)
                contexts.at(batch, patch, dim) = state.at(batch, text_start + patch, dim);

    const auto decoder_rows = batch_count * patch_count;
    std::vector<float> hidden = contexts.values;
    std::vector<float> decoder_states(product({decoder_rows, config_.patch_size, config_.model_dim}));
    std::vector<float> logits(product({decoder_rows, config_.patch_size, static_cast<std::size_t>(mosaic_vocab_size)}));
    std::vector<std::int64_t> previous(decoder_rows, mosaic_bos_id);
    std::vector<float> embedded(product({decoder_rows, config_.byte_embedding_dim}));
    std::vector<float> input_gates(product({decoder_rows, 3, config_.model_dim}));
    std::vector<float> hidden_gates(input_gates.size());
    std::vector<float> normalized(product({decoder_rows, config_.model_dim}));
    for (std::size_t offset = 0; offset < config_.patch_size; ++offset) {
        for (std::size_t row = 0; row < decoder_rows; ++row)
            std::copy_n(weights_.byte_embedding.begin() + previous[row] * config_.byte_embedding_dim,
                config_.byte_embedding_dim, embedded.begin() + row * config_.byte_embedding_dim);
        linear(embedded, weights_.decoder_weight_ih, weights_.decoder_bias_ih,
            decoder_rows, config_.byte_embedding_dim, 3 * config_.model_dim, input_gates);
        linear(hidden, weights_.decoder_weight_hh, weights_.decoder_bias_hh,
            decoder_rows, config_.model_dim, 3 * config_.model_dim, hidden_gates);
        for (std::size_t row = 0; row < decoder_rows; ++row)
            for (std::size_t dim = 0; dim < config_.model_dim; ++dim) {
                const auto reset = sigmoid(input_gates[row * 3 * config_.model_dim + dim] + hidden_gates[row * 3 * config_.model_dim + dim]);
                const auto update = sigmoid(input_gates[row * 3 * config_.model_dim + config_.model_dim + dim] + hidden_gates[row * 3 * config_.model_dim + config_.model_dim + dim]);
                const auto candidate = std::tanh(input_gates[row * 3 * config_.model_dim + 2 * config_.model_dim + dim] + reset * hidden_gates[row * 3 * config_.model_dim + 2 * config_.model_dim + dim]);
                hidden[row * config_.model_dim + dim] = (1.0F - update) * candidate + update * hidden[row * config_.model_dim + dim];
                decoder_states[(row * config_.patch_size + offset) * config_.model_dim + dim] = hidden[row * config_.model_dim + dim];
            }
        layer_norm(hidden, weights_.output_norm_weight, weights_.output_norm_bias,
            decoder_rows, config_.model_dim, normalized);
        std::vector<float> step_logits(product({decoder_rows, static_cast<std::size_t>(mosaic_vocab_size)}));
        linear(normalized, weights_.lm_head_weight, weights_.lm_head_bias,
            decoder_rows, config_.model_dim, mosaic_vocab_size, step_logits);
        for (std::size_t row = 0; row < decoder_rows; ++row) {
            std::copy_n(step_logits.begin() + row * mosaic_vocab_size, mosaic_vocab_size,
                logits.begin() + (row * config_.patch_size + offset) * mosaic_vocab_size);
            const auto batch = row / patch_count, patch = row % patch_count;
            previous[row] = target_patches.at(batch, patch, offset);
        }
    }
    MosaicTokenBatch raw_labels;
    if (!targets) raw_labels = raw_targets;
    else {
        if (targets->size() != batch_count || targets->empty())
            throw std::invalid_argument("targets must align with input_ids or shifted targets");
        raw_labels = *targets;
        if (targets->front().size() == input_ids.front().size())
            for (auto& row : raw_labels) row.erase(row.begin());
    }
    const auto expected = patch_count * config_.patch_size;
    std::vector<std::int64_t> labels(batch_count * expected, mosaic_ignore_index);
    for (std::size_t batch = 0; batch < batch_count; ++batch) {
        if (raw_labels[batch].size() > expected)
            throw std::invalid_argument("targets are longer than the input token sequence");
        for (std::size_t index = 0; index < raw_labels[batch].size(); ++index)
            labels[batch * expected + index] = raw_labels[batch][index] == mosaic_pad_id
                ? mosaic_ignore_index : raw_labels[batch][index];
    }
    std::vector<std::uint8_t> target_mask(labels.size());
    double loss_sum = 0.0; std::size_t loss_count = 0;
    for (std::size_t index = 0; index < labels.size(); ++index) {
        if (labels[index] == mosaic_ignore_index) continue;
        if (labels[index] < 0 || labels[index] >= mosaic_vocab_size)
            throw std::invalid_argument("target id is outside the byte vocabulary");
        target_mask[index] = 1;
        const auto begin = index * mosaic_vocab_size;
        const auto maximum = *std::max_element(logits.begin() + begin, logits.begin() + begin + mosaic_vocab_size);
        double denominator = 0.0;
        for (std::size_t token = 0; token < mosaic_vocab_size; ++token)
            denominator += std::exp(static_cast<double>(logits[begin + token] - maximum));
        loss_sum += std::log(denominator) + maximum - logits[begin + labels[index]];
        ++loss_count;
    }
    std::vector<double> logit_values(logits.begin(), logits.end());
    std::vector<double> decoder_values(decoder_states.begin(), decoder_states.end());
    return {Tensor(TensorDType::float32, {batch_count, patch_count, config_.patch_size,
                static_cast<std::uint64_t>(mosaic_vocab_size)}, std::move(logit_values), "cpu"),
        loss_count ? std::optional<double>(loss_sum / loss_count) : std::nullopt,
        recurrent_rounds, std::move(target_mask),
        Tensor(TensorDType::float32, {batch_count, patch_count, config_.patch_size, config_.model_dim},
            std::move(decoder_values), "cpu"), as_tensor(contexts)};
}

MosaicTokenBatch MosaicTextLM::generate(
    const MosaicTokenBatch& input_ids, const std::size_t max_new_bytes,
    const std::optional<std::size_t> rounds, const MosaicTokenBatch* memory_ids,
    const Tensor* memory_summary, const Tensor* operator_coefficients) const {
    validate_tokens(input_ids, "input_ids", true);
    auto generated = input_ids;
    std::vector<std::uint8_t> finished(generated.size(), 0);
    for (std::size_t step = 0; step < max_new_bytes; ++step) {
        auto candidate = generated;
        for (auto& row : candidate) row.push_back(mosaic_pad_id);
        MosaicTokenBatch ignored(candidate.size(),
            std::vector<std::int64_t>(candidate.front().size(), mosaic_ignore_index));
        const auto output = forward(candidate, &ignored, rounds, memory_ids,
            memory_summary, operator_coefficients);
        const auto position = generated.front().size() - 1;
        const auto patch = position / config_.patch_size, offset = position % config_.patch_size;
        for (std::size_t batch = 0; batch < generated.size(); ++batch) {
            std::int64_t next = mosaic_eos_id;
            if (!finished[batch]) {
                double best = -std::numeric_limits<double>::infinity();
                const auto base = ((batch * output.logits.shape()[1] + patch) * config_.patch_size + offset) * mosaic_vocab_size;
                for (std::int64_t token = 0; token < mosaic_vocab_size; ++token) {
                    if (token == mosaic_pad_id || token == mosaic_bos_id) continue;
                    if (output.logits.values()[base + token] > best) {
                        best = output.logits.values()[base + token]; next = token;
                    }
                }
            }
            generated[batch].push_back(next);
            finished[batch] = finished[batch] || next == mosaic_eos_id;
        }
        if (std::ranges::all_of(finished, [](const auto value) { return value != 0; })) break;
    }
    return generated;
}

}  // namespace swegca::world
