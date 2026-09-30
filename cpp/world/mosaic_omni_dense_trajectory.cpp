#include "world/mosaic_omni_dense_trajectory.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace swegca::world {
namespace {

constexpr double layer_norm_epsilon = 1.0e-5;

[[nodiscard]] std::size_t checked_product(const std::size_t left,
                                          const std::size_t right,
                                          const char* const label) {
    if (left != 0 && right > std::numeric_limits<std::size_t>::max() / left) {
        throw std::overflow_error(std::string(label) + " size overflow");
    }
    return left * right;
}

[[nodiscard]] std::size_t checked_sum(const std::size_t left,
                                      const std::size_t right,
                                      const char* const label) {
    if (right > std::numeric_limits<std::size_t>::max() - left) {
        throw std::overflow_error(std::string(label) + " size overflow");
    }
    return left + right;
}

[[nodiscard]] std::size_t trajectory_input_width(const std::size_t dim) {
    return checked_sum(checked_product(dim, std::size_t{3}, "trajectory input"),
                       DescriptorConditionedDenseTrajectoryBinding::trajectory_width,
                       "trajectory input");
}

void require_size(const std::vector<double>& values, const std::size_t expected,
                  const char* const name) {
    if (values.size() != expected) {
        throw std::invalid_argument(std::string(name) + " has an invalid shape");
    }
}

void require_empty(const std::vector<double>& values, const char* const name) {
    if (!values.empty()) {
        throw std::invalid_argument(std::string(name) +
                                    " must be absent for this configuration");
    }
}

[[nodiscard]] std::vector<double> ones(const std::size_t count) {
    return std::vector<double>(count, 1.0);
}

[[nodiscard]] std::vector<double> zeros(const std::size_t count) {
    return std::vector<double>(count, 0.0);
}

[[nodiscard]] std::vector<double> identity(const std::size_t dimension) {
    std::vector<double> result(checked_product(dimension, dimension, "identity"));
    for (std::size_t index = 0; index != dimension; ++index) {
        result[index * dimension + index] = 1.0;
    }
    return result;
}

[[nodiscard]] Tensor make_tensor(const Tensor& like,
                                 std::vector<std::uint64_t> shape,
                                 std::vector<double> values) {
    return Tensor(like.dtype(), std::move(shape), std::move(values),
                  std::string(like.device()));
}

void require_cpu_compatible(const Tensor& left, const Tensor& right,
                            const char* const label) {
    if (left.device() != "cpu" || right.device() != "cpu" ||
        left.dtype() != right.dtype()) {
        throw std::invalid_argument(std::string(label) +
                                    " must be CPU tensors with one dtype");
    }
}

[[nodiscard]] std::vector<double> layer_norm(
    const std::vector<double>& input, const std::size_t rows,
    const std::size_t width, const std::vector<double>& weight,
    const std::vector<double>& bias) {
    std::vector<double> output(input.size());
    for (std::size_t row = 0; row != rows; ++row) {
        const auto offset = row * width;
        double mean = 0.0;
        for (std::size_t column = 0; column != width; ++column) {
            mean += input[offset + column];
        }
        mean /= static_cast<double>(width);
        double variance = 0.0;
        for (std::size_t column = 0; column != width; ++column) {
            const auto difference = input[offset + column] - mean;
            variance += difference * difference;
        }
        variance /= static_cast<double>(width);
        const auto inverse = 1.0 / std::sqrt(variance + layer_norm_epsilon);
        for (std::size_t column = 0; column != width; ++column) {
            output[offset + column] =
                (input[offset + column] - mean) * inverse * weight[column] +
                bias[column];
        }
    }
    return output;
}

[[nodiscard]] std::vector<double> plain_layer_norm(
    const std::vector<double>& input, const std::size_t rows,
    const std::size_t width) {
    return layer_norm(input, rows, width, ones(width), zeros(width));
}

[[nodiscard]] std::vector<double> linear(
    const std::vector<double>& input, const std::size_t rows,
    const std::size_t input_width, const std::size_t output_width,
    const std::vector<double>& weight,
    const std::vector<double>* const bias = nullptr) {
    std::vector<double> output(checked_product(rows, output_width, "linear output"));
    for (std::size_t row = 0; row != rows; ++row) {
        for (std::size_t out = 0; out != output_width; ++out) {
            double value = bias == nullptr ? 0.0 : (*bias)[out];
            for (std::size_t in = 0; in != input_width; ++in) {
                value += input[row * input_width + in] *
                         weight[out * input_width + in];
            }
            output[row * output_width + out] = value;
        }
    }
    return output;
}

void softmax_spatial(std::vector<double>& values, const std::size_t rows,
                     const std::size_t pixels) {
    for (std::size_t row = 0; row != rows; ++row) {
        auto begin = values.begin() + static_cast<std::ptrdiff_t>(row * pixels);
        auto end = begin + static_cast<std::ptrdiff_t>(pixels);
        const auto maximum = *std::max_element(begin, end);
        double total = 0.0;
        for (auto iterator = begin; iterator != end; ++iterator) {
            *iterator = std::exp(*iterator - maximum);
            total += *iterator;
        }
        for (auto iterator = begin; iterator != end; ++iterator) *iterator /= total;
    }
}

[[nodiscard]] double sigmoid(const double value) {
    if (value >= 0.0) return 1.0 / (1.0 + std::exp(-value));
    const auto exponential = std::exp(value);
    return exponential / (1.0 + exponential);
}

[[nodiscard]] double gelu(const double value) {
    return 0.5 * value * (1.0 + std::erf(value / std::sqrt(2.0)));
}

[[nodiscard]] std::vector<double> endpoint_times(const std::size_t frames) {
    if (frames == 1) return {-1.0};
    std::vector<double> result(frames);
    for (std::size_t frame = 0; frame != frames; ++frame) {
        result[frame] = -1.0 + 2.0 * static_cast<double>(frame) /
                                  static_cast<double>(frames - 1);
    }
    return result;
}

}  // namespace

