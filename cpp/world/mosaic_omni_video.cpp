#include "world/mosaic_omni_video.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace swegca::world {
namespace {

void require(const std::vector<float>& values, const std::size_t expected, const char* name) {
    if (values.size() != expected) throw std::invalid_argument(std::string(name) + " shape mismatch");
}

void norm(const std::span<const double> input, const std::span<const float> weight,
          const std::span<const float> bias, std::span<double> output) {
    double mean = 0.0; for (const auto value : input) mean += value; mean /= input.size();
    double variance = 0.0; for (const auto value : input) { const auto delta = value - mean; variance += delta * delta; }
    const auto inverse = 1.0 / std::sqrt(variance / input.size() + 1.0e-5);
    for (std::size_t index = 0; index < input.size(); ++index)
        output[index] = (input[index] - mean) * inverse * weight[index] + bias[index];
}

void linear(const std::span<const double> input, const std::span<const float> weight,
            const std::size_t in, const std::size_t out, std::span<double> output) {
    for (std::size_t destination = 0; destination < out; ++destination) {
        double value = 0.0; for (std::size_t source = 0; source < in; ++source)
            value += input[source] * weight[destination * in + source];
        output[destination] = value;
    }
}

std::vector<double> softmax(const std::vector<double>& values) {
    const auto maximum = *std::max_element(values.begin(), values.end());
    std::vector<double> result(values.size()); double denominator = 0.0;
    for (std::size_t index = 0; index < values.size(); ++index) {
        result[index] = std::exp(values[index] - maximum); denominator += result[index];
    }
    for (auto& value : result) value /= denominator; return result;
}

double gelu(const double value) { return 0.5 * value * (1.0 + std::erf(value / std::sqrt(2.0))); }

Tensor make(std::vector<std::uint64_t> shape, std::vector<double> values) {
    return Tensor(TensorDType::float32, std::move(shape), std::move(values), "cpu");
}

void validate_binding_inputs(const Tensor& identity, const Tensor& event,
    const Tensor& delta, const Tensor& trajectory, const std::size_t dimension) {
    const auto shape = identity.shape();
    if (shape.size() != 3 || shape[2] != dimension ||
        event.shape().size() != 3 || delta.shape().size() != 3 ||
        !std::equal(shape.begin(), shape.end(), event.shape().begin()) ||
        !std::equal(shape.begin(), shape.end(), delta.shape().begin()))
        throw std::invalid_argument("identity, event, and delta slots must match [B,S,D]");
    if (trajectory.shape().size() != 3 || trajectory.shape()[0] != shape[0] ||
        trajectory.shape()[1] != shape[1] || trajectory.shape()[2] != 12)
        throw std::invalid_argument("object trajectory geometry must be [B,S,12]");
}

struct BindingValues final {
    std::size_t batch{}, slots{}, dimension{};
    std::vector<double> keys, values;
};

BindingValues prepare_binding(const Tensor& identity, const Tensor& event, const Tensor& delta,
    const Tensor& trajectory, const ObjectTrajectoryBindingWeights& weights, const std::size_t dimension) {
    const auto batch = static_cast<std::size_t>(identity.shape()[0]), slots = static_cast<std::size_t>(identity.shape()[1]);
    BindingValues result{batch, slots, dimension, std::vector<double>(batch * slots * dimension),
        std::vector<double>(batch * slots * dimension)};
    const auto width = dimension * 3 + 12;
    std::vector<double> normalized_identity(dimension), joined(width), normalized(width), projected(dimension);
    for (std::size_t b = 0; b < batch; ++b) for (std::size_t slot = 0; slot < slots; ++slot) {
        const auto base = (b * slots + slot) * dimension;
        norm(identity.values().subspan(base, dimension), weights.identity_norm_weight,
            weights.identity_norm_bias, normalized_identity);
        linear(normalized_identity, weights.key_weight, dimension, dimension,
            std::span<double>(result.keys).subspan(base, dimension));
        for (std::size_t d = 0; d < dimension; ++d) {
            joined[d] = identity.values()[base + d]; joined[dimension + d] = event.values()[base + d];
            joined[2 * dimension + d] = delta.values()[base + d];
        }
        std::copy_n(trajectory.values().begin() + (b * slots + slot) * 12, 12, joined.begin() + 3 * dimension);
        norm(joined, weights.trajectory_norm_weight, weights.trajectory_norm_bias, normalized);
        linear(normalized, weights.value_weight, width, dimension, projected);
        for (std::size_t d = 0; d < dimension; ++d) result.values[base + d] = gelu(projected[d]);
    }
    return result;
}

}  // namespace

