#include "world/mosaic_omni.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace swegca::world {
namespace {

void require_size(const std::vector<float>& values, const std::size_t expected, const char* name) {
    if (values.size() != expected) throw std::invalid_argument(std::string(name) + " shape mismatch");
}

float sigmoid(const float value) { return 1.0F / (1.0F + std::exp(-value)); }

void linear(const std::span<const float> input, const std::span<const float> weight,
            const std::span<const float> bias, const std::size_t rows,
            const std::size_t in, const std::size_t out, std::span<float> output) {
    for (std::size_t row = 0; row < rows; ++row) for (std::size_t destination = 0; destination < out; ++destination) {
        float value = bias.empty() ? 0.0F : bias[destination];
        for (std::size_t source = 0; source < in; ++source)
            value += input[row * in + source] * weight[destination * in + source];
        output[row * out + destination] = value;
    }
}

void norm(const std::span<const float> input, const std::span<const float> weight,
          const std::span<const float> bias, const std::size_t rows,
          const std::size_t width, std::span<float> output) {
    for (std::size_t row = 0; row < rows; ++row) {
        float mean = 0.0F; for (std::size_t d = 0; d < width; ++d) mean += input[row * width + d];
        mean /= width; float variance = 0.0F; for (std::size_t d = 0; d < width; ++d) {
            const auto difference = input[row * width + d] - mean; variance += difference * difference;
        }
        const auto inverse = 1.0F / std::sqrt(variance / width + 1.0e-5F);
        for (std::size_t d = 0; d < width; ++d)
            output[row * width + d] = (input[row * width + d] - mean) * inverse * weight[d] + bias[d];
    }
}

}  // namespace

void LongVideoWorldWeights::validate(
    const MosaicOmniConfig& config, const bool transition_features) const {
    config.validate(); const auto d = config.world_dim;
    require_size(position_weight, d * 2, "long-video position weight");
    if (transition_features) {
        require_size(transition_weight, d * d * 2, "long-video transition weight");
        require_size(transition_bias, d, "long-video transition bias");
    } else if (!transition_weight.empty() || !transition_bias.empty())
        throw std::invalid_argument("disabled transition weights must be empty");
    require_size(gru_weight_ih, 3 * d * d, "long-video GRU input weight");
    require_size(gru_weight_hh, 3 * d * d, "long-video GRU hidden weight");
    require_size(gru_bias_ih, 3 * d, "long-video GRU input bias");
    require_size(gru_bias_hh, 3 * d, "long-video GRU hidden bias");
    require_size(norm_weight, d, "long-video norm weight"); require_size(norm_bias, d, "long-video norm bias");
    require_size(order_norm_weight, d, "order norm weight"); require_size(order_norm_bias, d, "order norm bias");
    require_size(order_weight, 2 * d, "order weight"); require_size(order_bias, 2, "order bias");
}

LongVideoWorldAccumulator::LongVideoWorldAccumulator(
    MosaicOmniConfig config, const bool transition_features, LongVideoWorldWeights weights)
    : config_(std::move(config)), transition_features_(transition_features),
      weights_(std::move(weights)) { weights_.validate(config_, transition_features_); }

