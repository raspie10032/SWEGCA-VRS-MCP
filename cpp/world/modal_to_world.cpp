#include "world/modal_to_world.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <span>
#include <stdexcept>
#include <utility>

namespace swegca::world {
namespace {

constexpr float layer_norm_epsilon = 1.0e-5F;

[[nodiscard]] std::size_t checked_size(const std::uint64_t value) {
    if (value > std::numeric_limits<std::size_t>::max()) {
        throw std::length_error("modal-to-World dimension exceeds addressable memory");
    }
    return static_cast<std::size_t>(value);
}

[[nodiscard]] std::size_t checked_product(const std::initializer_list<std::uint64_t> values) {
    if (std::find(values.begin(), values.end(), 0) != values.end()) return 0;
    std::uint64_t product = 1;
    for (const auto value : values) {
        if (value != 0 && product > std::numeric_limits<std::uint64_t>::max() / value) {
            throw std::overflow_error("modal-to-World tensor shape overflow");
        }
        product *= value;
    }
    return checked_size(product);
}

void require_shape(const checkpoint::MaterializedTensor& tensor,
                   const std::initializer_list<std::uint64_t> expected,
                   const char* const label) {
    if (tensor.dtype() != checkpoint::TensorDType::float32 ||
        tensor.shape().size() != expected.size() ||
        !std::equal(tensor.shape().begin(), tensor.shape().end(), expected.begin())) {
        throw std::invalid_argument(std::string(label) +
                                    " must be a float32 tensor with the pinned shape");
    }
}

[[nodiscard]] std::vector<float> layer_norm(
    const std::span<const float> input, const std::size_t rows,
    const std::size_t width, const std::span<const float> weight,
    const std::span<const float> bias) {
    std::vector<float> output(input.size());
    for (std::size_t row = 0; row != rows; ++row) {
        const auto offset = row * width;
        float mean = 0.0F;
        for (std::size_t column = 0; column != width; ++column) {
            mean += input[offset + column];
        }
        mean /= static_cast<float>(width);
        float variance = 0.0F;
        for (std::size_t column = 0; column != width; ++column) {
            const float difference = input[offset + column] - mean;
            variance += difference * difference;
        }
        variance /= static_cast<float>(width);
        const float inverse = 1.0F / std::sqrt(variance + layer_norm_epsilon);
        for (std::size_t column = 0; column != width; ++column) {
            output[offset + column] =
                (input[offset + column] - mean) * inverse * weight[column] + bias[column];
        }
    }
    return output;
}

[[nodiscard]] std::vector<float> linear(
    const std::span<const float> input, const std::size_t rows,
    const std::size_t input_width, const std::size_t output_width,
    const std::span<const float> weight, const std::span<const float> bias,
    const std::size_t weight_row_offset = 0) {
    std::vector<float> output(rows * output_width);
    for (std::size_t row = 0; row != rows; ++row) {
        for (std::size_t out = 0; out != output_width; ++out) {
            float value = bias[weight_row_offset + out];
            const auto weight_offset = (weight_row_offset + out) * input_width;
            for (std::size_t in = 0; in != input_width; ++in) {
                value += input[row * input_width + in] * weight[weight_offset + in];
            }
            output[row * output_width + out] = value;
        }
    }
    return output;
}

}  // namespace

void ModalToWorldConfig::validate() const {
    world.validate();
    if (source_dim == 0 || attention_heads == 0) {
        throw std::invalid_argument(
            "source_dim and attention_heads must be positive");
    }
    if (world.world_dim % attention_heads != 0) {
        throw std::invalid_argument(
            "world_dim must be divisible by attention_heads");
    }
}

ModalToWorldWeights::ModalToWorldWeights(
    checkpoint::MaterializedTensor world_queries,
    checkpoint::MaterializedTensor source_norm_weight,
    checkpoint::MaterializedTensor source_norm_bias,
    checkpoint::MaterializedTensor source_linear_weight,
    checkpoint::MaterializedTensor source_linear_bias,
    checkpoint::MaterializedTensor attention_in_weight,
    checkpoint::MaterializedTensor attention_in_bias,
    checkpoint::MaterializedTensor attention_out_weight,
    checkpoint::MaterializedTensor attention_out_bias,
    checkpoint::MaterializedTensor output_norm_weight,
    checkpoint::MaterializedTensor output_norm_bias,
    const ModalToWorldConfig& config)
    : config_(config) {
    config.validate();
    const auto source_dim = config.source_dim;
    const auto world_dim = config.world.world_dim;
    const auto world_slots = config.world.world_slots;
    if (world_dim > std::numeric_limits<std::uint64_t>::max() / 3U) {
        throw std::overflow_error("packed attention dimension overflow");
    }
    require_shape(world_queries, {world_slots, world_dim}, "world queries");
    require_shape(source_norm_weight, {source_dim}, "source norm weight");
    require_shape(source_norm_bias, {source_dim}, "source norm bias");
    require_shape(source_linear_weight, {world_dim, source_dim},
                  "source linear weight");
    require_shape(source_linear_bias, {world_dim}, "source linear bias");
    require_shape(attention_in_weight, {3U * world_dim, world_dim},
                  "attention input weight");
    require_shape(attention_in_bias, {3U * world_dim}, "attention input bias");
    require_shape(attention_out_weight, {world_dim, world_dim},
                  "attention output weight");
    require_shape(attention_out_bias, {world_dim}, "attention output bias");
    require_shape(output_norm_weight, {world_dim}, "output norm weight");
    require_shape(output_norm_bias, {world_dim}, "output norm bias");

    world_queries_ = world_queries.float32_values();
    source_norm_weight_ = source_norm_weight.float32_values();
    source_norm_bias_ = source_norm_bias.float32_values();
    source_linear_weight_ = source_linear_weight.float32_values();
    source_linear_bias_ = source_linear_bias.float32_values();
    attention_in_weight_ = attention_in_weight.float32_values();
    attention_in_bias_ = attention_in_bias.float32_values();
    attention_out_weight_ = attention_out_weight.float32_values();
    attention_out_bias_ = attention_out_bias.float32_values();
    output_norm_weight_ = output_norm_weight.float32_values();
    output_norm_bias_ = output_norm_bias.float32_values();
}

ModalToWorldWeights ModalToWorldWeights::load(
    const checkpoint::RestrictedCheckpoint& checkpoint,
    const ModalToWorldConfig& config) {
    if (config.source_dim != 256 || config.world.world_slots != 32 ||
        config.world.world_dim != 256 || config.world.object_slots != 8 ||
        config.attention_heads != 8) {
        throw std::invalid_argument(
            "modal-to-World config does not match the audited checkpoint profile");
    }
    return ModalToWorldWeights(
        checkpoint::materialize_model_tensor(checkpoint, "to_world.world_queries"),
        checkpoint::materialize_model_tensor(
            checkpoint, "to_world.source_projection.0.weight"),
        checkpoint::materialize_model_tensor(
            checkpoint, "to_world.source_projection.0.bias"),
        checkpoint::materialize_model_tensor(
            checkpoint, "to_world.source_projection.1.weight"),
        checkpoint::materialize_model_tensor(
            checkpoint, "to_world.source_projection.1.bias"),
        checkpoint::materialize_model_tensor(
            checkpoint, "to_world.cross_attention.in_proj_weight"),
        checkpoint::materialize_model_tensor(
            checkpoint, "to_world.cross_attention.in_proj_bias"),
        checkpoint::materialize_model_tensor(
            checkpoint, "to_world.cross_attention.out_proj.weight"),
        checkpoint::materialize_model_tensor(
            checkpoint, "to_world.cross_attention.out_proj.bias"),
        checkpoint::materialize_model_tensor(checkpoint, "to_world.output_norm.weight"),
        checkpoint::materialize_model_tensor(checkpoint, "to_world.output_norm.bias"),
        config);
}

ModalToWorldAdapter::ModalToWorldAdapter(ModalToWorldConfig config,
                                         ModalToWorldWeights weights)
    : config_(std::move(config)), weights_(std::move(weights)) {
    config_.validate();
    const auto& bound = weights_.config_;
    if (config_.source_dim != bound.source_dim ||
        config_.world.world_slots != bound.world.world_slots ||
        config_.world.world_dim != bound.world.world_dim ||
        config_.world.object_slots != bound.world.object_slots ||
        config_.attention_heads != bound.attention_heads) {
        throw std::invalid_argument(
            "modal-to-World weights and adapter config differ");
    }
}

WorldState ModalToWorldAdapter::forward(const Tensor& source_states,
                                        const BooleanMask* const source_mask,
                                        std::string source) const {
    if (source_states.dtype() != TensorDType::float32 ||
        source_states.device() != "cpu") {
        throw std::invalid_argument(
            "source_states must be a CPU float32 tensor");
    }
    const auto shape = source_states.shape();
    if (shape.size() != 3) {
        throw std::invalid_argument(
            "source_states must have shape [batch, tokens, dim]");
    }
    if (shape[2] != config_.source_dim) {
        throw std::invalid_argument("source_states final dimension changed");
    }
    const auto batch = shape[0];
    const auto source_tokens = shape[1];
    if (source_tokens == 0) {
        throw std::runtime_error(
            "source token dimension must be nonzero for attention");
    }
    if (source_mask != nullptr) {
        const auto mask_shape = source_mask->shape();
        if (mask_shape.size() != 2 || mask_shape[0] != batch ||
            mask_shape[1] != source_tokens) {
            throw std::invalid_argument(
                "source_mask must have shape [batch, tokens]");
        }
    }

    const auto batch_size = checked_size(batch);
    const auto token_count = checked_size(source_tokens);
    const auto source_dim = checked_size(config_.source_dim);
    const auto world_slots = checked_size(config_.world.world_slots);
    const auto world_dim = checked_size(config_.world.world_dim);
    const auto heads = checked_size(config_.attention_heads);
    const auto head_dim = world_dim / heads;
    (void)checked_product({batch, source_tokens, config_.source_dim});
    (void)checked_product({batch, source_tokens, config_.world.world_dim});
    (void)checked_product({batch, config_.world.world_slots,
                           config_.world.world_dim});

    std::vector<float> source_values;
    source_values.reserve(source_states.values().size());
    for (const auto value : source_states.values()) {
        source_values.push_back(static_cast<float>(value));
    }
    const auto normalized = layer_norm(
        source_values, batch_size * token_count, source_dim,
        weights_.source_norm_weight_, weights_.source_norm_bias_);
    const auto memory = linear(
        normalized, batch_size * token_count, source_dim, world_dim,
        weights_.source_linear_weight_, weights_.source_linear_bias_);

    std::vector<float> queries(batch_size * world_slots * world_dim);
    for (std::size_t batch_index = 0; batch_index != batch_size; ++batch_index) {
        std::copy(weights_.world_queries_.begin(), weights_.world_queries_.end(),
                  queries.begin() + static_cast<std::ptrdiff_t>(
                      batch_index * world_slots * world_dim));
    }
    const auto projected_queries = linear(
        queries, batch_size * world_slots, world_dim, world_dim,
        weights_.attention_in_weight_, weights_.attention_in_bias_, 0);
    const auto projected_keys = linear(
        memory, batch_size * token_count, world_dim, world_dim,
        weights_.attention_in_weight_, weights_.attention_in_bias_, world_dim);
    const auto projected_values = linear(
        memory, batch_size * token_count, world_dim, world_dim,
        weights_.attention_in_weight_, weights_.attention_in_bias_, 2U * world_dim);

    std::vector<float> attended(batch_size * world_slots * world_dim, 0.0F);
    const float scale = 1.0F / std::sqrt(static_cast<float>(head_dim));
    std::vector<float> logits(token_count);
    for (std::size_t batch_index = 0; batch_index != batch_size; ++batch_index) {
        for (std::size_t slot = 0; slot != world_slots; ++slot) {
            for (std::size_t head = 0; head != heads; ++head) {
                float maximum = -std::numeric_limits<float>::infinity();
                bool any_valid = false;
                for (std::size_t token = 0; token != token_count; ++token) {
                    const bool valid = source_mask == nullptr ||
                        source_mask->at(batch_index, token);
                    if (!valid) {
                        logits[token] = -std::numeric_limits<float>::infinity();
                        continue;
                    }
                    float score = 0.0F;
                    for (std::size_t component = 0; component != head_dim; ++component) {
                        const auto feature = head * head_dim + component;
                        const auto query_index =
                            (batch_index * world_slots + slot) * world_dim + feature;
                        const auto key_index =
                            (batch_index * token_count + token) * world_dim + feature;
                        score += projected_queries[query_index] * projected_keys[key_index];
                    }
                    score *= scale;
                    logits[token] = score;
                    maximum = std::max(maximum, score);
                    any_valid = true;
                }
                if (!any_valid) {
                    // PyTorch's need_weights=False SDPA path returns a zero
                    // attention vector for a row whose keys are all padded.
                    continue;
                }
                float denominator = 0.0F;
                for (std::size_t token = 0; token != token_count; ++token) {
                    const bool valid = source_mask == nullptr ||
                        source_mask->at(batch_index, token);
                    if (!valid) continue;
                    logits[token] = std::exp(logits[token] - maximum);
                    denominator += logits[token];
                }
                for (std::size_t component = 0; component != head_dim; ++component) {
                    float value = 0.0F;
                    const auto feature = head * head_dim + component;
                    for (std::size_t token = 0; token != token_count; ++token) {
                        const bool valid = source_mask == nullptr ||
                            source_mask->at(batch_index, token);
                        if (!valid) continue;
                        const auto value_index =
                            (batch_index * token_count + token) * world_dim + feature;
                        value += (logits[token] / denominator) *
                            projected_values[value_index];
                    }
                    attended[(batch_index * world_slots + slot) * world_dim + feature] =
                        value;
                }
            }
        }
    }

    auto slots = linear(
        attended, batch_size * world_slots, world_dim, world_dim,
        weights_.attention_out_weight_, weights_.attention_out_bias_);
    for (std::size_t index = 0; index != slots.size(); ++index) {
        slots[index] += queries[index];
    }
    slots = layer_norm(slots, batch_size * world_slots, world_dim,
                       weights_.output_norm_weight_, weights_.output_norm_bias_);

    std::vector<double> output_values;
    output_values.reserve(slots.size());
    for (const auto value : slots) output_values.push_back(value);
    std::vector<std::uint8_t> active(batch_size * world_slots, 1U);
    std::vector<std::uint8_t> dirty(batch_size * world_slots, 0U);
    WorldState result(
        Tensor(TensorDType::float32,
               {batch, config_.world.world_slots, config_.world.world_dim},
               std::move(output_values)),
        BooleanMask({batch, config_.world.world_slots}, std::move(active)),
        BooleanMask({batch, config_.world.world_slots}, std::move(dirty)),
        std::move(source));
    result.validate(config_.world);
    return result;
}

WorldState ModalToWorldAdapter::forward(const Tensor& source_states,
                                        std::string source) const {
    return forward(source_states, nullptr, std::move(source));
}

}  // namespace swegca::world