void DescriptorConditionedDenseTrajectoryConfig::validate() const {
    if (dim == 0) {
        throw std::invalid_argument("descriptor trajectory dimension must be positive");
    }
    if (object_memory_contrast_readout &&
        !(object_memory && object_memory_contrast_visibility)) {
        throw std::invalid_argument("contrast readout requires contrast object memory");
    }
    if (object_memory_temporal_relative_readout && !object_memory) {
        throw std::invalid_argument("temporal relative readout requires object memory");
    }
    if (object_memory_temporal_relative_readout &&
        object_memory_temporal_relative_visibility) {
        throw std::invalid_argument(
            "temporal relative readout cannot replace the parent visibility path");
    }
}

DescriptorConditionedDenseTrajectoryWeights
DescriptorConditionedDenseTrajectoryWeights::initialize(
    const DescriptorConditionedDenseTrajectoryConfig& config) {
    config.validate();
    const auto dim = config.dim;
    const auto trajectory_width = trajectory_input_width(dim);
    std::vector<double> value_weight(
        checked_product(dim, trajectory_width, "value.weight"), 0.0);
    for (std::size_t index = 0; index != dim; ++index) {
        value_weight[index * trajectory_width + dim + index] = 1.0;
    }
    const auto visibility_width = config.object_memory_contrast_visibility
                                      ? DescriptorConditionedDenseTrajectoryBinding::
                                            object_memory_contrast_visibility_width
                                      : DescriptorConditionedDenseTrajectoryBinding::
                                            object_memory_visibility_width;
    return {
        ones(dim), zeros(dim), identity(dim), identity(dim),
        ones(trajectory_width), zeros(trajectory_width), std::move(value_weight),
        zeros(checked_product(dim, checked_product(dim, std::size_t{4},
                                                  "output width"),
                              "output.weight")),
        config.object_memory ? zeros(visibility_width) : std::vector<double>{},
        config.object_memory ? zeros(1) : std::vector<double>{},
        config.object_memory
            ? zeros(checked_product(dim,
                                    DescriptorConditionedDenseTrajectoryBinding::
                                        object_memory_width,
                                    "memory_output.weight"))
            : std::vector<double>{},
        config.object_memory_contrast_readout
            ? zeros(checked_product(dim,
                                    DescriptorConditionedDenseTrajectoryBinding::
                                        object_memory_contrast_summary_width,
                                    "memory_contrast_output.weight"))
            : std::vector<double>{},
        config.object_memory_temporal_relative_readout
            ? zeros(checked_product(dim,
                                    DescriptorConditionedDenseTrajectoryBinding::
                                        object_memory_width,
                                    "memory_temporal_relative_output.weight"))
            : std::vector<double>{},
        config.object_memory && config.object_memory_query_gate ? zeros(dim)
                                                                : std::vector<double>{},
        config.object_memory && config.object_memory_query_gate ? zeros(1)
                                                                : std::vector<double>{},
        config.object_memory && config.object_memory_reliability_gate
            ? zeros(DescriptorConditionedDenseTrajectoryBinding::object_memory_width)
            : std::vector<double>{},
        config.object_memory && config.object_memory_reliability_gate
            ? zeros(1)
            : std::vector<double>{}};
}

