#include "world/mosaic_omni_video_pipeline.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <string>
#include <utility>

namespace swegca::world {
namespace {

[[noreturn]] void invalid(const std::string& value) { throw std::invalid_argument(value); }

std::size_t size_of(const std::uint64_t value, const char* label) {
    if (value > std::numeric_limits<std::size_t>::max())
        throw std::overflow_error(std::string(label) + " exceeds size_t");
    return static_cast<std::size_t>(value);
}

std::size_t product(std::initializer_list<std::size_t> values, const char* label) {
    std::size_t result = 1;
    for (const auto value : values) {
        if (value && result > std::numeric_limits<std::size_t>::max() / value)
            throw std::overflow_error(std::string(label) + " shape overflow");
        result *= value;
    }
    return result;
}

std::vector<std::uint64_t> shape_of(std::initializer_list<std::size_t> values) {
    std::vector<std::uint64_t> result;
    result.reserve(values.size());
    for (const auto value : values) result.push_back(value);
    return result;
}

Tensor tensor_like(const Tensor& reference, std::vector<std::uint64_t> shape,
                   std::vector<double> values) {
    return Tensor(reference.dtype(), std::move(shape), std::move(values),
                  std::string(reference.device()));
}

void same_storage(const Tensor& left, const Tensor& right, const char* label) {
    if (left.dtype() != right.dtype() || left.device() != right.device())
        invalid(std::string(label) + " dtype/device mismatch");
}

void require_size(const std::vector<double>& values, const std::size_t expected,
                  const char* label) {
    if (values.size() != expected)
        invalid(std::string(label) + " has the wrong number of values");
}

void validate_linear(const ModalLinearWeights& weights, const std::size_t input,
                     const std::size_t output, const bool bias, const char* label) {
    require_size(weights.weight, product({input, output}, label), label);
    if (bias) require_size(weights.bias, output, label);
    else if (!weights.bias.empty()) invalid(std::string(label) + " must not have bias");
}

void validate_conv(const ModalConvWeights& weights, const std::size_t output,
                   const std::size_t input, const std::size_t kernel,
                   const char* label) {
    require_size(weights.weight, product({output, input, kernel, kernel}, label), label);
    require_size(weights.bias, output, label);
}

void validate_gru(const ModalGruWeights& weights, const std::size_t input,
                  const std::size_t hidden, const char* label) {
    require_size(weights.weight_ih, product({3 * hidden, input}, label), label);
    require_size(weights.weight_hh, product({3 * hidden, hidden}, label), label);
    require_size(weights.bias_ih, 3 * hidden, label);
    require_size(weights.bias_hh, 3 * hidden, label);
}

Tensor linear(const Tensor& input, const ModalLinearWeights& weights,
              const std::size_t output, const bool use_bias = true) {
    const auto shape = input.shape();
    if (shape.empty()) invalid("linear input must have a trailing dimension");
    const auto width = size_of(shape.back(), "linear width");
    validate_linear(weights, width, output, use_bias, "linear");
    const auto rows = input.values().size() / width;
    std::vector<double> values(product({rows, output}, "linear output"));
    for (std::size_t row = 0; row < rows; ++row)
        for (std::size_t destination = 0; destination < output; ++destination) {
            double value = use_bias ? weights.bias[destination] : 0.0;
            for (std::size_t source = 0; source < width; ++source)
                value += input.values()[row * width + source] *
                         weights.weight[destination * width + source];
            values[row * output + destination] = value;
        }
    std::vector<std::uint64_t> result_shape(shape.begin(), shape.end());
    result_shape.back() = output;
    return tensor_like(input, std::move(result_shape), std::move(values));
}

Tensor layer_norm(const Tensor& input, const std::vector<double>& weight,
                  const std::vector<double>& bias) {
    const auto shape = input.shape();
    if (shape.empty()) invalid("layer norm input must have a trailing dimension");
    const auto width = size_of(shape.back(), "layer norm width");
    require_size(weight, width, "layer norm weight");
    require_size(bias, width, "layer norm bias");
    const auto rows = input.values().size() / width;
    std::vector<double> values(input.values().size());
    for (std::size_t row = 0; row < rows; ++row) {
        const auto offset = row * width;
        double mean = 0.0;
        for (std::size_t column = 0; column < width; ++column)
            mean += input.values()[offset + column];
        mean /= static_cast<double>(width);
        double variance = 0.0;
        for (std::size_t column = 0; column < width; ++column) {
            const auto difference = input.values()[offset + column] - mean;
            variance += difference * difference;
        }
        const auto inverse = 1.0 / std::sqrt(variance / static_cast<double>(width) + 1.0e-5);
        for (std::size_t column = 0; column < width; ++column)
            values[offset + column] = (input.values()[offset + column] - mean) * inverse *
                                      weight[column] + bias[column];
    }
    return tensor_like(input, {shape.begin(), shape.end()}, std::move(values));
}

Tensor gelu(Tensor input) {
    std::vector<double> values(input.values().begin(), input.values().end());
    for (auto& value : values)
        value *= 0.5 * (1.0 + std::erf(value / std::sqrt(2.0)));
    return tensor_like(input, {input.shape().begin(), input.shape().end()}, std::move(values));
}

Tensor norm_linear(const Tensor& input, const VideoNormLinear& weights) {
    return gelu(linear(layer_norm(input, weights.norm_weight, weights.norm_bias),
                       weights.linear, weights.linear.bias.size()));
}

Tensor norm_mlp(const Tensor& input, const VideoNormMlp& weights) {
    auto hidden = gelu(linear(layer_norm(input, weights.norm_weight, weights.norm_bias),
                              weights.hidden, weights.hidden.bias.size()));
    return linear(hidden, weights.output, weights.output.bias.size());
}

Tensor mlp(const Tensor& input, const VideoMlp& weights) {
    return linear(gelu(linear(input, weights.hidden, weights.hidden.bias.size())),
                  weights.output, weights.output.bias.size());
}

Tensor add(const Tensor& left, const Tensor& right) {
    same_storage(left, right, "add");
    if (left.shape().size() != right.shape().size() ||
        !std::equal(left.shape().begin(), left.shape().end(), right.shape().begin()))
        invalid("add shape mismatch");
    std::vector<double> values(left.values().size());
    for (std::size_t i = 0; i < values.size(); ++i)
        values[i] = left.values()[i] + right.values()[i];
    return tensor_like(left, {left.shape().begin(), left.shape().end()}, std::move(values));
}

Tensor scale(const Tensor& input, const double amount) {
    std::vector<double> values(input.values().begin(), input.values().end());
    for (auto& value : values) value *= amount;
    return tensor_like(input, {input.shape().begin(), input.shape().end()}, std::move(values));
}

Tensor sigmoid(Tensor input) {
    std::vector<double> values(input.values().begin(), input.values().end());
    for (auto& value : values) value = 1.0 / (1.0 + std::exp(-value));
    return tensor_like(input, {input.shape().begin(), input.shape().end()}, std::move(values));
}

Tensor softmax_last(const Tensor& input) {
    const auto width = size_of(input.shape().back(), "softmax width");
    const auto rows = input.values().size() / width;
    std::vector<double> values(input.values().size());
    for (std::size_t row = 0; row < rows; ++row) {
        const auto begin = input.values().begin() + row * width;
        const auto maximum = *std::max_element(begin, begin + width);
        double denominator = 0.0;
        for (std::size_t i = 0; i < width; ++i) {
            values[row * width + i] = std::exp(input.values()[row * width + i] - maximum);
            denominator += values[row * width + i];
        }
        for (std::size_t i = 0; i < width; ++i) values[row * width + i] /= denominator;
    }
    return tensor_like(input, {input.shape().begin(), input.shape().end()}, std::move(values));
}

Tensor concatenate_last(const std::vector<const Tensor*>& tensors) {
    if (tensors.empty()) invalid("cannot concatenate an empty tensor list");
    const auto& first = *tensors.front();
    const auto rank = first.rank();
    if (!rank) invalid("concatenation requires rank >= 1");
    std::size_t rows = 1, total_width = 0;
    for (std::size_t i = 0; i + 1 < rank; ++i)
        rows = product({rows, size_of(first.shape()[i], "concatenation")}, "concatenation");
    for (const auto* tensor : tensors) {
        same_storage(first, *tensor, "concatenation");
        if (tensor->rank() != rank) invalid("concatenation rank mismatch");
        for (std::size_t i = 0; i + 1 < rank; ++i)
            if (tensor->shape()[i] != first.shape()[i]) invalid("concatenation shape mismatch");
        total_width += size_of(tensor->shape().back(), "concatenation width");
    }
    std::vector<double> values(product({rows, total_width}, "concatenation output"));
    for (std::size_t row = 0; row < rows; ++row) {
        std::size_t output = row * total_width;
        for (const auto* tensor : tensors) {
            const auto width = size_of(tensor->shape().back(), "concatenation width");
            std::copy_n(tensor->values().begin() + row * width, width, values.begin() + output);
            output += width;
        }
    }
    std::vector<std::uint64_t> shape(first.shape().begin(), first.shape().end());
    shape.back() = total_width;
    return tensor_like(first, std::move(shape), std::move(values));
}

Tensor mean_axis_one(const Tensor& input) {
    const auto shape = input.shape();
    if (shape.size() != 3 || shape[1] == 0) invalid("mean input must be [B,N,D] with N > 0");
    const auto batch = size_of(shape[0], "mean batch");
    const auto rows = size_of(shape[1], "mean rows");
    const auto width = size_of(shape[2], "mean width");
    std::vector<double> values(batch * width);
    for (std::size_t b = 0; b < batch; ++b)
        for (std::size_t row = 0; row < rows; ++row)
            for (std::size_t d = 0; d < width; ++d)
                values[b * width + d] += input.values()[(b * rows + row) * width + d] /
                                           static_cast<double>(rows);
    return tensor_like(input, shape_of({batch, width}), std::move(values));
}

Tensor conv2d(const Tensor& input, const ModalConvWeights& weights,
              const std::size_t output_channels, const std::size_t kernel) {
    const auto shape = input.shape();
    if (shape.size() != 4 || shape[1] != 3) invalid("object frontend requires [N,3,H,W]");
    const auto batch = size_of(shape[0], "conv batch");
    const auto height = size_of(shape[2], "conv height");
    const auto width = size_of(shape[3], "conv width");
    if (height < kernel || width < kernel) invalid("object kernel does not fit input");
    validate_conv(weights, output_channels, 3, kernel, "object frontend");
    const auto output_height = (height - kernel) / kernel + 1;
    const auto output_width = (width - kernel) / kernel + 1;
    std::vector<double> values(product({batch, output_channels, output_height, output_width},
                                       "object frontend output"));
    for (std::size_t b = 0; b < batch; ++b)
        for (std::size_t out = 0; out < output_channels; ++out)
            for (std::size_t y = 0; y < output_height; ++y)
                for (std::size_t x = 0; x < output_width; ++x) {
                    double value = weights.bias[out];
                    for (std::size_t in = 0; in < 3; ++in)
                        for (std::size_t ky = 0; ky < kernel; ++ky)
                            for (std::size_t kx = 0; kx < kernel; ++kx)
                                value += input.values()[((b * 3 + in) * height + y * kernel + ky) * width + x * kernel + kx] *
                                         weights.weight[((out * 3 + in) * kernel + ky) * kernel + kx];
                    values[((b * output_channels + out) * output_height + y) * output_width + x] = value;
                }
    return tensor_like(input, shape_of({batch, output_channels, output_height, output_width}),
                       std::move(values));
}

Tensor temporal_mix(const Tensor& input, const std::vector<double>& weights) {
    const auto shape = input.shape();
    if (shape.size() != 5) invalid("temporal mixer input must be [B,D,F,H,W]");
    const auto batch = size_of(shape[0], "mixer batch");
    const auto dim = size_of(shape[1], "mixer dimension");
    const auto frames = size_of(shape[2], "mixer frames");
    const auto height = size_of(shape[3], "mixer height");
    const auto width = size_of(shape[4], "mixer width");
    require_size(weights, dim * 3, "depthwise temporal mixer weight");
    std::vector<double> values(input.values().size());
    const auto at = [&](const std::size_t b, const std::size_t d, const std::size_t f,
                        const std::size_t y, const std::size_t x) {
        return ((((b * dim + d) * frames + f) * height + y) * width + x);
    };
    for (std::size_t b = 0; b < batch; ++b)
        for (std::size_t d = 0; d < dim; ++d)
            for (std::size_t f = 0; f < frames; ++f)
                for (std::size_t y = 0; y < height; ++y)
                    for (std::size_t x = 0; x < width; ++x) {
                        double value = 0.0;
                        if (f) value += input.values()[at(b, d, f - 1, y, x)] * weights[d * 3];
                        value += input.values()[at(b, d, f, y, x)] * weights[d * 3 + 1];
                        if (f + 1 < frames) value += input.values()[at(b, d, f + 1, y, x)] * weights[d * 3 + 2];
                        values[at(b, d, f, y, x)] = value;
                    }
    return tensor_like(input, {shape.begin(), shape.end()}, std::move(values));
}

Tensor frame_means(const Tensor& input) {
    const auto shape = input.shape();
    if (shape.size() != 5) invalid("frame means input must be [B,D,F,H,W]");
    const auto batch = size_of(shape[0], "frame means batch");
    const auto dim = size_of(shape[1], "frame means dimension");
    const auto frames = size_of(shape[2], "frame means frames");
    const auto pixels = product({size_of(shape[3], "height"), size_of(shape[4], "width")}, "pixels");
    std::vector<double> values(product({batch, frames, dim}, "frame means output"));
    for (std::size_t b = 0; b < batch; ++b)
        for (std::size_t f = 0; f < frames; ++f)
            for (std::size_t d = 0; d < dim; ++d) {
                const auto base = ((b * dim + d) * frames + f) * pixels;
                values[(b * frames + f) * dim + d] =
                    std::accumulate(input.values().begin() + base,
                                    input.values().begin() + base + pixels, 0.0) /
                    static_cast<double>(pixels);
            }
    return tensor_like(input, shape_of({batch, frames, dim}), std::move(values));
}

std::pair<Tensor, Tensor> gru(const Tensor& input, const ModalGruWeights& weights,
                              const std::size_t hidden) {
    const auto shape = input.shape();
    if (shape.size() != 3 || shape[2] == 0) invalid("GRU input must be [B,T,D]");
    const auto batch = size_of(shape[0], "GRU batch");
    const auto steps = size_of(shape[1], "GRU steps");
    const auto width = size_of(shape[2], "GRU width");
    validate_gru(weights, width, hidden, "GRU");
    std::vector<double> state(batch * hidden), output(batch * steps * hidden);
    for (std::size_t step = 0; step < steps; ++step)
        for (std::size_t b = 0; b < batch; ++b) {
            std::vector<double> input_gates(3 * hidden), hidden_gates(3 * hidden);
            for (std::size_t gate = 0; gate < 3 * hidden; ++gate) {
                input_gates[gate] = weights.bias_ih[gate];
                hidden_gates[gate] = weights.bias_hh[gate];
                for (std::size_t i = 0; i < width; ++i)
                    input_gates[gate] += input.values()[(b * steps + step) * width + i] *
                                         weights.weight_ih[gate * width + i];
                for (std::size_t i = 0; i < hidden; ++i)
                    hidden_gates[gate] += state[b * hidden + i] *
                                          weights.weight_hh[gate * hidden + i];
            }
            for (std::size_t i = 0; i < hidden; ++i) {
                const auto reset = 1.0 / (1.0 + std::exp(-(input_gates[i] + hidden_gates[i])));
                const auto update = 1.0 / (1.0 + std::exp(-(input_gates[hidden + i] + hidden_gates[hidden + i])));
                const auto candidate = std::tanh(input_gates[2 * hidden + i] + reset * hidden_gates[2 * hidden + i]);
                const auto next = (1.0 - update) * candidate + update * state[b * hidden + i];
                state[b * hidden + i] = next;
                output[(b * steps + step) * hidden + i] = next;
            }
        }
    return {tensor_like(input, shape_of({batch, steps, hidden}), std::move(output)),
            tensor_like(input, shape_of({batch, hidden}), std::move(state))};
}

Tensor time_embedding(const Tensor& reference, const std::size_t batch,
                      const std::size_t steps, const ModalLinearWeights& weights,
                      const std::size_t dim) {
    std::vector<double> times(batch * steps);
    for (std::size_t b = 0; b < batch; ++b)
        for (std::size_t step = 0; step < steps; ++step)
            times[b * steps + step] = steps == 1 ? 0.0 :
                static_cast<double>(step) / static_cast<double>(steps - 1);
    return linear(tensor_like(reference, shape_of({batch, steps, 1}), std::move(times)),
                  weights, dim, false);
}

Tensor endpoint_delta(const Tensor& input) {
    const auto shape = input.shape();
    const auto rows = size_of(shape[0], "endpoint rows");
    const auto steps = size_of(shape[1], "endpoint steps");
    const auto width = size_of(shape[2], "endpoint width");
    std::vector<double> values(rows * width);
    for (std::size_t row = 0; row < rows; ++row)
        for (std::size_t d = 0; d < width; ++d)
            values[row * width + d] = input.values()[(row * steps + steps - 1) * width + d] -
                                      input.values()[row * steps * width + d];
    return tensor_like(input, shape_of({rows, width}), std::move(values));
}

Tensor temporal_moment(const Tensor& input) {
    const auto shape = input.shape();
    const auto rows = size_of(shape[0], "moment rows");
    const auto steps = size_of(shape[1], "moment steps");
    const auto width = size_of(shape[2], "moment width");
    std::vector<double> values(rows * width);
    for (std::size_t row = 0; row < rows; ++row)
        for (std::size_t step = 0; step < steps; ++step) {
            const auto time = steps == 1 ? -1.0 : -1.0 + 2.0 * static_cast<double>(step) /
                                                        static_cast<double>(steps - 1);
            for (std::size_t d = 0; d < width; ++d)
                values[row * width + d] += input.values()[(row * steps + step) * width + d] *
                                           time / static_cast<double>(steps);
        }
    return tensor_like(input, shape_of({rows, width}), std::move(values));
}

Tensor reveal_delta(const Tensor& input) {
    const auto shape = input.shape();
    const auto rows = size_of(shape[0], "reveal rows");
    const auto steps = size_of(shape[1], "reveal steps");
    const auto width = size_of(shape[2], "reveal width");
    const auto reveal = std::min(steps - 1, std::max<std::size_t>(1, steps / 2 - (steps / 2 != 0)));
    std::vector<double> values(rows * width);
    for (std::size_t row = 0; row < rows; ++row)
        for (std::size_t d = 0; d < width; ++d)
            values[row * width + d] = input.values()[(row * steps + reveal) * width + d] -
                                      input.values()[row * steps * width + d];
    return tensor_like(input, shape_of({rows, width}), std::move(values));
}

Tensor slice_first_batch(const Tensor& input, const std::size_t batch) {
    const auto shape = input.shape();
    if (shape.empty() || shape[0] < batch) invalid("batch slice out of range");
    const auto row = input.values().size() / size_of(shape[0], "slice batch");
    std::vector<double> values(input.values().begin(), input.values().begin() + batch * row);
    std::vector<std::uint64_t> result(shape.begin(), shape.end());
    result[0] = batch;
    return tensor_like(input, std::move(result), std::move(values));
}

Tensor average_tensors(const std::vector<Tensor>& tensors) {
    if (tensors.empty()) invalid("cannot average an empty tensor sequence");
    auto result = tensors.front().clone();
    std::vector<double> values(result.values().size());
    for (const auto& tensor : tensors) {
        same_storage(result, tensor, "average");
        if (!std::equal(result.shape().begin(), result.shape().end(), tensor.shape().begin()))
            invalid("average shape mismatch");
        for (std::size_t i = 0; i < values.size(); ++i)
            values[i] += tensor.values()[i] / static_cast<double>(tensors.size());
    }
    return tensor_like(result, {result.shape().begin(), result.shape().end()}, std::move(values));
}

}  // namespace

VideoPipelineConfig VideoPipelineConfig::from_unified(const MosaicUnifiedConfig& source) {
    VideoPipelineConfig result;
    result.world_dim = source.omni.world_dim;
    result.world_slots = source.omni.world_slots;
    result.object_slots = source.omni.object_slots;
    result.vision_patch_size = static_cast<std::size_t>(source.vision_patch_size);
    result.vision_teacher_dim = static_cast<std::size_t>(source.vision_teacher_dim);
    result.camera_pose_dim = static_cast<std::size_t>(source.video_camera_pose_dim);
    result.spatial_relation_classes = static_cast<std::size_t>(source.video_spatial_relation_classes);
    result.action_dim = static_cast<std::size_t>(source.video_action_dim);
    result.egomotion_classes = static_cast<std::size_t>(source.video_egomotion_classes);
    result.uses_visual_semantic_encoder = source.video_uses_visual_semantic_encoder;
    result.visual_semantic_scale = source.video_visual_semantic_scale;
    result.explicit_temporal_delta = source.video_explicit_temporal_delta;
    result.explicit_temporal_delta_scale = source.video_explicit_temporal_delta_scale;
    result.separate_temporal_delta_projection = source.video_separate_temporal_delta_projection;
    result.object_temporal_encoder = source.video_object_temporal_encoder;
    result.object_frame_normalized_input = source.video_object_frame_normalized_input;
    result.object_camera_invariant_residual = source.video_object_camera_invariant_residual;
    result.object_frame_normalized_residual_scale = source.video_object_frame_normalized_residual_scale;
    result.object_time_centered_input = source.video_object_time_centered_input;
    result.object_activity_sorted_slots = source.video_object_activity_sorted_slots;
    result.object_dual_evidence = source.video_object_dual_evidence;
    result.object_set_decision = source.video_object_set_decision;
    result.object_identity_event_binding = source.video_object_identity_event_binding;
    result.object_learned_queries = source.video_object_learned_queries;
    result.object_spatial_event_binding = source.video_object_spatial_event_binding;
    result.spatial_temporal_moment = source.video_spatial_temporal_moment;
    result.query_spatial_temporal_moment = source.video_query_spatial_temporal_moment;
    result.spatial_temporal_y_moment = source.video_spatial_temporal_y_moment;
    result.spatial_temporal_logit_head = source.video_spatial_temporal_logit_head;
    result.spatial_temporal_bilinear_head = source.video_spatial_temporal_bilinear_head;
    result.object_trajectory_binding = source.video_object_trajectory_binding;
    result.object_pair_trajectory_binding = source.video_object_pair_trajectory_binding;
    result.descriptor_trajectory_binding = source.video_descriptor_trajectory_binding;
    result.descriptor_memory_evidence_routing = source.video_descriptor_object_memory_evidence_routing;
    result.descriptor_memory_evidence_routing_margin = source.video_descriptor_object_memory_evidence_routing_margin;
    result.query_conditioned_head = source.video_query_conditioned_head;
    result.camera_robustness_adapter = source.video_camera_robustness_adapter;
    result.camera_robustness_nonlinear_gate = source.video_camera_robustness_nonlinear_gate;
    result.egomotion_validity_head = source.video_egomotion_validity_head;
    result.egomotion_evidence_gate = source.video_egomotion_evidence_gate;
    result.egomotion_minimum_motion_evidence = source.video_egomotion_minimum_motion_evidence;
    result.validate();
    return result;
}

void VideoPipelineConfig::validate() const {
    if (!world_dim || !world_slots || !object_slots || !vision_patch_size)
        invalid("video pipeline dimensions must be positive");
    if (object_slots > world_slots) invalid("object slots exceed World slots");
    if (!(visual_semantic_scale > 0.0 && visual_semantic_scale <= 1.0))
        invalid("video semantic scale must be in (0,1]");
    if (!(explicit_temporal_delta_scale > 0.0)) invalid("video delta scale must be positive");
    if (separate_temporal_delta_projection && !explicit_temporal_delta)
        invalid("separate delta projection requires explicit temporal delta");
    if (object_dual_evidence && !object_temporal_encoder)
        invalid("dual object evidence requires object temporal encoder");
    if (object_set_decision && !object_dual_evidence)
        invalid("set decision requires dual object evidence");
    if (descriptor_memory_evidence_routing && !object_dual_evidence)
        invalid("descriptor memory routing requires dual evidence");
    if (!(descriptor_memory_evidence_routing_margin >= 0.0 &&
          descriptor_memory_evidence_routing_margin < 1.0))
        invalid("descriptor routing margin must be in [0,1)");
    if (camera_robustness_nonlinear_gate && !camera_robustness_adapter)
        invalid("nonlinear robustness gate requires robustness adapter");
    if (spatial_relation_classes && !descriptor_trajectory_binding)
        invalid("spatial relation head requires descriptor trajectory binding");
    if (egomotion_validity_head && !egomotion_classes)
        invalid("egomotion validity head requires egomotion classes");
}

void VideoPipelineWeights::validate(const VideoPipelineConfig& config) const {
    const auto dim = config.world_dim;
    validate_linear(video_time, 1, dim, false, "video time");
    require_size(temporal_mixer_weight, dim * 3, "video temporal mixer");
    validate_gru(temporal_cell, dim, dim, "video temporal GRU");
    validate_linear(temporal_to_world, dim, dim, true, "video temporal-to-world");
    if (config.separate_temporal_delta_projection) {
        if (!temporal_delta_to_world) invalid("missing temporal delta projection");
        validate_linear(*temporal_delta_to_world, dim, dim, false, "video delta projection");
    } else if (temporal_delta_to_world) invalid("unexpected temporal delta projection");
    require_size(video_modality_embedding, dim, "video modality embedding");
    require_size(order_head.norm_weight, dim, "video order norm");
    require_size(order_head.norm_bias, dim, "video order norm");
    validate_linear(order_head.linear, dim, 2, true, "video order head");
    if (config.object_temporal_encoder) {
        if (!object_temporal_cell || !object_frontend || !object_to_world ||
            !object_statistics || !object_decision)
            invalid("object temporal encoder weights are incomplete");
        validate_gru(*object_temporal_cell, dim, dim, "object temporal GRU");
        const auto patch = std::max<std::size_t>(2, config.vision_patch_size / 2);
        validate_conv(*object_frontend, dim, 3, patch, "object frontend");
        validate_linear(*object_to_world, dim, dim, true, "object-to-world");
        validate_linear(*object_statistics, 3 * dim, dim, true, "object statistics");
        const auto decision_width = dim * (config.object_slots + 1);
        require_size(object_decision->norm_weight, decision_width, "object decision norm");
        require_size(object_decision->norm_bias, decision_width, "object decision norm");
        validate_linear(object_decision->linear, decision_width, dim, true, "object decision");
    }
    if (config.object_camera_invariant_residual) {
        if (!object_camera_invariant_frontend) invalid("missing camera invariant frontend");
        validate_conv(*object_camera_invariant_frontend, dim, 3,
                      std::max<std::size_t>(2, config.vision_patch_size / 2),
                      "camera invariant frontend");
    }
    const auto require_optional_linear = [&](const bool enabled, const auto& value,
                                             const std::size_t input, const std::size_t output,
                                             const bool bias, const char* name) {
        if (enabled != value.has_value()) invalid(std::string(name) + " presence mismatch");
        if (value) validate_linear(*value, input, output, bias, name);
    };
    if (config.object_set_decision != object_set_decision.has_value())
        invalid("object set decision presence mismatch");
    if (object_set_decision) {
        require_size(object_set_decision->norm_weight, 2 * dim, "object set decision norm");
        require_size(object_set_decision->norm_bias, 2 * dim, "object set decision norm");
        validate_linear(object_set_decision->linear, 2 * dim, dim, true, "object set decision");
    }
    if (config.object_identity_event_binding != object_binding_decision.has_value())
        invalid("object binding decision presence mismatch");
    if (object_binding_decision) {
        require_size(object_binding_decision->norm_weight, 4 * dim, "object binding norm");
        require_size(object_binding_decision->norm_bias, 4 * dim, "object binding norm");
        validate_linear(object_binding_decision->linear, 4 * dim, dim, true, "object binding");
    }
    require_optional_linear(config.spatial_temporal_moment, spatial_moment_to_world, 2, dim, false, "x moment projection");
    require_optional_linear(config.query_spatial_temporal_moment, spatial_moment_gate, dim, 2, true, "x moment gate");
    require_optional_linear(config.spatial_temporal_y_moment, spatial_y_moment_to_world, 2, dim, false, "y moment projection");
    require_optional_linear(config.spatial_temporal_y_moment, spatial_y_moment_gate, dim, 2, true, "y moment gate");
    require_optional_linear(config.spatial_temporal_logit_head, spatial_logit_head, dim + 4, 2, true, "spatial logit head");
    require_optional_linear(config.spatial_temporal_bilinear_head, spatial_bilinear_head, dim, 4, true, "spatial bilinear head");
    require_optional_linear(config.query_conditioned_head, query_conditioning, dim, 2 * dim, true, "query conditioning");
    require_optional_linear(config.object_dual_evidence, object_evidence_gate, dim, 2, true, "object evidence gate");
    require_optional_linear(config.camera_robustness_adapter && !config.camera_robustness_nonlinear_gate,
                            camera_robustness_gate_linear, 8, 1, true, "camera robustness gate");
    if (config.camera_robustness_nonlinear_gate != camera_robustness_gate_nonlinear.has_value())
        invalid("nonlinear camera gate presence mismatch");
    if (camera_robustness_gate_nonlinear) {
        validate_linear(camera_robustness_gate_nonlinear->hidden, 8, 16, true, "camera gate hidden");
        validate_linear(camera_robustness_gate_nonlinear->output, 16, 1, true, "camera gate output");
    }
    require_optional_linear(config.camera_robustness_adapter, camera_robustness_head, dim, 2, false, "camera residual head");
    require_optional_linear(config.camera_pose_dim != 0, camera_pose_encoder, config.camera_pose_dim, dim, true, "camera pose encoder");
    require_optional_linear(config.action_dim != 0, action_encoder, config.action_dim, dim, true, "action encoder");
    require_optional_linear(config.vision_teacher_dim != 0, teacher_projection, dim, config.vision_teacher_dim, true, "teacher projection");
    if (config.spatial_relation_classes != spatial_relation_head.has_value()) invalid("spatial relation head presence mismatch");
    if (spatial_relation_head) {
        const auto width = 2 * dim + 48 + (config.camera_pose_dim ? VideoSpatialGeometryReasoner::output_dim : 0);
        require_size(spatial_relation_head->norm_weight, width, "spatial relation norm");
        require_size(spatial_relation_head->norm_bias, width, "spatial relation norm");
        validate_linear(spatial_relation_head->hidden, width, dim, true, "spatial relation hidden");
        validate_linear(spatial_relation_head->output, dim, config.spatial_relation_classes, true, "spatial relation output");
    }
    if (config.egomotion_classes != egomotion_head.has_value()) invalid("egomotion head presence mismatch");
    if (egomotion_head) {
        require_size(egomotion_head->norm_weight, 3 * dim, "egomotion norm");
        require_size(egomotion_head->norm_bias, 3 * dim, "egomotion norm");
        validate_linear(egomotion_head->hidden, 3 * dim, dim, true, "egomotion hidden");
        validate_linear(egomotion_head->output, dim, config.egomotion_classes, true, "egomotion output");
    }
    if (config.egomotion_validity_head != egomotion_validity_head_weights.has_value()) invalid("egomotion validity head presence mismatch");
    if (egomotion_validity_head_weights) {
        require_size(egomotion_validity_head_weights->norm_weight, dim + 2, "egomotion validity norm");
        require_size(egomotion_validity_head_weights->norm_bias, dim + 2, "egomotion validity norm");
        validate_linear(egomotion_validity_head_weights->hidden, dim + 2, 64, true, "egomotion validity hidden");
        validate_linear(egomotion_validity_head_weights->output, 64, 2, true, "egomotion validity output");
    }
}

MosaicOmniVideoPipeline::MosaicOmniVideoPipeline(VideoPipelineConfig config,
                                                 VideoPipelineWeights weights,
                                                 VideoPipelineModules modules)
    : config_(std::move(config)), weights_(std::move(weights)), modules_(std::move(modules)) {
    config_.validate();
    weights_.validate(config_);
    if (!modules_.frontends) invalid("video pipeline requires modal frontends");
    if (config_.object_learned_queries != static_cast<bool>(modules_.object_tracker)) invalid("object tracker presence mismatch");
    if (config_.object_trajectory_binding != static_cast<bool>(modules_.trajectory_binding)) invalid("trajectory binding presence mismatch");
    if (config_.object_pair_trajectory_binding != static_cast<bool>(modules_.pair_trajectory_binding)) invalid("pair trajectory binding presence mismatch");
    if (config_.descriptor_trajectory_binding != static_cast<bool>(modules_.descriptor_binding)) invalid("descriptor binding presence mismatch");
    if ((config_.camera_pose_dim && config_.spatial_relation_classes) != static_cast<bool>(modules_.spatial_geometry)) invalid("spatial geometry presence mismatch");
    if ((config_.egomotion_classes != 0) != static_cast<bool>(modules_.egomotion)) invalid("egomotion reasoner presence mismatch");
}

namespace {

Tensor reshape_video_channels_first(const Tensor& map, const std::size_t batch,
                                    const std::size_t frames) {
    const auto shape = map.shape();
    if (shape.size() != 4 || shape[0] != batch * frames)
        invalid("visual map must be [B*F,D,H,W]");
    const auto dim = size_of(shape[1], "visual map dimension");
    const auto height = size_of(shape[2], "visual map height");
    const auto width = size_of(shape[3], "visual map width");
    std::vector<double> values(map.values().size());
    for (std::size_t b = 0; b < batch; ++b)
        for (std::size_t f = 0; f < frames; ++f)
            for (std::size_t d = 0; d < dim; ++d)
                for (std::size_t p = 0; p < height * width; ++p)
                    values[((b * dim + d) * frames + f) * height * width + p] =
                        map.values()[((b * frames + f) * dim + d) * height * width + p];
    return tensor_like(map, shape_of({batch, dim, frames, height, width}), std::move(values));
}

Tensor visual_tokens(const Tensor& video, const ModalLinearWeights& time,
                     const std::vector<double>& modality) {
    const auto shape = video.shape();
    const auto batch = size_of(shape[0], "video token batch");
    const auto dim = size_of(shape[1], "video token dimension");
    const auto frames = size_of(shape[2], "video token frames");
    const auto pixels = product({size_of(shape[3], "video token height"),
                                 size_of(shape[4], "video token width")}, "video token pixels");
    require_size(modality, dim, "video modality embedding");
    const auto times = time_embedding(video, batch, frames, time, dim);
    std::vector<double> values(product({batch, frames * pixels, dim}, "video tokens"));
    for (std::size_t b = 0; b < batch; ++b)
        for (std::size_t f = 0; f < frames; ++f)
            for (std::size_t p = 0; p < pixels; ++p)
                for (std::size_t d = 0; d < dim; ++d)
                    values[((b * frames * pixels + f * pixels + p) * dim) + d] =
                        video.values()[((b * dim + d) * frames + f) * pixels + p] +
                        times.values()[(b * frames + f) * dim + d] + modality[d];
    return tensor_like(video, shape_of({batch, frames * pixels, dim}), std::move(values));
}

Tensor append_sequence(const Tensor& first, const Tensor& second,
                       const std::vector<double>& second_embedding) {
    same_storage(first, second, "sequence append");
    const auto a = first.shape(), b = second.shape();
    if (a.size() != 3 || b.size() != 3 || a[0] != b[0] || a[2] != b[2])
        invalid("sequence append shape mismatch");
    const auto batch = size_of(a[0], "sequence batch");
    const auto first_rows = size_of(a[1], "first sequence");
    const auto second_rows = size_of(b[1], "second sequence");
    const auto width = size_of(a[2], "sequence width");
    require_size(second_embedding, width, "sequence embedding");
    std::vector<double> values(product({batch, first_rows + second_rows, width}, "sequence append"));
    for (std::size_t batch_index = 0; batch_index < batch; ++batch_index) {
        std::copy_n(first.values().begin() + batch_index * first_rows * width,
                    first_rows * width,
                    values.begin() + batch_index * (first_rows + second_rows) * width);
        for (std::size_t row = 0; row < second_rows; ++row)
            for (std::size_t d = 0; d < width; ++d)
                values[(batch_index * (first_rows + second_rows) + first_rows + row) * width + d] =
                    second.values()[(batch_index * second_rows + row) * width + d] + second_embedding[d];
    }
    return tensor_like(first, shape_of({batch, first_rows + second_rows, width}), std::move(values));
}

Tensor adaptive_object_grid(const Tensor& feature, const std::size_t batch,
                            const std::size_t frames, const std::size_t slots) {
    const auto shape = feature.shape();
    const auto dim = size_of(shape[1], "object grid dimension");
    const auto height = size_of(shape[2], "object grid height");
    const auto width = size_of(shape[3], "object grid width");
    std::size_t rows = static_cast<std::size_t>(std::sqrt(static_cast<double>(slots)));
    while (rows > 1 && slots % rows) --rows;
    const auto columns = slots / rows;
    std::vector<double> values(product({batch, slots, frames, dim}, "object grid"));
    for (std::size_t b = 0; b < batch; ++b)
        for (std::size_t f = 0; f < frames; ++f)
            for (std::size_t gy = 0; gy < rows; ++gy)
                for (std::size_t gx = 0; gx < columns; ++gx) {
                    const auto y0 = gy * height / rows;
                    const auto y1 = (gy + 1) * height / rows;
                    const auto x0 = gx * width / columns;
                    const auto x1 = (gx + 1) * width / columns;
                    const auto count = (y1 - y0) * (x1 - x0);
                    if (!count) invalid("adaptive object pool produced an empty bin");
                    const auto slot = gy * columns + gx;
                    for (std::size_t d = 0; d < dim; ++d) {
                        double value = 0.0;
                        for (std::size_t y = y0; y < y1; ++y)
                            for (std::size_t x = x0; x < x1; ++x)
                                value += feature.values()[(((b * frames + f) * dim + d) * height + y) * width + x];
                        values[((b * slots + slot) * frames + f) * dim + d] = value / static_cast<double>(count);
                    }
                }
    return tensor_like(feature, shape_of({batch * slots, frames, dim}), std::move(values));
}

struct TrajectoryFeatures final { Tensor identity, event, delta, geometry; };

TrajectoryFeatures trajectory_features(const Tensor& frame_grid, const Tensor& attention,
                                        const std::size_t batch, const std::size_t slots) {
    const auto shape = frame_grid.shape();
    const auto frames = size_of(shape[1], "trajectory frames");
    const auto dim = size_of(shape[2], "trajectory dimension");
    std::vector<double> identity(batch * slots * dim), event(identity.size()), delta(identity.size());
    for (std::size_t b = 0; b < batch; ++b)
        for (std::size_t slot = 0; slot < slots; ++slot)
            for (std::size_t d = 0; d < dim; ++d) {
                const auto out = (b * slots + slot) * dim + d;
                const auto first = frame_grid.values()[((b * slots + slot) * frames) * dim + d];
                const auto last = frame_grid.values()[((b * slots + slot) * frames + frames - 1) * dim + d];
                identity[out] = (first + last) * 0.5;
                delta[out] = last - first;
                double mean = 0.0;
                for (std::size_t frame = 0; frame < frames; ++frame)
                    mean += frame_grid.values()[((b * slots + slot) * frames + frame) * dim + d];
                mean /= static_cast<double>(frames);
                for (std::size_t frame = 0; frame < frames; ++frame) {
                    const auto time = frames == 1 ? -1.0 : -1.0 + 2.0 * frame / static_cast<double>(frames - 1);
                    event[out] += (frame_grid.values()[((b * slots + slot) * frames + frame) * dim + d] - mean) *
                                  time / static_cast<double>(frames);
                }
            }
    const auto geometry = object_attention_trajectory(slice_first_batch(attention, batch));
    return {tensor_like(frame_grid, shape_of({batch, slots, dim}), std::move(identity)),
            tensor_like(frame_grid, shape_of({batch, slots, dim}), std::move(event)),
            tensor_like(frame_grid, shape_of({batch, slots, dim}), std::move(delta)), geometry};
}

Tensor binding_features(const Tensor& frame_grid, const Tensor* attention,
                        const std::size_t batch, const std::size_t slots,
                        const bool spatial) {
    const auto frames = size_of(frame_grid.shape()[1], "binding frames");
    const auto dim = size_of(frame_grid.shape()[2], "binding dimension");
    std::vector<double> identity(batch * slots * dim), event(identity.size());
    for (std::size_t b = 0; b < batch; ++b)
        for (std::size_t slot = 0; slot < slots; ++slot)
            for (std::size_t d = 0; d < dim; ++d) {
                const auto out = (b * slots + slot) * dim + d;
                for (std::size_t frame = 0; frame < frames; ++frame)
                    identity[out] += frame_grid.values()[((b * slots + slot) * frames + frame) * dim + d] /
                                     static_cast<double>(frames);
                for (std::size_t frame = 0; frame < frames; ++frame) {
                    const auto time = frames == 1 ? -1.0 : -1.0 + 2.0 * frame / static_cast<double>(frames - 1);
                    event[out] += (frame_grid.values()[((b * slots + slot) * frames + frame) * dim + d] - identity[out]) *
                                  time / static_cast<double>(frames);
                }
            }
    auto identity_tensor = tensor_like(frame_grid, shape_of({batch, slots, dim}), std::move(identity));
    auto event_tensor = tensor_like(frame_grid, shape_of({batch, slots, dim}), std::move(event));
    if (spatial) {
        if (!attention) invalid("spatial event binding requires attention");
        event_tensor = add(event_tensor, spatial_event_features(
            slice_first_batch(frame_grid, batch * slots), slice_first_batch(*attention, batch)));
    }
    std::vector<double> product_values(identity_tensor.values().size());
    for (std::size_t i = 0; i < product_values.size(); ++i)
        product_values[i] = identity_tensor.values()[i] * event_tensor.values()[i];
    auto interaction = tensor_like(identity_tensor, shape_of({batch, slots, dim}), std::move(product_values));
    return concatenate_last({&identity_tensor, &event_tensor, &interaction});
}

Tensor add_repeated_time(const Tensor& frame_grid, const ModalLinearWeights& weights,
                         const std::size_t object_batch, const std::size_t slots,
                         const std::size_t dim) {
    const auto frames = size_of(frame_grid.shape()[1], "object frames");
    auto one = time_embedding(frame_grid, 1, frames, weights, dim);
    std::vector<double> values(frame_grid.values().begin(), frame_grid.values().end());
    for (std::size_t row = 0; row < object_batch * slots; ++row)
        for (std::size_t frame = 0; frame < frames; ++frame)
            for (std::size_t d = 0; d < dim; ++d)
                values[(row * frames + frame) * dim + d] += one.values()[frame * dim + d];
    return tensor_like(frame_grid, {frame_grid.shape().begin(), frame_grid.shape().end()}, std::move(values));
}

Tensor reshape_hidden_slots(const Tensor& hidden, const std::size_t batch,
                            const std::size_t slots) {
    return tensor_like(hidden, shape_of({batch, slots, size_of(hidden.shape()[1], "hidden dim")}),
                       {hidden.values().begin(), hidden.values().end()});
}

Tensor stack_pair(const Tensor& first, const Tensor& second) {
    same_storage(first, second, "stack pair");
    if (first.shape().size() != 1 || second.shape().size() != 1 || first.shape()[0] != second.shape()[0])
        invalid("pair stack expects matching [B]");
    const auto batch = size_of(first.shape()[0], "pair batch");
    std::vector<double> values(batch * 2);
    for (std::size_t b = 0; b < batch; ++b) {
        values[b * 2] = first.values()[b];
        values[b * 2 + 1] = second.values()[b];
    }
    return tensor_like(first, shape_of({batch, 2}), std::move(values));
}

}  // namespace

VideoPrepared MosaicOmniVideoPipeline::prepare(const VideoPipelineInput& input) const {
    const auto video_shape = input.video_values.shape();
    if (video_shape.size() != 5 || video_shape[2] != 3)
        invalid("video_values must have shape [batch,frames,3,height,width]");
    const auto batch = size_of(video_shape[0], "video batch");
    const auto frames = size_of(video_shape[1], "video frames");
    const auto height = size_of(video_shape[3], "video height");
    const auto width = size_of(video_shape[4], "video width");
    if (!batch || !frames || !height || !width) invalid("video dimensions must be non-zero");

    std::optional<Tensor> camera_states, camera_summary, action_summary, ego_summary, camera_stats;
    if (input.camera_pose_values) {
        if (!weights_.camera_pose_encoder) invalid("camera pose values require the camera pose encoder");
        const auto shape = input.camera_pose_values->shape();
        if (shape.size() != 3 || shape[0] != batch || shape[1] != frames || shape[2] != config_.camera_pose_dim)
            invalid("camera pose values have the wrong shape");
        same_storage(input.video_values, *input.camera_pose_values, "camera pose");
        camera_states = linear(*input.camera_pose_values, *weights_.camera_pose_encoder, config_.world_dim);
        camera_summary = mean_axis_one(*camera_states);
    }
    if (input.action_values) {
        if (!weights_.action_encoder) invalid("action values require the action encoder");
        const auto shape = input.action_values->shape();
        if (shape.size() != 2 || shape[0] != batch || shape[1] != config_.action_dim)
            invalid("action values have the wrong shape");
        same_storage(input.video_values, *input.action_values, "action");
        action_summary = linear(*input.action_values, *weights_.action_encoder, config_.world_dim);
    }
    if (input.action_values && modules_.egomotion)
        ego_summary = modules_.egomotion->forward(input.video_values);
    if (config_.camera_robustness_adapter)
        camera_stats = video_camera_statistics(input.video_values);

    auto flattened = tensor_like(input.video_values,
        shape_of({batch * frames, 3, height, width}),
        {input.video_values.values().begin(), input.video_values.values().end()});
    modules_.frontends->validate_image(flattened, batch * frames, "video_values");
    auto frontend = modules_.frontends->frontend_input(flattened);
    auto visual_map = modules_.frontends->encode_visual_map(frontend);
    if (config_.uses_visual_semantic_encoder) {
        const auto semantic = modules_.frontends->encode_visual_semantic_map(frontend);
        visual_map = add(scale(visual_map, 1.0 - config_.visual_semantic_scale),
                         scale(semantic, config_.visual_semantic_scale));
    }

    std::optional<Tensor> object_map, raw_object_map, normalized_object_map;
    std::vector<Tensor> dense_features;
    std::size_t evidence_count = 1;
    if (weights_.object_frontend) {
        const auto patch = std::max<std::size_t>(2, config_.vision_patch_size / 2);
        std::optional<Tensor> camera_residual;
        if (weights_.object_camera_invariant_frontend)
            camera_residual = conv2d(camera_invariant_object_frames(frontend),
                                     *weights_.object_camera_invariant_frontend,
                                     config_.world_dim, patch);
        raw_object_map = conv2d(frontend, *weights_.object_frontend, config_.world_dim, patch);
        normalized_object_map = conv2d(normalize_object_frontend_frames(frontend),
                                       *weights_.object_frontend, config_.world_dim, patch);
        if (camera_residual) {
            raw_object_map = add(*raw_object_map, *camera_residual);
            normalized_object_map = add(*normalized_object_map, *camera_residual);
        }
        if (config_.object_dual_evidence) {
            evidence_count = 2;
            const auto map_shape = raw_object_map->shape();
            const auto one = raw_object_map->values().size();
            std::vector<double> joined(2 * one);
            std::copy(raw_object_map->values().begin(), raw_object_map->values().end(), joined.begin());
            std::copy(normalized_object_map->values().begin(), normalized_object_map->values().end(), joined.begin() + one);
            object_map = tensor_like(*raw_object_map,
                shape_of({2 * batch * frames, size_of(map_shape[1], "object dim"),
                          size_of(map_shape[2], "object height"), size_of(map_shape[3], "object width")}),
                std::move(joined));
        } else if (!config_.object_frame_normalized_input) object_map = raw_object_map;
        else if (config_.object_frame_normalized_residual_scale == 1.0) object_map = normalized_object_map;
        else object_map = add(scale(*raw_object_map, 1.0 - config_.object_frame_normalized_residual_scale),
                              scale(*normalized_object_map, config_.object_frame_normalized_residual_scale));
        if (modules_.descriptor_binding) {
            const auto shape = object_map->shape();
            const auto chunk = object_map->values().size() / evidence_count;
            for (std::size_t evidence = 0; evidence < evidence_count; ++evidence)
                dense_features.emplace_back(tensor_like(*object_map,
                    shape_of({batch, frames, size_of(shape[1], "dense dim"),
                              size_of(shape[2], "dense height"), size_of(shape[3], "dense width")}),
                    {object_map->values().begin() + evidence * chunk,
                     object_map->values().begin() + (evidence + 1) * chunk}));
        }
    }

    std::optional<Tensor> x_moment, y_moment, xy_features;
    if (config_.spatial_temporal_moment) {
        x_moment = stack_pair(spatial_temporal_moment(*raw_object_map, batch, frames),
                              spatial_temporal_moment(*normalized_object_map, batch, frames));
    }
    if (config_.spatial_temporal_y_moment) {
        y_moment = stack_pair(spatial_temporal_moment(*raw_object_map, batch, frames, 'y'),
                              spatial_temporal_moment(*normalized_object_map, batch, frames, 'y'));
        xy_features = concatenate_last({&*x_moment, &*y_moment});
    }

    auto video = reshape_video_channels_first(visual_map, batch, frames);
    std::optional<Tensor> temporal_delta_value, temporal_delta_world;
    if (config_.explicit_temporal_delta) temporal_delta_value = endpoint_delta(frame_means(video));
    video = add(video, temporal_mix(video, weights_.temporal_mixer_weight));
    auto frame_states = frame_means(video);
    frame_states = add(frame_states, time_embedding(video, batch, frames, weights_.video_time, config_.world_dim));
    auto [temporal_states, temporal_summary] = gru(frame_states, weights_.temporal_cell, config_.world_dim);
    auto evidence_tokens = linear(temporal_states, weights_.temporal_to_world, config_.world_dim);
    auto world_summary = linear(temporal_summary, weights_.temporal_to_world, config_.world_dim);
    if (temporal_delta_value) {
        auto scaled_delta = scale(*temporal_delta_value, config_.explicit_temporal_delta_scale);
        if (weights_.temporal_delta_to_world)
            temporal_delta_world = linear(scaled_delta, *weights_.temporal_delta_to_world, config_.world_dim, false);
        else {
            auto no_bias = weights_.temporal_to_world;
            no_bias.bias.clear();
            temporal_delta_world = linear(scaled_delta, no_bias, config_.world_dim, false);
        }
    }

    std::optional<Tensor> object_slots, normalized_slots, object_world_slots, binding;
    std::optional<Tensor> identity, event, delta, geometry, attention_output;
    if (weights_.object_temporal_cell) {
        const auto object_batch = batch * evidence_count;
        std::optional<Tensor> attention;
        Tensor frame_grid = adaptive_object_grid(*object_map, object_batch, frames, config_.object_slots);
        if (modules_.object_tracker) {
            if (config_.object_spatial_event_binding || modules_.trajectory_binding || modules_.pair_trajectory_binding) {
                auto tracks = modules_.object_tracker->track_with_attention(*object_map, object_batch, frames);
                frame_grid = std::move(tracks.tracks);
                attention = std::move(tracks.attention);
            } else {
                frame_grid = modules_.object_tracker->forward(*object_map, object_batch, frames);
            }
        }
        const auto raw_grid = frame_grid.clone();
        if (modules_.trajectory_binding || modules_.pair_trajectory_binding) {
            if (!attention) invalid("object trajectory binding requires object attention");
            auto features = trajectory_features(raw_grid, *attention, batch, config_.object_slots);
            identity = std::move(features.identity); event = std::move(features.event);
            delta = std::move(features.delta); geometry = std::move(features.geometry);
            attention_output = slice_first_batch(*attention, batch);
        }
        if (weights_.object_binding_decision)
            binding = binding_features(raw_grid, attention ? &*attention : nullptr,
                                       batch, config_.object_slots,
                                       config_.object_spatial_event_binding);
        frame_grid = select_object_temporal_evidence(raw_grid, config_.object_time_centered_input,
                                                      config_.object_dual_evidence,
                                                      batch * config_.object_slots);
        auto statistics_input_a = endpoint_delta(frame_grid);
        auto statistics_input_b = temporal_moment(frame_grid);
        auto statistics_input_c = reveal_delta(frame_grid);
        auto statistics_input = concatenate_last({&statistics_input_a, &statistics_input_b, &statistics_input_c});
        auto statistics = linear(statistics_input, *weights_.object_statistics, config_.world_dim);
        auto [unused_states, hidden] = gru(add_repeated_time(frame_grid, weights_.video_time,
                                                             object_batch, config_.object_slots,
                                                             config_.world_dim),
                                           *weights_.object_temporal_cell, config_.world_dim);
        (void)unused_states;
        auto encoded = reshape_hidden_slots(add(hidden, statistics), object_batch, config_.object_slots);
        if (config_.object_activity_sorted_slots) {
            auto reshaped_grid = tensor_like(raw_grid,
                shape_of({object_batch, config_.object_slots, frames, config_.world_dim}),
                {raw_grid.values().begin(), raw_grid.values().end()});
            encoded = sort_object_slots_by_temporal_activity(encoded, reshaped_grid);
        }
        object_slots = slice_first_batch(encoded, batch);
        object_world_slots = linear(*object_slots, *weights_.object_to_world, config_.world_dim);
        if (config_.object_dual_evidence) {
            const auto one = batch * config_.object_slots * config_.world_dim;
            normalized_slots = tensor_like(encoded, shape_of({batch, config_.object_slots, config_.world_dim}),
                {encoded.values().begin() + one, encoded.values().begin() + 2 * one});
        }
    }

    auto tokens = visual_tokens(video, weights_.video_time, weights_.video_modality_embedding);
    tokens = append_sequence(tokens, temporal_states, weights_.video_modality_embedding);
    auto mask = MosaicOmniModalFrontends::full_mask(tokens);
    return VideoPrepared{std::move(tokens), std::move(mask), std::move(temporal_states),
        std::move(temporal_summary), std::move(evidence_tokens), std::move(temporal_delta_value),
        std::move(temporal_delta_world), std::move(object_slots), std::move(normalized_slots),
        std::move(object_world_slots), std::move(binding), std::move(identity), std::move(event),
        std::move(delta), std::move(geometry), std::move(attention_output), std::move(dense_features),
        std::move(x_moment), std::move(y_moment), std::move(xy_features), std::move(camera_stats),
        std::move(camera_states), std::move(camera_summary), std::move(action_summary),
        std::move(ego_summary), std::move(world_summary)};
}

namespace {

Tensor world_last(const Tensor& world) {
    const auto shape = world.shape();
    if (shape.size() != 3 || shape[1] == 0) invalid("post_world must be [B,S,D]");
    const auto batch = size_of(shape[0], "world batch");
    const auto slots = size_of(shape[1], "world slots");
    const auto dim = size_of(shape[2], "world dimension");
    std::vector<double> values(batch * dim);
    for (std::size_t b = 0; b < batch; ++b)
        std::copy_n(world.values().begin() + (b * slots + slots - 1) * dim, dim,
                    values.begin() + b * dim);
    return tensor_like(world, shape_of({batch, dim}), std::move(values));
}

Tensor world_object_decision_input(const Tensor& world, const std::size_t object_slots) {
    const auto shape = world.shape();
    const auto batch = size_of(shape[0], "world batch");
    const auto slots = size_of(shape[1], "world slots");
    const auto dim = size_of(shape[2], "world dimension");
    if (object_slots > slots) invalid("object slots exceed post_world slots");
    std::vector<double> values(batch * (object_slots + 1) * dim);
    for (std::size_t b = 0; b < batch; ++b) {
        std::copy_n(world.values().begin() + b * slots * dim, object_slots * dim,
                    values.begin() + b * (object_slots + 1) * dim);
        std::copy_n(world.values().begin() + (b * slots + slots - 1) * dim, dim,
                    values.begin() + (b * (object_slots + 1) + object_slots) * dim);
    }
    return tensor_like(world, shape_of({batch, (object_slots + 1) * dim}), std::move(values));
}

Tensor append_query_to_slots(const Tensor& slots, const Tensor& query) {
    const auto shape = slots.shape();
    const auto batch = size_of(shape[0], "slot batch");
    const auto count = size_of(shape[1], "slot count");
    const auto slot_width = size_of(shape[2], "slot dimension");
    if (query.shape().size() != 2 || query.shape()[0] != batch)
        invalid("query shape mismatch");
    const auto query_width = size_of(query.shape()[1], "query dimension");
    same_storage(slots, query, "slot query");
    const auto output_width = slot_width + query_width;
    std::vector<double> values(batch * count * output_width);
    for (std::size_t b = 0; b < batch; ++b)
        for (std::size_t slot = 0; slot < count; ++slot) {
            std::copy_n(slots.values().begin() + (b * count + slot) * slot_width, slot_width,
                        values.begin() + (b * count + slot) * output_width);
            std::copy_n(query.values().begin() + b * query_width, query_width,
                        values.begin() + (b * count + slot) * output_width + slot_width);
        }
    return tensor_like(slots, shape_of({batch, count, output_width}), std::move(values));
}

Tensor flatten_slots_with_query(const Tensor& slots, const Tensor& query) {
    const auto shape = slots.shape();
    const auto batch = size_of(shape[0], "slot batch");
    const auto count = size_of(shape[1], "slot count");
    const auto dim = size_of(shape[2], "slot dimension");
    auto flat = tensor_like(slots, shape_of({batch, count * dim}),
                            {slots.values().begin(), slots.values().end()});
    return concatenate_last({&flat, &query});
}

Tensor weighted_pair(const Tensor& raw, const Tensor& normalized, const Tensor& weights) {
    same_storage(raw, normalized, "weighted pair");
    same_storage(raw, weights, "weighted pair");
    const auto shape = raw.shape();
    if (shape.size() != 2 || normalized.shape().size() != 2 ||
        !std::equal(shape.begin(), shape.end(), normalized.shape().begin()) ||
        weights.shape().size() != 2 || weights.shape()[0] != shape[0] || weights.shape()[1] != 2)
        invalid("weighted pair shape mismatch");
    const auto batch = size_of(shape[0], "weighted pair batch");
    const auto dim = size_of(shape[1], "weighted pair dimension");
    std::vector<double> values(batch * dim);
    for (std::size_t b = 0; b < batch; ++b)
        for (std::size_t d = 0; d < dim; ++d)
            values[b * dim + d] = raw.values()[b * dim + d] * weights.values()[b * 2] +
                                  normalized.values()[b * dim + d] * weights.values()[b * 2 + 1];
    return tensor_like(raw, shape_of({batch, dim}), std::move(values));
}

Tensor gated_moment(const Tensor& moment, const Tensor& query,
                    const ModalLinearWeights& gate) {
    auto factors = linear(query, gate, size_of(moment.shape()[1], "moment width"));
    std::vector<double> values(moment.values().size());
    for (std::size_t i = 0; i < values.size(); ++i)
        values[i] = moment.values()[i] * (1.0 + std::tanh(factors.values()[i]));
    return tensor_like(moment, {moment.shape().begin(), moment.shape().end()}, std::move(values));
}

Tensor query_condition(const Tensor& state, const Tensor& query,
                       const ModalLinearWeights& weights) {
    const auto dim = size_of(state.shape()[1], "decision dimension");
    const auto parameters = linear(query, weights, 2 * dim);
    std::vector<double> values(state.values().size());
    for (std::size_t b = 0; b < size_of(state.shape()[0], "decision batch"); ++b)
        for (std::size_t d = 0; d < dim; ++d)
            values[b * dim + d] = state.values()[b * dim + d] *
                (1.0 + std::tanh(parameters.values()[b * 2 * dim + d])) +
                parameters.values()[b * 2 * dim + dim + d];
    return tensor_like(state, {state.shape().begin(), state.shape().end()}, std::move(values));
}

Tensor relation_geometry(const Tensor& attention) {
    const auto paths = object_attention_trajectory(attention);
    const auto shape = paths.shape();
    if (shape.size() != 3 || shape[1] != 2 || shape[2] != 12)
        invalid("descriptor relation paths must be [B,2,12]");
    const auto batch = size_of(shape[0], "relation batch");
    std::vector<double> values(batch * 48);
    for (std::size_t b = 0; b < batch; ++b)
        for (std::size_t d = 0; d < 12; ++d) {
            const auto first = paths.values()[(b * 2) * 12 + d];
            const auto second = paths.values()[(b * 2 + 1) * 12 + d];
            values[b * 48 + d] = first;
            values[b * 48 + 12 + d] = second;
            values[b * 48 + 24 + d] = first - second;
            values[b * 48 + 36 + d] = first * second;
        }
    return tensor_like(paths, shape_of({batch, 48}), std::move(values));
}

Tensor zeros_like_rows(const Tensor& reference, const std::size_t width) {
    return tensor_like(reference, shape_of({size_of(reference.shape()[0], "zero batch"), width}),
                       std::vector<double>(size_of(reference.shape()[0], "zero batch") * width));
}

Tensor teacher_embedding(const Tensor& summary, const std::optional<Tensor>& delta,
                         const ModalLinearWeights& weights, const std::size_t output) {
    auto result = linear(summary, weights, output);
    if (!delta) return result;
    const auto midpoint = output / 2;
    const auto dim = size_of(delta->shape()[1], "teacher input dimension");
    std::vector<double> values(result.values().begin(), result.values().end());
    const auto batch = size_of(delta->shape()[0], "teacher batch");
    for (std::size_t b = 0; b < batch; ++b)
        for (std::size_t out = midpoint; out < output; ++out) {
            double value = 0.0;
            for (std::size_t in = 0; in < dim; ++in)
                value += delta->values()[b * dim + in] * weights.weight[out * dim + in];
            values[b * output + out] = value;
        }
    return tensor_like(result, {result.shape().begin(), result.shape().end()}, std::move(values));
}

}  // namespace

VideoPipelineOutput MosaicOmniVideoPipeline::decide(
    const VideoPipelineInput& source, const VideoPrepared& prepared,
    const VideoDecisionInput& decision) const {
    const auto world_shape = decision.post_world.shape();
    if (world_shape.size() != 3 || world_shape[1] != config_.world_slots ||
        world_shape[2] != config_.world_dim)
        invalid("post_world must match configured [B,S,D]");
    const auto batch = size_of(world_shape[0], "decision batch");
    same_storage(source.video_values, decision.post_world, "video decision");
    if (decision.text_query) {
        const auto shape = decision.text_query->shape();
        if (shape.size() != 2 || shape[0] != batch || shape[1] != config_.world_dim)
            invalid("text query must be [B,D]");
        same_storage(decision.post_world, *decision.text_query, "text query");
    }
    if (decision.descriptor_queries) {
        const auto shape = decision.descriptor_queries->shape();
        if (shape.size() != 3 || shape[0] != batch || shape[1] != 2 ||
            shape[2] != config_.world_dim)
            invalid("descriptor queries must be [B,2,D]");
        same_storage(decision.post_world, *decision.descriptor_queries, "descriptor queries");
    }

    auto state = world_last(decision.post_world);
    if (prepared.temporal_delta_world) state = add(state, *prepared.temporal_delta_world);
    std::optional<Tensor> evidence_weights;
    if (prepared.object_slots) {
        Tensor object_decision = state.clone();
        if (!prepared.normalized_object_slots) {
            object_decision = norm_linear(world_object_decision_input(decision.post_world,
                                                                       config_.object_slots),
                                          *weights_.object_decision);
        } else {
            if (!decision.text_query) invalid("dual object evidence requires a text query");
            const auto raw_world = linear(*prepared.object_slots, *weights_.object_to_world, config_.world_dim);
            const auto normalized_world = linear(*prepared.normalized_object_slots,
                                                 *weights_.object_to_world, config_.world_dim);
            Tensor raw_decision = state.clone(), normalized_decision = state.clone();
            if (!weights_.object_set_decision) {
                raw_decision = norm_linear(flatten_slots_with_query(raw_world, *decision.text_query),
                                           *weights_.object_decision);
                normalized_decision = norm_linear(flatten_slots_with_query(normalized_world, *decision.text_query),
                                                  *weights_.object_decision);
            } else {
                raw_decision = mean_axis_one(norm_linear(append_query_to_slots(raw_world, *decision.text_query),
                                                         *weights_.object_set_decision));
                normalized_decision = mean_axis_one(norm_linear(append_query_to_slots(normalized_world, *decision.text_query),
                                                                *weights_.object_set_decision));
            }
            evidence_weights = softmax_last(linear(*decision.text_query, *weights_.object_evidence_gate, 2));
            object_decision = weighted_pair(raw_decision, normalized_decision, *evidence_weights);
        }
        state = add(state, object_decision);
    }
    if (prepared.object_binding_features) {
        if (!decision.text_query) invalid("identity-event binding requires a text query");
        state = add(state, mean_axis_one(norm_linear(
            append_query_to_slots(*prepared.object_binding_features, *decision.text_query),
            *weights_.object_binding_decision)));
    }

    std::optional<Tensor> trajectory_weights, pair_weights;
    if (prepared.trajectory_identity && modules_.trajectory_binding) {
        if (!decision.text_query) invalid("object trajectory binding requires a text query");
        auto result = modules_.trajectory_binding->forward(
            *prepared.trajectory_identity, *prepared.trajectory_event,
            *prepared.trajectory_delta, *prepared.trajectory_geometry,
            *decision.text_query);
        state = add(state, result.decision);
        trajectory_weights = std::move(result.weights);
    }
    if (prepared.trajectory_identity && modules_.pair_trajectory_binding &&
        decision.descriptor_queries) {
        auto result = modules_.pair_trajectory_binding->forward(
            *prepared.trajectory_identity, *prepared.trajectory_event,
            *prepared.trajectory_delta, *prepared.trajectory_geometry,
            *decision.descriptor_queries);
        state = add(state, result.decision);
        pair_weights = std::move(result.weights);
    }

    std::optional<Tensor> descriptor_attention, visibility, margin, reliability;
    if (!prepared.dense_object_features.empty() && modules_.descriptor_binding &&
        decision.descriptor_queries) {
        std::optional<Tensor> memory_scale;
        if (config_.descriptor_memory_evidence_routing) {
            if (!evidence_weights || evidence_weights->shape()[1] != prepared.dense_object_features.size())
                invalid("descriptor memory routing requires matched evidence weights");
            memory_scale = normalized_evidence_preference(
                *evidence_weights, config_.descriptor_memory_evidence_routing_margin);
        }
        std::vector<Tensor> decisions, attentions, visibilities, margins, reliabilities;
        for (const auto& features : prepared.dense_object_features) {
            auto result = modules_.descriptor_binding->forward(
                features, *decision.descriptor_queries,
                decision.text_query ? &*decision.text_query : nullptr,
                memory_scale ? &*memory_scale : nullptr);
            decisions.push_back(std::move(result.decision));
            attentions.push_back(std::move(result.attention));
            if (result.visibility_logits) visibilities.push_back(std::move(*result.visibility_logits));
            if (result.memory_margin) margins.push_back(std::move(*result.memory_margin));
            if (result.memory_reliability_logits) reliabilities.push_back(std::move(*result.memory_reliability_logits));
        }
        state = add(state, average_tensors(decisions));
        descriptor_attention = average_tensors(attentions);
        if (!visibilities.empty()) visibility = average_tensors(visibilities);
        if (!margins.empty()) margin = average_tensors(margins);
        if (!reliabilities.empty()) reliability = average_tensors(reliabilities);
    }

    if (prepared.spatial_moment) {
        auto moment = prepared.spatial_moment->clone();
        if (weights_.spatial_moment_gate) {
            if (!decision.text_query) invalid("query spatial-temporal moment requires a text query");
            moment = gated_moment(moment, *decision.text_query, *weights_.spatial_moment_gate);
        }
        state = add(state, linear(moment, *weights_.spatial_moment_to_world,
                                  config_.world_dim, false));
    }
    if (prepared.spatial_y_moment) {
        if (!decision.text_query) invalid("y spatial-temporal moment requires a text query");
        auto moment = gated_moment(*prepared.spatial_y_moment, *decision.text_query,
                                   *weights_.spatial_y_moment_gate);
        state = add(state, linear(moment, *weights_.spatial_y_moment_to_world,
                                  config_.world_dim, false));
    }
    if (weights_.query_conditioning) {
        if (!decision.text_query) invalid("query-conditioned video requires a text query");
        state = query_condition(state, *decision.text_query, *weights_.query_conditioning);
    }

    std::optional<Tensor> teacher;
    if (weights_.teacher_projection)
        teacher = teacher_embedding(prepared.world_summary, prepared.temporal_delta_world,
                                    *weights_.teacher_projection, config_.vision_teacher_dim);
    auto order = linear(layer_norm(state, weights_.order_head.norm_weight,
                                   weights_.order_head.norm_bias),
                        weights_.order_head.linear, 2);

    std::optional<Tensor> relation_logits;
    if (weights_.spatial_relation_head) {
        std::optional<Tensor> geometry, spatial_reasoning;
        if (descriptor_attention) {
            geometry = relation_geometry(*descriptor_attention);
            if (modules_.spatial_geometry && source.camera_pose_values)
                spatial_reasoning = modules_.spatial_geometry->forward(
                    *descriptor_attention, *source.camera_pose_values);
        }
        auto camera = prepared.camera_pose_summary
            ? prepared.camera_pose_summary->clone() : zeros_like_rows(state, config_.world_dim);
        auto relation = geometry ? geometry->clone() : zeros_like_rows(state, 48);
        auto reasoning = spatial_reasoning ? spatial_reasoning->clone() :
            zeros_like_rows(state, modules_.spatial_geometry ? VideoSpatialGeometryReasoner::output_dim : 0);
        auto input = concatenate_last({&state, &camera, &relation, &reasoning});
        relation_logits = norm_mlp(input, *weights_.spatial_relation_head);
    }

    std::optional<Tensor> ego_logits, ego_validity, motion_evidence;
    std::optional<BooleanMask> sufficient;
    if (weights_.egomotion_head && prepared.action_summary && prepared.egomotion_summary) {
        auto input = concatenate_last({&state, &*prepared.action_summary, &*prepared.egomotion_summary});
        ego_logits = norm_mlp(input, *weights_.egomotion_head);
    }
    std::optional<Tensor> validity_statistics;
    if (prepared.egomotion_summary &&
        (weights_.egomotion_validity_head_weights || config_.egomotion_evidence_gate))
        validity_statistics = video_egomotion_validity_statistics(source.video_values);
    if (weights_.egomotion_validity_head_weights) {
        auto input = concatenate_last({&*prepared.egomotion_summary, &*validity_statistics});
        ego_validity = norm_mlp(input, *weights_.egomotion_validity_head_weights);
    }
    if (config_.egomotion_evidence_gate) {
        std::vector<double> values(batch);
        std::vector<std::uint8_t> mask(batch);
        for (std::size_t b = 0; b < batch; ++b) {
            values[b] = validity_statistics->values()[b * 2 + 1];
            mask[b] = static_cast<std::uint8_t>(values[b] > config_.egomotion_minimum_motion_evidence);
        }
        motion_evidence = tensor_like(*validity_statistics, shape_of({batch}), std::move(values));
        sufficient.emplace(shape_of({batch}), std::move(mask));
    }

    std::optional<Tensor> robustness;
    if (config_.camera_robustness_adapter) {
        if (!prepared.camera_statistics) invalid("camera robustness statistics are unavailable");
        robustness = weights_.camera_robustness_gate_nonlinear
            ? sigmoid(mlp(*prepared.camera_statistics, *weights_.camera_robustness_gate_nonlinear))
            : sigmoid(linear(*prepared.camera_statistics, *weights_.camera_robustness_gate_linear, 1));
        auto residual = linear(layer_norm(state, weights_.order_head.norm_weight,
                                          weights_.order_head.norm_bias),
                               *weights_.camera_robustness_head, 2, false);
        std::vector<double> values(order.values().begin(), order.values().end());
        for (std::size_t b = 0; b < batch; ++b)
            for (std::size_t i = 0; i < 2; ++i)
                values[b * 2 + i] += robustness->values()[b] * residual.values()[b * 2 + i];
        order = tensor_like(order, {order.shape().begin(), order.shape().end()}, std::move(values));
    }
    if (weights_.spatial_logit_head) {
        if (!decision.text_query || !prepared.spatial_features)
            invalid("spatial-temporal logit head requires query and xy moments");
        order = add(order, linear(concatenate_last({&*decision.text_query,
                                                    &*prepared.spatial_features}),
                                  *weights_.spatial_logit_head, 2));
    }
    if (weights_.spatial_bilinear_head) {
        if (!decision.text_query || !prepared.spatial_features)
            invalid("spatial-temporal bilinear head requires query and xy moments");
        const auto projection = linear(*decision.text_query, *weights_.spatial_bilinear_head, 4);
        std::vector<double> values(order.values().begin(), order.values().end());
        for (std::size_t b = 0; b < batch; ++b) {
            double score = 0.0;
            for (std::size_t i = 0; i < 4; ++i)
                score += projection.values()[b * 4 + i] * prepared.spatial_features->values()[b * 4 + i];
            values[b * 2] -= score;
            values[b * 2 + 1] += score;
        }
        order = tensor_like(order, {order.shape().begin(), order.shape().end()}, std::move(values));
    }

    return VideoPipelineOutput{std::move(state), std::move(order), std::move(evidence_weights),
        prepared.object_attention ? std::optional<Tensor>(prepared.object_attention->clone()) : std::nullopt,
        std::move(trajectory_weights), std::move(pair_weights), std::move(descriptor_attention),
        std::move(visibility), std::move(margin), std::move(reliability), std::move(robustness),
        std::move(relation_logits), std::move(ego_logits), std::move(ego_validity),
        std::move(motion_evidence), std::move(sufficient), std::move(teacher)};
}

}  // namespace swegca::world
