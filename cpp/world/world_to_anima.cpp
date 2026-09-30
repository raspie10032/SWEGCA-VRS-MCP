#include "world/mosaic_omni.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace swegca::world {
namespace {

void require(const std::vector<float>& values, const std::size_t expected, const char* name) {
    if (values.size() != expected) throw std::invalid_argument(std::string(name) + " shape mismatch");
}

void linear(const std::span<const float> input, const std::span<const float> weight,
            const std::span<const float> bias, const std::size_t rows,
            const std::size_t in, const std::size_t out, std::span<float> output,
            const std::size_t weight_row = 0) {
    for (std::size_t row = 0; row < rows; ++row) for (std::size_t destination = 0; destination < out; ++destination) {
        float value = bias.empty() ? 0.0F : bias[weight_row + destination];
        for (std::size_t source = 0; source < in; ++source)
            value += input[row * in + source] * weight[(weight_row + destination) * in + source];
        output[row * out + destination] = value;
    }
}

void norm(const std::span<const float> input, const std::span<const float> weight,
          const std::span<const float> bias, const std::size_t rows,
          const std::size_t width, std::span<float> output) {
    for (std::size_t row = 0; row < rows; ++row) {
        float mean = 0.0F; for (std::size_t column = 0; column < width; ++column) mean += input[row * width + column];
        mean /= width; float variance = 0.0F;
        for (std::size_t column = 0; column < width; ++column) {
            const auto delta = input[row * width + column] - mean; variance += delta * delta;
        }
        const auto inverse = 1.0F / std::sqrt(variance / width + 1.0e-5F);
        for (std::size_t column = 0; column < width; ++column)
            output[row * width + column] = (input[row * width + column] - mean) * inverse * weight[column] + bias[column];
    }
}

}  // namespace

void WorldToAnimaWeights::validate(const MosaicOmniConfig& config) const {
    config.validate(); const auto d = config.world_dim, q = config.anima_conditioning_tokens;
    require(conditioning_queries, q * d, "conditioning queries");
    require(attention_in_weight, 3 * d * d, "attention input weight");
    require(attention_in_bias, 3 * d, "attention input bias");
    require(attention_out_weight, d * d, "attention output weight");
    require(attention_out_bias, d, "attention output bias");
    require(output_norm_weight, d, "output norm weight"); require(output_norm_bias, d, "output norm bias");
    require(output_weight, config.anima_conditioning_dim * d, "conditioning output weight");
}

WorldToAnimaConditioning::WorldToAnimaConditioning(
    MosaicOmniConfig config, WorldToAnimaWeights weights)
    : config_(std::move(config)), weights_(std::move(weights)) { weights_.validate(config_); }

Tensor WorldToAnimaConditioning::forward(const WorldState& world_state) const {
    world_state.validate(WorldConfig{config_.world_slots, config_.world_dim, config_.object_slots});
    const auto shape = world_state.semantic_slots().shape(); const auto batch = static_cast<std::size_t>(shape[0]);
    const auto slots = config_.world_slots, queries = config_.anima_conditioning_tokens;
    const auto dim = config_.world_dim, heads = config_.attention_heads, head_dim = dim / heads;
    std::vector<float> memory(world_state.semantic_slots().values().size());
    std::ranges::transform(world_state.semantic_slots().values(), memory.begin(), [](const double value) { return static_cast<float>(value); });
    std::vector<float> query(batch * queries * dim);
    for (std::size_t b = 0; b < batch; ++b)
        std::copy(weights_.conditioning_queries.begin(), weights_.conditioning_queries.end(), query.begin() + b * queries * dim);
    std::vector<float> projected_query(query.size()), projected_key(memory.size()), projected_value(memory.size());
    linear(query, weights_.attention_in_weight, weights_.attention_in_bias, batch * queries, dim, dim, projected_query, 0);
    linear(memory, weights_.attention_in_weight, weights_.attention_in_bias, batch * slots, dim, dim, projected_key, dim);
    linear(memory, weights_.attention_in_weight, weights_.attention_in_bias, batch * slots, dim, dim, projected_value, 2 * dim);
    std::vector<float> attended(query.size()); std::vector<float> scores(slots);
    const auto scale = 1.0F / std::sqrt(static_cast<float>(head_dim));
    for (std::size_t b = 0; b < batch; ++b) for (std::size_t q = 0; q < queries; ++q)
        for (std::size_t head = 0; head < heads; ++head) {
            float maximum = -std::numeric_limits<float>::infinity(); bool any = false;
            for (std::size_t slot = 0; slot < slots; ++slot) {
                if (!world_state.active_mask().at(b, slot)) { scores[slot] = -std::numeric_limits<float>::infinity(); continue; }
                float score = 0.0F; for (std::size_t inner = 0; inner < head_dim; ++inner) {
                    const auto feature = head * head_dim + inner;
                    score += projected_query[(b * queries + q) * dim + feature] * projected_key[(b * slots + slot) * dim + feature];
                }
                scores[slot] = score * scale; maximum = std::max(maximum, scores[slot]); any = true;
            }
            if (!any) continue; float denominator = 0.0F;
            for (std::size_t slot = 0; slot < slots; ++slot) if (world_state.active_mask().at(b, slot)) {
                scores[slot] = std::exp(scores[slot] - maximum); denominator += scores[slot];
            }
            for (std::size_t inner = 0; inner < head_dim; ++inner) {
                const auto feature = head * head_dim + inner; float value = 0.0F;
                for (std::size_t slot = 0; slot < slots; ++slot) if (world_state.active_mask().at(b, slot))
                    value += scores[slot] / denominator * projected_value[(b * slots + slot) * dim + feature];
                attended[(b * queries + q) * dim + feature] = value;
            }
        }
    std::vector<float> projected(attended.size());
    linear(attended, weights_.attention_out_weight, weights_.attention_out_bias, batch * queries, dim, dim, projected);
    for (std::size_t index = 0; index < projected.size(); ++index) projected[index] += query[index];
    std::vector<float> normalized(projected.size());
    norm(projected, weights_.output_norm_weight, weights_.output_norm_bias, batch * queries, dim, normalized);
    std::vector<float> output(batch * queries * config_.anima_conditioning_dim);
    linear(normalized, weights_.output_weight, {}, batch * queries, dim, config_.anima_conditioning_dim, output);
    std::vector<double> values(output.begin(), output.end());
    return Tensor(TensorDType::float32, {batch, queries, config_.anima_conditioning_dim}, std::move(values), "cpu");
}

}  // namespace swegca::world