void DescriptorConditionedDenseTrajectoryWeights::validate(
    const DescriptorConditionedDenseTrajectoryConfig& config) const {
    config.validate();
    const auto dim = config.dim;
    const auto trajectory_width = trajectory_input_width(dim);
    const auto output_width = checked_product(dim, std::size_t{4}, "output width");
    const auto visibility_width = config.object_memory_contrast_visibility
                                      ? DescriptorConditionedDenseTrajectoryBinding::
                                            object_memory_contrast_visibility_width
                                      : DescriptorConditionedDenseTrajectoryBinding::
                                            object_memory_visibility_width;
    require_size(feature_norm_weight, dim, "feature_norm.weight");
    require_size(feature_norm_bias, dim, "feature_norm.bias");
    require_size(query_weight, checked_product(dim, dim, "query.weight"),
                 "query.weight");
    require_size(key_weight, checked_product(dim, dim, "key.weight"),
                 "key.weight");
    require_size(trajectory_norm_weight, trajectory_width,
                 "trajectory_norm.weight");
    require_size(trajectory_norm_bias, trajectory_width,
                 "trajectory_norm.bias");
    require_size(value_weight, checked_product(dim, trajectory_width, "value.weight"),
                 "value.weight");
    require_size(output_weight, checked_product(dim, output_width, "output.weight"),
                 "output.weight");
    if (config.object_memory) {
        require_size(memory_visibility_weight, visibility_width,
                     "memory_visibility.weight");
        require_size(memory_visibility_bias, 1, "memory_visibility.bias");
        require_size(memory_output_weight,
                     checked_product(dim,
                                     DescriptorConditionedDenseTrajectoryBinding::
                                         object_memory_width,
                                     "memory_output.weight"),
                     "memory_output.weight");
    } else {
        require_empty(memory_visibility_weight, "memory_visibility.weight");
        require_empty(memory_visibility_bias, "memory_visibility.bias");
        require_empty(memory_output_weight, "memory_output.weight");
    }
    if (config.object_memory_contrast_readout) {
        require_size(memory_contrast_output_weight,
                     checked_product(dim,
                                     DescriptorConditionedDenseTrajectoryBinding::
                                         object_memory_contrast_summary_width,
                                     "memory_contrast_output.weight"),
                     "memory_contrast_output.weight");
    } else {
        require_empty(memory_contrast_output_weight,
                      "memory_contrast_output.weight");
    }
    if (config.object_memory_temporal_relative_readout) {
        require_size(memory_temporal_relative_output_weight,
                     checked_product(dim,
                                     DescriptorConditionedDenseTrajectoryBinding::
                                         object_memory_width,
                                     "memory_temporal_relative_output.weight"),
                     "memory_temporal_relative_output.weight");
    } else {
        require_empty(memory_temporal_relative_output_weight,
                      "memory_temporal_relative_output.weight");
    }
    if (config.object_memory && config.object_memory_query_gate) {
        require_size(memory_gate_weight, dim, "memory_gate.weight");
        require_size(memory_gate_bias, 1, "memory_gate.bias");
    } else {
        require_empty(memory_gate_weight, "memory_gate.weight");
        require_empty(memory_gate_bias, "memory_gate.bias");
    }
    if (config.object_memory && config.object_memory_reliability_gate) {
        require_size(memory_reliability_gate_weight,
                     DescriptorConditionedDenseTrajectoryBinding::object_memory_width,
                     "memory_reliability_gate.weight");
        require_size(memory_reliability_gate_bias, 1,
                     "memory_reliability_gate.bias");
    } else {
        require_empty(memory_reliability_gate_weight,
                      "memory_reliability_gate.weight");
        require_empty(memory_reliability_gate_bias,
                      "memory_reliability_gate.bias");
    }
}