LongVideoWorldOutput LongVideoWorldAccumulator::forward(
    const Tensor& clips, const BooleanMask& clip_mask, const WorldState* initial_state) const {
    const auto shape = clips.shape();
    if (shape.size() != 4) throw std::invalid_argument("clip_world_states must be [batch, clips, slots, dim]");
    const auto batch = static_cast<std::size_t>(shape[0]), clip_count = static_cast<std::size_t>(shape[1]);
    const auto slots = static_cast<std::size_t>(shape[2]), dim = static_cast<std::size_t>(shape[3]);
    if (slots != config_.world_slots || dim != config_.world_dim)
        throw std::invalid_argument("clip World State shape does not match the core");
    if (clip_mask.shape().size() != 2 || clip_mask.shape()[0] != batch || clip_mask.shape()[1] != clip_count)
        throw std::invalid_argument("clip_mask must be boolean [batch, clips]");
    std::vector<std::size_t> totals(batch);
    for (std::size_t b = 0; b < batch; ++b) for (std::size_t c = 0; c < clip_count; ++c) totals[b] += clip_mask.at(b, c);
    if (std::ranges::find(totals, 0) != totals.end())
        throw std::invalid_argument("every sequence needs at least one observed clip");
    std::vector<float> hidden(batch * slots * dim, 0.0F), previous(hidden.size(), 0.0F);
    std::vector<std::uint8_t> active(batch * slots, 0);
    if (initial_state) {
        initial_state->validate(WorldConfig{config_.world_slots, config_.world_dim, config_.object_slots});
        if (initial_state->semantic_slots().shape()[0] != batch)
            throw std::invalid_argument("initial World State batch does not match");
        std::ranges::transform(initial_state->semantic_slots().values(), hidden.begin(),
            [](const double value) { return static_cast<float>(value); });
        std::copy(initial_state->active_mask().values().begin(), initial_state->active_mask().values().end(), active.begin());
    }
    std::vector<std::size_t> observed(batch);
    std::vector<float> checkpoints(batch * clip_count * slots * dim);
    const auto rows = batch * slots;
    for (std::size_t clip = 0; clip < clip_count; ++clip) {
        std::vector<float> input(rows * dim), position(batch * dim);
        for (std::size_t b = 0; b < batch; ++b) {
            const auto denominator = static_cast<float>(std::max<std::size_t>(totals[b] - 1, 1));
            const float coordinates[2]{static_cast<float>(observed[b]) / denominator,
                static_cast<float>(clip_mask.at(b, clip)) / denominator};
            for (std::size_t d = 0; d < dim; ++d)
                position[b * dim + d] = coordinates[0] * weights_.position_weight[d * 2] +
                    coordinates[1] * weights_.position_weight[d * 2 + 1];
            for (std::size_t slot = 0; slot < slots; ++slot) for (std::size_t d = 0; d < dim; ++d) {
                const auto source = ((b * clip_count + clip) * slots + slot) * dim + d;
                input[(b * slots + slot) * dim + d] = static_cast<float>(clips.values()[source]);
            }
        }
        if (transition_features_) {
            std::vector<float> joined(rows * dim * 2), projected(rows * dim);
            for (std::size_t row = 0; row < rows; ++row) for (std::size_t d = 0; d < dim; ++d) {
                joined[row * dim * 2 + d] = input[row * dim + d];
                joined[row * dim * 2 + dim + d] = input[row * dim + d] - previous[row * dim + d];
            }
            linear(joined, weights_.transition_weight, weights_.transition_bias, rows, dim * 2, dim, projected);
            input = std::move(projected);
        }
        for (std::size_t b = 0; b < batch; ++b) for (std::size_t slot = 0; slot < slots; ++slot)
            for (std::size_t d = 0; d < dim; ++d) input[(b * slots + slot) * dim + d] += position[b * dim + d];
        std::vector<float> input_gates(rows * 3 * dim), hidden_gates(input_gates.size());
        linear(input, weights_.gru_weight_ih, weights_.gru_bias_ih, rows, dim, 3 * dim, input_gates);
        linear(hidden, weights_.gru_weight_hh, weights_.gru_bias_hh, rows, dim, 3 * dim, hidden_gates);
        auto candidate = hidden;
        for (std::size_t row = 0; row < rows; ++row) for (std::size_t d = 0; d < dim; ++d) {
            const auto reset = sigmoid(input_gates[row * 3 * dim + d] + hidden_gates[row * 3 * dim + d]);
            const auto update = sigmoid(input_gates[row * 3 * dim + dim + d] + hidden_gates[row * 3 * dim + dim + d]);
            const auto next = std::tanh(input_gates[row * 3 * dim + 2 * dim + d] + reset * hidden_gates[row * 3 * dim + 2 * dim + d]);
            candidate[row * dim + d] = (1.0F - update) * next + update * hidden[row * dim + d];
        }
        for (std::size_t b = 0; b < batch; ++b) if (clip_mask.at(b, clip)) {
            std::copy_n(candidate.begin() + b * slots * dim, slots * dim, hidden.begin() + b * slots * dim);
            for (std::size_t slot = 0; slot < slots; ++slot) {
                active[b * slots + slot] = 1;
                for (std::size_t d = 0; d < dim; ++d) {
                    const auto source = ((b * clip_count + clip) * slots + slot) * dim + d;
                    previous[(b * slots + slot) * dim + d] = static_cast<float>(clips.values()[source]);
                }
            }
            ++observed[b];
        }
        std::vector<float> normalized(hidden.size());
        norm(hidden, weights_.norm_weight, weights_.norm_bias, rows, dim, normalized);
        for (std::size_t b = 0; b < batch; ++b)
            std::copy_n(normalized.begin() + b * slots * dim, slots * dim,
                checkpoints.begin() + (b * clip_count + clip) * slots * dim);
    }
    std::vector<float> normalized(hidden.size()); norm(hidden, weights_.norm_weight, weights_.norm_bias, rows, dim, normalized);
    std::vector<float> final_slot(batch * dim), order_norm(batch * dim), order(batch * 2);
    for (std::size_t b = 0; b < batch; ++b)
        std::copy_n(normalized.begin() + (b * slots + slots - 1) * dim, dim, final_slot.begin() + b * dim);
    norm(final_slot, weights_.order_norm_weight, weights_.order_norm_bias, batch, dim, order_norm);
    linear(order_norm, weights_.order_weight, weights_.order_bias, batch, dim, 2, order);
    std::vector<double> world_values(normalized.begin(), normalized.end()), order_values(order.begin(), order.end());
    std::vector<double> checkpoint_values(checkpoints.begin(), checkpoints.end());
    std::vector<std::uint8_t> dirty(active.size(), 0);
    WorldState state(Tensor(TensorDType::float32, {batch, slots, dim}, std::move(world_values), "cpu"),
        BooleanMask({batch, slots}, std::move(active)), BooleanMask({batch, slots}, std::move(dirty)),
        "unified:long-video");
    return {std::move(state), Tensor(TensorDType::float32, {batch, 2}, std::move(order_values), "cpu"),
        Tensor(TensorDType::float32, {batch, clip_count, slots, dim}, std::move(checkpoint_values), "cpu")};
}

}  // namespace swegca::world