LearnedVideoObjectTracker::LearnedVideoObjectTracker(
    const std::size_t slots, const std::size_t dimension, const bool spatial_coordinates,
    VideoObjectTrackerWeights weights)
    : slots_(slots), dimension_(dimension), spatial_coordinates_(spatial_coordinates),
      weights_(std::move(weights)) {
    if (!slots_ || !dimension_) throw std::invalid_argument("learned object tracker dimensions must be positive");
    require(weights_.queries, slots_ * dimension_, "object queries");
    require(weights_.query_norm_weight, dimension_, "query norm weight"); require(weights_.query_norm_bias, dimension_, "query norm bias");
    require(weights_.token_norm_weight, dimension_, "token norm weight"); require(weights_.token_norm_bias, dimension_, "token norm bias");
    if (spatial_coordinates_) require(weights_.position_projection_weight, dimension_ * 2, "position projection weight");
    else if (!weights_.position_projection_weight.empty()) throw std::invalid_argument("disabled position projection must be empty");
}

VideoObjectTracks LearnedVideoObjectTracker::track_with_attention(
    const Tensor& feature_map, const std::size_t batch, const std::size_t frames) const {
    const auto shape = feature_map.shape();
    if (shape.size() != 4 || shape[0] != batch * frames || shape[1] != dimension_)
        throw std::invalid_argument("object feature map must be [batch*frames,dim,height,width]");
    const auto height = static_cast<std::size_t>(shape[2]), width = static_cast<std::size_t>(shape[3]);
    const auto pixels = height * width;
    std::vector<double> tokens(batch * frames * pixels * dimension_);
    for (std::size_t b = 0; b < batch; ++b) for (std::size_t f = 0; f < frames; ++f)
        for (std::size_t p = 0; p < pixels; ++p) for (std::size_t d = 0; d < dimension_; ++d) {
            auto value = feature_map.values()[((b * frames + f) * dimension_ + d) * pixels + p];
            if (spatial_coordinates_) {
                const auto x = width == 1 ? -1.0 : -1.0 + 2.0 * (p % width) / (width - 1);
                const auto y = height == 1 ? -1.0 : -1.0 + 2.0 * (p / width) / (height - 1);
                value += x * weights_.position_projection_weight[d * 2] + y * weights_.position_projection_weight[d * 2 + 1];
            }
            tokens[((b * frames + f) * pixels + p) * dimension_ + d] = value;
        }
    std::vector<double> state(batch * slots_ * dimension_, 0.0), tracks(batch * slots_ * frames * dimension_);
    std::vector<double> attentions(batch * slots_ * frames * pixels);
    std::vector<double> query(dimension_), token(dimension_), logits(slots_ * pixels);
    for (std::size_t f = 0; f < frames; ++f) for (std::size_t b = 0; b < batch; ++b) {
        for (std::size_t slot = 0; slot < slots_; ++slot) {
            std::vector<double> raw(dimension_); for (std::size_t d = 0; d < dimension_; ++d)
                raw[d] = state[(b * slots_ + slot) * dimension_ + d] + weights_.queries[slot * dimension_ + d];
            norm(raw, weights_.query_norm_weight, weights_.query_norm_bias, query);
            for (std::size_t p = 0; p < pixels; ++p) {
                norm(std::span<const double>(tokens).subspan(((b * frames + f) * pixels + p) * dimension_, dimension_),
                    weights_.token_norm_weight, weights_.token_norm_bias, token);
                double score = 0.0; for (std::size_t d = 0; d < dimension_; ++d) score += query[d] * token[d];
                logits[slot * pixels + p] = score / std::sqrt(static_cast<double>(dimension_));
            }
        }
        // Original first normalizes across slots for every pixel, then across pixels per slot.
        for (std::size_t p = 0; p < pixels; ++p) {
            std::vector<double> column(slots_); for (std::size_t slot = 0; slot < slots_; ++slot) column[slot] = logits[slot * pixels + p];
            const auto probabilities = softmax(column);
            for (std::size_t slot = 0; slot < slots_; ++slot) logits[slot * pixels + p] = std::max(probabilities[slot], 1.0e-6);
        }
        for (std::size_t slot = 0; slot < slots_; ++slot) {
            double denominator = 0.0; for (std::size_t p = 0; p < pixels; ++p) denominator += logits[slot * pixels + p];
            for (std::size_t d = 0; d < dimension_; ++d) {
                double value = 0.0; for (std::size_t p = 0; p < pixels; ++p) {
                    const auto probability = logits[slot * pixels + p] / denominator;
                    value += probability * tokens[((b * frames + f) * pixels + p) * dimension_ + d];
                    attentions[((b * slots_ + slot) * frames + f) * pixels + p] = probability;
                }
                state[(b * slots_ + slot) * dimension_ + d] = value;
                tracks[((b * slots_ + slot) * frames + f) * dimension_ + d] = value;
            }
        }
    }
    return {make({batch * slots_, frames, dimension_}, std::move(tracks)),
        make({batch, slots_, frames, height, width}, std::move(attentions))};
}