DescriptorConditionedDenseTrajectoryBinding::
    DescriptorConditionedDenseTrajectoryBinding(
        DescriptorConditionedDenseTrajectoryConfig config)
    : DescriptorConditionedDenseTrajectoryBinding(
          config, DescriptorConditionedDenseTrajectoryWeights::initialize(config)) {}

DescriptorConditionedDenseTrajectoryBinding::
    DescriptorConditionedDenseTrajectoryBinding(
        DescriptorConditionedDenseTrajectoryConfig config,
        DescriptorConditionedDenseTrajectoryWeights weights)
    : config_(std::move(config)), weights_(std::move(weights)) {
    config_.validate();
    weights_.validate(config_);
}

DescriptorConditionedDenseTrajectoryOutput
DescriptorConditionedDenseTrajectoryBinding::forward(
    const Tensor& features, const Tensor& queries, const Tensor* const condition,
    const Tensor* const memory_scale) const {
    const auto feature_shape = features.shape();
    if (feature_shape.size() != 5 || feature_shape[2] != config_.dim) {
        throw std::invalid_argument("dense video features must be [B,F,D,H,W]");
    }
    if (features.device() != "cpu") {
        throw std::invalid_argument("dense trajectory binding requires CPU tensors");
    }
    const auto batch = static_cast<std::size_t>(feature_shape[0]);
    const auto frames = static_cast<std::size_t>(feature_shape[1]);
    const auto dim = config_.dim;
    const auto height = static_cast<std::size_t>(feature_shape[3]);
    const auto width = static_cast<std::size_t>(feature_shape[4]);
    if (frames == 0 || height == 0 || width == 0) {
        throw std::invalid_argument("dense video frames and spatial grid must be nonempty");
    }
    const auto pixels = checked_product(height, width, "dense spatial grid");
    const auto query_shape = queries.shape();
    if (query_shape.size() != 3 || query_shape[0] != feature_shape[0] ||
        query_shape[1] != 2 || query_shape[2] != dim) {
        throw std::invalid_argument("descriptor queries must be [B,2,D]");
    }
    require_cpu_compatible(features, queries, "dense trajectory inputs");
    if (memory_scale != nullptr) {
        const auto scale_shape = memory_scale->shape();
        if (scale_shape.size() != 1 || scale_shape[0] != feature_shape[0]) {
            throw std::invalid_argument("descriptor memory scale must be [B]");
        }
        if (memory_scale->device() != features.device()) {
            throw std::invalid_argument(
                "descriptor memory scale must share the feature device");
        }
    }

    // [B,F,D,H,W] -> [B,F,H,W,D]
    std::vector<double> tokens(features.values().size());
    for (std::size_t item = 0; item != batch; ++item) {
        for (std::size_t frame = 0; frame != frames; ++frame) {
            for (std::size_t component = 0; component != dim; ++component) {
                for (std::size_t pixel = 0; pixel != pixels; ++pixel) {
                    tokens[((item * frames + frame) * pixels + pixel) * dim + component] =
                        features.values()[((item * frames + frame) * dim + component) *
                                              pixels +
                                          pixel];
                }
            }
        }
    }
    std::vector<double> query_values(queries.values().begin(), queries.values().end());
    if (config_.pair_centered_queries) {
        for (std::size_t item = 0; item != batch; ++item) {
            for (std::size_t component = 0; component != dim; ++component) {
                const auto mean =
                    0.5 * (query_values[(item * 2) * dim + component] +
                           query_values[(item * 2 + 1) * dim + component]);
                query_values[(item * 2) * dim + component] -= mean;
                query_values[(item * 2 + 1) * dim + component] -= mean;
            }
        }
        query_values = plain_layer_norm(query_values, batch * 2, dim);
    }
    const auto projected_queries =
        linear(query_values, batch * 2, dim, dim, weights_.query_weight);
    auto normalized_tokens = layer_norm(tokens, batch * frames * pixels, dim,
                                        weights_.feature_norm_weight,
                                        weights_.feature_norm_bias);
    const auto projected_keys =
        linear(normalized_tokens, batch * frames * pixels, dim, dim,
               weights_.key_weight);
    std::vector<double> logits(batch * 2 * frames * pixels);
    const auto attention_scale = std::sqrt(static_cast<double>(dim));
    for (std::size_t item = 0; item != batch; ++item) {
        for (std::size_t role = 0; role != 2; ++role) {
            for (std::size_t frame = 0; frame != frames; ++frame) {
                for (std::size_t pixel = 0; pixel != pixels; ++pixel) {
                    double score = 0.0;
                    for (std::size_t component = 0; component != dim; ++component) {
                        score += projected_queries[(item * 2 + role) * dim + component] *
                                 projected_keys[((item * frames + frame) * pixels +
                                                 pixel) *
                                                    dim +
                                                component];
                    }
                    logits[((item * 2 + role) * frames + frame) * pixels + pixel] =
                        score / attention_scale;
                }
            }
        }
    }
    auto attention_values = logits;
    softmax_spatial(attention_values, batch * 2 * frames, pixels);
    const auto attention = make_tensor(
        features,
        {feature_shape[0], 2, feature_shape[1], feature_shape[3], feature_shape[4]},
        attention_values);

    std::optional<Tensor> visibility_logits;
    std::optional<Tensor> memory_margin;
    std::optional<Tensor> memory_decision;
    std::optional<Tensor> memory_reliability_logits;
    if (config_.object_memory) {
        std::vector<double> visibility_features;
        const auto visibility_width = config_.object_memory_contrast_visibility
                                          ? object_memory_contrast_visibility_width
                                          : object_memory_visibility_width;
        visibility_features.resize(batch * 2 * frames * visibility_width);
        std::vector<double> cosine_peak(batch * 2 * frames);
        std::vector<double> cosine_margin(batch * 2 * frames);
        for (std::size_t item = 0; item != batch; ++item) {
            for (std::size_t role = 0; role != 2; ++role) {
                double query_norm = 0.0;
                for (std::size_t component = 0; component != dim; ++component) {
                    const auto value =
                        projected_queries[(item * 2 + role) * dim + component];
                    query_norm += value * value;
                }
                query_norm = std::max(std::sqrt(query_norm), 1.0e-12);
                for (std::size_t frame = 0; frame != frames; ++frame) {
                    const auto row = (item * 2 + role) * frames + frame;
                    double peak = 0.0;
                    double entropy = 0.0;
                    double cosine_first = -std::numeric_limits<double>::infinity();
                    double cosine_second = -std::numeric_limits<double>::infinity();
                    for (std::size_t pixel = 0; pixel != pixels; ++pixel) {
                        const auto probability = attention_values[row * pixels + pixel];
                        peak = std::max(peak, probability);
                        entropy -= probability * std::log(std::max(probability, 1.0e-6));
                        if (config_.object_memory_contrast_visibility) {
                            double key_norm = 0.0;
                            double cosine = 0.0;
                            const auto key_offset =
                                ((item * frames + frame) * pixels + pixel) * dim;
                            for (std::size_t component = 0; component != dim;
                                 ++component) {
                                const auto key = projected_keys[key_offset + component];
                                key_norm += key * key;
                                cosine +=
                                    projected_queries[(item * 2 + role) * dim +
                                                      component] *
                                    key;
                            }
                            cosine /= query_norm *
                                      std::max(std::sqrt(key_norm), 1.0e-12);
                            if (cosine > cosine_first) {
                                cosine_second = cosine_first;
                                cosine_first = cosine;
                            } else if (cosine > cosine_second) {
                                cosine_second = cosine;
                            }
                        }
                    }
                    const auto offset = row * visibility_width;
                    visibility_features[offset] = peak;
                    visibility_features[offset + 1] =
                        1.0 - entropy /
                                  std::log(static_cast<double>(
                                      std::max<std::size_t>(2, pixels)));
                    if (config_.object_memory_contrast_visibility) {
                        cosine_peak[row] = cosine_first;
                        cosine_margin[row] = pixels > 1 ? cosine_first - cosine_second : 0.0;
                        visibility_features[offset + 2] = cosine_peak[row];
                        visibility_features[offset + 3] = cosine_margin[row];
                    }
                }
            }
        }
        const auto raw_visibility = linear(
            visibility_features, batch * 2 * frames, visibility_width, 1,
            weights_.memory_visibility_weight, &weights_.memory_visibility_bias);
        visibility_logits = make_tensor(
            features, {feature_shape[0], 2, feature_shape[1]}, raw_visibility);
        const auto memory = descriptor_object_memory(
            attention, *visibility_logits,
            config_.object_memory_temporal_relative_visibility);
        memory_margin = memory.margin;
        auto decision_values = linear(
            std::vector<double>(memory.features.values().begin(),
                                memory.features.values().end()),
            batch, object_memory_width, dim, weights_.memory_output_weight);
        if (config_.object_memory_contrast_readout) {
            const auto peak_tensor = make_tensor(
                features, {feature_shape[0], 2, feature_shape[1]}, cosine_peak);
            const auto margin_tensor = make_tensor(
                features, {feature_shape[0], 2, feature_shape[1]}, cosine_margin);
            const auto summary = contrast_memory_summary(peak_tensor, margin_tensor);
            const auto contrast = linear(
                std::vector<double>(summary.values().begin(), summary.values().end()),
                batch, object_memory_contrast_summary_width, dim,
                weights_.memory_contrast_output_weight);
            for (std::size_t index = 0; index != decision_values.size(); ++index) {
                decision_values[index] += contrast[index];
            }
        }
        if (config_.object_memory_reliability_gate) {
            const auto reliability = linear(
                std::vector<double>(memory.features.values().begin(),
                                    memory.features.values().end()),
                batch, object_memory_width, 1,
                weights_.memory_reliability_gate_weight,
                &weights_.memory_reliability_gate_bias);
            memory_reliability_logits =
                make_tensor(features, {feature_shape[0]}, reliability);
            for (std::size_t item = 0; item != batch; ++item) {
                const auto gate = 2.0 * sigmoid(reliability[item]);
                for (std::size_t component = 0; component != dim; ++component) {
                    decision_values[item * dim + component] *= gate;
                }
            }
        }
        if (config_.object_memory_query_gate) {
            if (condition == nullptr || condition->shape().size() != 2 ||
                condition->shape()[0] != feature_shape[0] ||
                condition->shape()[1] != dim) {
                throw std::invalid_argument(
                    "descriptor memory query gate requires [B,D] condition");
            }
            require_cpu_compatible(features, *condition,
                                   "descriptor memory gate condition");
            const auto gate_logits = linear(
                std::vector<double>(condition->values().begin(),
                                    condition->values().end()),
                batch, dim, 1, weights_.memory_gate_weight,
                &weights_.memory_gate_bias);
            for (std::size_t item = 0; item != batch; ++item) {
                const auto gate = 2.0 * sigmoid(gate_logits[item]);
                for (std::size_t component = 0; component != dim; ++component) {
                    decision_values[item * dim + component] *= gate;
                }
            }
        }
        if (config_.object_memory_temporal_relative_readout) {
            const auto temporal =
                descriptor_object_memory(attention, *visibility_logits, true);
            memory_margin = temporal.margin;
            auto temporal_decision = linear(
                std::vector<double>(temporal.features.values().begin(),
                                    temporal.features.values().end()),
                batch, object_memory_width, dim,
                weights_.memory_temporal_relative_output_weight);
            if (memory_scale != nullptr) {
                for (std::size_t item = 0; item != batch; ++item) {
                    for (std::size_t component = 0; component != dim; ++component) {
                        temporal_decision[item * dim + component] *=
                            memory_scale->values()[item];
                    }
                }
            }
            for (std::size_t index = 0; index != decision_values.size(); ++index) {
                decision_values[index] += temporal_decision[index];
            }
        }
        memory_decision = make_tensor(
            features, {feature_shape[0], static_cast<std::uint64_t>(dim)},
            std::move(decision_values));
    }

    std::vector<double> selected(batch * 2 * frames * dim, 0.0);
    std::vector<double> match_logits(batch * 2 * frames,
                                     -std::numeric_limits<double>::infinity());
    for (std::size_t item = 0; item != batch; ++item) {
        for (std::size_t role = 0; role != 2; ++role) {
            for (std::size_t frame = 0; frame != frames; ++frame) {
                const auto row = (item * 2 + role) * frames + frame;
                for (std::size_t pixel = 0; pixel != pixels; ++pixel) {
                    const auto coefficient = attention_values[row * pixels + pixel];
                    match_logits[row] =
                        std::max(match_logits[row], logits[row * pixels + pixel]);
                    for (std::size_t component = 0; component != dim; ++component) {
                        selected[(row * dim) + component] +=
                            coefficient *
                            tokens[((item * frames + frame) * pixels + pixel) * dim +
                                   component];
                    }
                }
            }
        }
    }
    auto selected_tensor = make_tensor(
        features,
        {feature_shape[0], 2, feature_shape[1], static_cast<std::uint64_t>(dim)},
        std::move(selected));
    const auto match_tensor = make_tensor(
        features, {feature_shape[0], 2, feature_shape[1]}, match_logits);
    if (config_.persistent_identity_state) {
        selected_tensor = confidence_gated_sequence(selected_tensor, match_tensor);
    }
    const auto times = endpoint_times(frames);
    std::vector<double> identity_values(batch * 2 * dim);
    std::vector<double> event_values(batch * 2 * dim);
    std::vector<double> delta_values(batch * 2 * dim);
    for (std::size_t item = 0; item != batch; ++item) {
        for (std::size_t role = 0; role != 2; ++role) {
            const auto sequence = (item * 2 + role) * frames;
            for (std::size_t component = 0; component != dim; ++component) {
                double mean = 0.0;
                for (std::size_t frame = 0; frame != frames; ++frame) {
                    mean += selected_tensor.values()[(sequence + frame) * dim +
                                                     component];
                }
                mean /= static_cast<double>(frames);
                double event = 0.0;
                for (std::size_t frame = 0; frame != frames; ++frame) {
                    event += (selected_tensor.values()[(sequence + frame) * dim +
                                                       component] -
                              mean) *
                             times[frame];
                }
                event /= static_cast<double>(frames);
                const auto output = (item * 2 + role) * dim + component;
                const auto first = selected_tensor.values()[sequence * dim + component];
                const auto last = selected_tensor.values()[
                    (sequence + frames - 1) * dim + component];
                identity_values[output] = 0.5 * (first + last);
                event_values[output] = event;
                delta_values[output] = last - first;
            }
        }
    }
    const auto geometry = object_attention_trajectory(
        attention, config_.persistent_identity_state ? &match_tensor : nullptr);
    const auto trajectory_width = trajectory_input_width(dim);
    std::vector<double> combined(
        checked_product(checked_product(batch, std::size_t{2}, "trajectory rows"),
                        trajectory_width, "trajectory values"));
    for (std::size_t row = 0; row != batch * 2; ++row) {
        const auto target = row * trajectory_width;
        std::copy_n(identity_values.begin() + static_cast<std::ptrdiff_t>(row * dim),
                    dim, combined.begin() + static_cast<std::ptrdiff_t>(target));
        std::copy_n(event_values.begin() + static_cast<std::ptrdiff_t>(row * dim),
                    dim,
                    combined.begin() + static_cast<std::ptrdiff_t>(target + dim));
        std::copy_n(delta_values.begin() + static_cast<std::ptrdiff_t>(row * dim),
                    dim,
                    combined.begin() + static_cast<std::ptrdiff_t>(target + 2 * dim));
        std::copy_n(geometry.values().begin() +
                        static_cast<std::ptrdiff_t>(
                            row * DescriptorConditionedDenseTrajectoryBinding::
                                      trajectory_width),
                    DescriptorConditionedDenseTrajectoryBinding::trajectory_width,
                    combined.begin() + static_cast<std::ptrdiff_t>(target + 3 * dim));
    }
    combined = layer_norm(combined, batch * 2, trajectory_width,
                          weights_.trajectory_norm_weight,
                          weights_.trajectory_norm_bias);
    auto values = linear(combined, batch * 2, trajectory_width, dim,
                         weights_.value_weight);
    for (double& value : values) value = gelu(value);
    const auto output_width = checked_product(dim, std::size_t{4}, "output width");
    std::vector<double> output_input(batch * output_width);
    for (std::size_t item = 0; item != batch; ++item) {
        for (std::size_t component = 0; component != dim; ++component) {
            const auto first = values[(item * 2) * dim + component];
            const auto second = values[(item * 2 + 1) * dim + component];
            output_input[item * output_width + component] = first;
            output_input[item * output_width + dim + component] = second;
            output_input[item * output_width + 2 * dim + component] = first - second;
            output_input[item * output_width + 3 * dim + component] = first * second;
        }
    }
    auto decision_values =
        linear(output_input, batch, output_width, dim, weights_.output_weight);
    if (memory_decision.has_value()) {
        for (std::size_t item = 0; item != batch; ++item) {
            const auto external_scale =
                memory_scale != nullptr &&
                        !config_.object_memory_temporal_relative_readout
                    ? memory_scale->values()[item]
                    : 1.0;
            for (std::size_t component = 0; component != dim; ++component) {
                decision_values[item * dim + component] +=
                    config_.object_memory_scale * external_scale *
                    memory_decision->values()[item * dim + component];
            }
        }
    }
    return {
        make_tensor(features,
                    {feature_shape[0], static_cast<std::uint64_t>(dim)},
                    std::move(decision_values)),
        attention.clone(), std::move(visibility_logits), std::move(memory_margin),
        std::move(memory_reliability_logits)};
}

}  // namespace swegca::world