Tensor LearnedVideoObjectTracker::forward(
    const Tensor& feature_map, const std::size_t batch, const std::size_t frames) const {
    return track_with_attention(feature_map, batch, frames).tracks;
}

void ObjectTrajectoryBindingWeights::validate(const std::size_t dimension, const bool pair) const {
    const auto width = dimension * 3 + 12;
    require(identity_norm_weight, dimension, "identity norm weight"); require(identity_norm_bias, dimension, "identity norm bias");
    require(trajectory_norm_weight, width, "trajectory norm weight"); require(trajectory_norm_bias, width, "trajectory norm bias");
    require(query_weight, dimension * dimension, "trajectory query weight"); require(key_weight, dimension * dimension, "trajectory key weight");
    require(value_weight, dimension * width, "trajectory value weight");
    require(output_weight, dimension * dimension * (pair ? 4 : 1), "trajectory output weight");
}

QueryConditionedObjectTrajectoryBinding::QueryConditionedObjectTrajectoryBinding(
    const std::size_t dimension, ObjectTrajectoryBindingWeights weights)
    : dimension_(dimension), weights_(std::move(weights)) { if (!dimension_) throw std::invalid_argument("dimension must be positive"); weights_.validate(dimension_, false); }

ObjectTrajectoryBindingOutput QueryConditionedObjectTrajectoryBinding::forward(
    const Tensor& identity, const Tensor& event, const Tensor& delta,
    const Tensor& trajectory, const Tensor& query) const {
    validate_binding_inputs(identity, event, delta, trajectory, dimension_);
    const auto prepared = prepare_binding(identity, event, delta, trajectory, weights_, dimension_);
    if (query.shape().size() != 2 || query.shape()[0] != prepared.batch || query.shape()[1] != dimension_)
        throw std::invalid_argument("object trajectory query must be [B,D]");
    std::vector<double> decisions(prepared.batch * dimension_), all_weights(prepared.batch * prepared.slots);
    std::vector<double> projected_query(dimension_), scores(prepared.slots), selected(dimension_), output(dimension_);
    for (std::size_t b = 0; b < prepared.batch; ++b) {
        linear(query.values().subspan(b * dimension_, dimension_), weights_.query_weight, dimension_, dimension_, projected_query);
        for (std::size_t slot = 0; slot < prepared.slots; ++slot) {
            double score = 0.0; for (std::size_t d = 0; d < dimension_; ++d)
                score += projected_query[d] * prepared.keys[(b * prepared.slots + slot) * dimension_ + d];
            scores[slot] = score / std::sqrt(static_cast<double>(dimension_));
        }
        const auto probabilities = softmax(scores); std::fill(selected.begin(), selected.end(), 0.0);
        for (std::size_t slot = 0; slot < prepared.slots; ++slot) for (std::size_t d = 0; d < dimension_; ++d)
            selected[d] += probabilities[slot] * prepared.values[(b * prepared.slots + slot) * dimension_ + d];
        linear(selected, weights_.output_weight, dimension_, dimension_, output);
        std::copy(output.begin(), output.end(), decisions.begin() + b * dimension_);
        std::copy(probabilities.begin(), probabilities.end(), all_weights.begin() + b * prepared.slots);
    }
    return {make({prepared.batch, dimension_}, std::move(decisions)),
        make({prepared.batch, prepared.slots}, std::move(all_weights))};
}

QueryConditionedObjectPairTrajectoryBinding::QueryConditionedObjectPairTrajectoryBinding(
    const std::size_t dimension, ObjectTrajectoryBindingWeights weights)
    : dimension_(dimension), weights_(std::move(weights)) { if (!dimension_) throw std::invalid_argument("dimension must be positive"); weights_.validate(dimension_, true); }

ObjectTrajectoryBindingOutput QueryConditionedObjectPairTrajectoryBinding::forward(
    const Tensor& identity, const Tensor& event, const Tensor& delta,
    const Tensor& trajectory, const Tensor& queries) const {
    validate_binding_inputs(identity, event, delta, trajectory, dimension_);
    const auto prepared = prepare_binding(identity, event, delta, trajectory, weights_, dimension_);
    if (queries.shape().size() != 3 || queries.shape()[0] != prepared.batch || queries.shape()[1] != 2 || queries.shape()[2] != dimension_)
        throw std::invalid_argument("object pair queries must be [B,2,D]");
    std::vector<double> decisions(prepared.batch * dimension_), all_weights(prepared.batch * 2 * prepared.slots);
    std::vector<double> selected(2 * dimension_), projected_query(dimension_), scores(prepared.slots), joined(4 * dimension_), output(dimension_);
    for (std::size_t b = 0; b < prepared.batch; ++b) {
        for (std::size_t role = 0; role < 2; ++role) {
            linear(queries.values().subspan((b * 2 + role) * dimension_, dimension_), weights_.query_weight, dimension_, dimension_, projected_query);
            for (std::size_t slot = 0; slot < prepared.slots; ++slot) {
                double score = 0.0; for (std::size_t d = 0; d < dimension_; ++d)
                    score += projected_query[d] * prepared.keys[(b * prepared.slots + slot) * dimension_ + d];
                scores[slot] = score / std::sqrt(static_cast<double>(dimension_));
            }
            const auto probabilities = softmax(scores);
            for (std::size_t d = 0; d < dimension_; ++d) {
                selected[role * dimension_ + d] = 0.0;
                for (std::size_t slot = 0; slot < prepared.slots; ++slot)
                    selected[role * dimension_ + d] += probabilities[slot] * prepared.values[(b * prepared.slots + slot) * dimension_ + d];
            }
            std::copy(probabilities.begin(), probabilities.end(), all_weights.begin() + (b * 2 + role) * prepared.slots);
        }
        for (std::size_t d = 0; d < dimension_; ++d) {
            const auto first = selected[d], second = selected[dimension_ + d];
            joined[d] = first; joined[dimension_ + d] = second; joined[2 * dimension_ + d] = first - second;
            joined[3 * dimension_ + d] = first * second;
        }
        linear(joined, weights_.output_weight, 4 * dimension_, dimension_, output);
        std::copy(output.begin(), output.end(), decisions.begin() + b * dimension_);
    }
    return {make({prepared.batch, dimension_}, std::move(decisions)),
        make({prepared.batch, 2, prepared.slots}, std::move(all_weights))};
}

}  // namespace swegca::world
