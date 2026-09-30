#include "world/mosaic_omni_visual.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <random>
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

[[nodiscard]] std::size_t relation_rank_for(const std::size_t world_dim,
                                            const std::size_t rank) {
    return std::min(world_dim, checked_product(rank, std::size_t{2},
                                               "relation rank"));
}

void validate_dimensions(const std::size_t world_dim, const std::size_t rank,
                         const char* const message) {
    if (world_dim == 0 || rank == 0 || rank > world_dim) {
        throw std::invalid_argument(message);
    }
}

void require_size(const std::vector<double>& values, const std::size_t expected,
                  const char* const name) {
    if (values.size() != expected) {
        throw std::invalid_argument(std::string(name) + " has an invalid shape");
    }
}

[[nodiscard]] std::vector<double> ones(const std::size_t size) {
    return std::vector<double>(size, 1.0);
}

[[nodiscard]] std::vector<double> zeros(const std::size_t size) {
    return std::vector<double>(size, 0.0);
}

[[nodiscard]] std::mt19937_64& initialization_engine() {
    static thread_local std::mt19937_64 engine(std::random_device{}());
    return engine;
}

// torch.nn.Linear.reset_parameters uses kaiming_uniform_(a=sqrt(5)), which is
// exactly U(-1/sqrt(fan_in), 1/sqrt(fan_in)); bias uses the same bound.
[[nodiscard]] std::vector<double> linear_values(const std::size_t count,
                                                const std::size_t fan_in) {
    if (fan_in == 0) {
        throw std::invalid_argument("linear fan-in must be positive");
    }
    const double bound = 1.0 / std::sqrt(static_cast<double>(fan_in));
    std::uniform_real_distribution<double> distribution(-bound, bound);
    std::vector<double> result(count);
    for (double& value : result) value = distribution(initialization_engine());
    return result;
}

void require_tensor_compatibility(const Tensor& left, const Tensor& right,
                                  const char* const label) {
    if (left.dtype() != right.dtype() || left.device() != right.device()) {
        throw std::invalid_argument(std::string(label) +
                                    " must share dtype and device");
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
            const double difference = input[offset + column] - mean;
            variance += difference * difference;
        }
        variance /= static_cast<double>(width);
        const double inverse = 1.0 / std::sqrt(variance + layer_norm_epsilon);
        for (std::size_t column = 0; column != width; ++column) {
            output[offset + column] =
                (input[offset + column] - mean) * inverse * weight[column] +
                bias[column];
        }
    }
    return output;
}

[[nodiscard]] std::vector<double> linear(
    const std::vector<double>& input, const std::size_t rows,
    const std::size_t input_width, const std::size_t output_width,
    const std::vector<double>& weight, const std::vector<double>* const bias) {
    std::vector<double> output(checked_product(rows, output_width, "linear output"));
    for (std::size_t row = 0; row != rows; ++row) {
        for (std::size_t out = 0; out != output_width; ++out) {
            double value = bias == nullptr ? 0.0 : (*bias)[out];
            const auto weight_offset = out * input_width;
            const auto input_offset = row * input_width;
            for (std::size_t in = 0; in != input_width; ++in) {
                value += input[input_offset + in] * weight[weight_offset + in];
            }
            output[row * output_width + out] = value;
        }
    }
    return output;
}

void softmax_rows(std::vector<double>& values, const std::size_t rows,
                  const std::size_t width) {
    for (std::size_t row = 0; row != rows; ++row) {
        const auto begin = values.begin() + static_cast<std::ptrdiff_t>(row * width);
        const auto end = begin + static_cast<std::ptrdiff_t>(width);
        const double maximum = *std::max_element(begin, end);
        double total = 0.0;
        for (auto iterator = begin; iterator != end; ++iterator) {
            *iterator = std::exp(*iterator - maximum);
            total += *iterator;
        }
        for (auto iterator = begin; iterator != end; ++iterator) *iterator /= total;
    }
}

[[nodiscard]] std::vector<double> linspace(const std::size_t count) {
    if (count == 0) return {};
    if (count == 1) return {-1.0};
    std::vector<double> result(count);
    for (std::size_t index = 0; index != count; ++index) {
        result[index] = -1.0 + 2.0 * static_cast<double>(index) /
                                   static_cast<double>(count - 1);
    }
    return result;
}

[[nodiscard]] std::vector<double> positional_grid(
    const std::size_t world_dim, const std::size_t height,
    const std::size_t width) {
    if (world_dim % 4 != 0) {
        throw std::invalid_argument("world dimension must be divisible by four");
    }
    const auto quarter = world_dim / 4;
    const auto denominator = std::max<std::size_t>(1, quarter - 1);
    std::vector<float> frequency(quarter);
    for (std::size_t index = 0; index != quarter; ++index) {
        frequency[index] = std::exp(
            -std::log(10000.0F) * static_cast<float>(index) /
            static_cast<float>(denominator));
    }
    const auto vertical = linspace(height);
    const auto horizontal = linspace(width);
    std::vector<double> result(checked_product(
        checked_product(height, width, "position grid"), world_dim,
        "position grid"));
    for (std::size_t y = 0; y != height; ++y) {
        for (std::size_t x = 0; x != width; ++x) {
            const auto offset = (y * width + x) * world_dim;
            for (std::size_t index = 0; index != quarter; ++index) {
                const float y_phase = static_cast<float>(vertical[y]) * frequency[index];
                const float x_phase = static_cast<float>(horizontal[x]) * frequency[index];
                result[offset + index] = std::sin(y_phase);
                result[offset + quarter + index] = std::cos(y_phase);
                result[offset + 2 * quarter + index] = std::sin(x_phase);
                result[offset + 3 * quarter + index] = std::cos(x_phase);
            }
        }
    }
    return result;
}

[[nodiscard]] std::vector<double> coordinate_grid(const std::size_t height,
                                                  const std::size_t width) {
    const auto vertical = linspace(height);
    const auto horizontal = linspace(width);
    std::vector<double> result(checked_product(
        checked_product(height, width, "coordinate grid"), std::size_t{4},
        "coordinate grid"));
    for (std::size_t y = 0; y != height; ++y) {
        for (std::size_t x = 0; x != width; ++x) {
            const auto offset = (y * width + x) * 4;
            result[offset] = horizontal[x];
            result[offset + 1] = vertical[y];
            result[offset + 2] = horizontal[x] * horizontal[x];
            result[offset + 3] = vertical[y] * vertical[y];
        }
    }
    return result;
}

[[nodiscard]] double gelu(const double value) {
    return 0.5 * value * (1.0 + std::erf(value / std::sqrt(2.0)));
}

struct Binding final {
    std::vector<double> slot;
    std::vector<double> attention;
};

[[nodiscard]] Binding bind_descriptor(
    const std::vector<double>& descriptor, const std::vector<double>& patches,
    const std::vector<double>& coordinates, const std::size_t batch,
    const std::size_t patch_count, const std::size_t world_dim,
    const std::size_t rank,
    const ExplicitObjectRelationGrounderWeights& weights) {
    const auto object_width = checked_sum(rank, std::size_t{4}, "object projection");
    const auto normalized = layer_norm(descriptor, batch, world_dim,
                                       weights.descriptor_norm_weight,
                                       weights.descriptor_norm_bias);
    const auto query = linear(normalized, batch, world_dim, rank,
                              weights.query_weight, nullptr);
    const auto keys = linear(patches, batch * patch_count, world_dim, rank,
                             weights.key_weight, nullptr);
    std::vector<double> attention(batch * patch_count);
    const double scale = std::sqrt(static_cast<double>(rank));
    for (std::size_t item = 0; item != batch; ++item) {
        for (std::size_t patch = 0; patch != patch_count; ++patch) {
            double score = 0.0;
            for (std::size_t component = 0; component != rank; ++component) {
                score += query[item * rank + component] *
                         keys[(item * patch_count + patch) * rank + component];
            }
            attention[item * patch_count + patch] = score / scale;
        }
    }
    softmax_rows(attention, batch, patch_count);
    const auto values = linear(patches, batch * patch_count, world_dim, rank,
                               weights.value_weight, &weights.value_bias);
    std::vector<double> combined(
        checked_product(batch, object_width, "object projection"), 0.0);
    for (std::size_t item = 0; item != batch; ++item) {
        for (std::size_t patch = 0; patch != patch_count; ++patch) {
            const double coefficient = attention[item * patch_count + patch];
            for (std::size_t component = 0; component != rank; ++component) {
                combined[item * object_width + component] +=
                    coefficient * values[(item * patch_count + patch) * rank + component];
            }
            for (std::size_t component = 0; component != 4; ++component) {
                combined[item * object_width + rank + component] +=
                    coefficient * coordinates[patch * 4 + component];
            }
        }
    }
    return {linear(combined, batch, object_width, world_dim,
                   weights.object_up_weight, &weights.object_up_bias),
            std::move(attention)};
}

}  // namespace

VisualTeacherSlotBridgeWeights VisualTeacherSlotBridgeWeights::initialize(
    const std::size_t world_dim, const std::size_t rank) {
    validate_dimensions(world_dim, rank, "visual teacher bridge dimensions are invalid");
    const auto down_size = checked_product(rank, world_dim, "low-rank projection");
    const auto up_size = checked_product(world_dim, rank, "value_up.weight");
    return {ones(world_dim), zeros(world_dim), ones(world_dim), zeros(world_dim),
            linear_values(down_size, world_dim),
            linear_values(down_size, world_dim),
            linear_values(down_size, world_dim), linear_values(rank, world_dim),
            zeros(up_size), zeros(world_dim)};
}

void VisualTeacherSlotBridgeWeights::validate(const std::size_t world_dim,
                                              const std::size_t rank) const {
    validate_dimensions(world_dim, rank,
                        "visual teacher bridge dimensions are invalid");
    require_size(slot_norm_weight, world_dim, "slot_norm.weight");
    require_size(slot_norm_bias, world_dim, "slot_norm.bias");
    require_size(patch_norm_weight, world_dim, "patch_norm.weight");
    require_size(patch_norm_bias, world_dim, "patch_norm.bias");
    require_size(query_weight, checked_product(rank, world_dim, "query.weight"),
                 "query.weight");
    require_size(key_weight, checked_product(rank, world_dim, "key.weight"),
                 "key.weight");
    require_size(value_down_weight,
                 checked_product(rank, world_dim, "value_down.weight"),
                 "value_down.weight");
    require_size(value_down_bias, rank, "value_down.bias");
    require_size(value_up_weight,
                 checked_product(world_dim, rank, "value_up.weight"),
                 "value_up.weight");
    require_size(value_up_bias, world_dim, "value_up.bias");
}

VisualTeacherSlotBridge::VisualTeacherSlotBridge(const std::size_t world_dim,
                                                 const std::size_t rank)
    : VisualTeacherSlotBridge(
          world_dim, rank, VisualTeacherSlotBridgeWeights::initialize(world_dim, rank)) {}

VisualTeacherSlotBridge::VisualTeacherSlotBridge(
    const std::size_t world_dim, const std::size_t rank,
    VisualTeacherSlotBridgeWeights weights)
    : world_dim_(world_dim), rank_(rank), weights_(std::move(weights)) {
    validate_dimensions(world_dim_, rank_,
                        "visual teacher bridge dimensions are invalid");
    weights_.validate(world_dim_, rank_);
}

Tensor VisualTeacherSlotBridge::forward(const Tensor& world_slots,
                                        const Tensor& image_patches,
                                        const std::size_t patch_height,
                                        const std::size_t patch_width) const {
    const auto slots_shape = world_slots.shape();
    const auto patches_shape = image_patches.shape();
    if (slots_shape.size() != 3 || patches_shape.size() != 3) {
        throw std::invalid_argument("visual teacher bridge inputs must be sequences");
    }
    if (patch_height == 0 || patch_width == 0) {
        throw std::invalid_argument("visual patch grid dimensions must be positive");
    }
    const auto patch_count = checked_product(patch_height, patch_width, "visual patch grid");
    if (patches_shape[1] != patch_count) {
        throw std::invalid_argument("visual patch grid does not match its sequence");
    }
    if (slots_shape[0] != patches_shape[0] || slots_shape[2] != world_dim_ ||
        patches_shape[2] != world_dim_) {
        throw std::invalid_argument("visual teacher bridge tensor dimensions do not match");
    }
    require_tensor_compatibility(world_slots, image_patches,
                                 "visual teacher bridge inputs");
    const auto batch = static_cast<std::size_t>(slots_shape[0]);
    const auto slot_count = static_cast<std::size_t>(slots_shape[1]);
    auto positions = positional_grid(world_dim_, patch_height, patch_width);
    // _position is constructed in float32 and then cast to the patch dtype.
    const Tensor cast_positions(image_patches.dtype(),
                                {static_cast<std::uint64_t>(patch_count),
                                 static_cast<std::uint64_t>(world_dim_)},
                                std::move(positions), std::string(image_patches.device()));
    std::vector<double> patches(image_patches.values().begin(),
                                image_patches.values().end());
    for (std::size_t item = 0; item != batch; ++item) {
        for (std::size_t patch = 0; patch != patch_count; ++patch) {
            for (std::size_t component = 0; component != world_dim_; ++component) {
                patches[(item * patch_count + patch) * world_dim_ + component] +=
                    cast_positions.values()[patch * world_dim_ + component];
            }
        }
    }
    patches = layer_norm(patches, batch * patch_count, world_dim_,
                         weights_.patch_norm_weight, weights_.patch_norm_bias);
    std::vector<double> slots(world_slots.values().begin(), world_slots.values().end());
    slots = layer_norm(slots, batch * slot_count, world_dim_,
                       weights_.slot_norm_weight, weights_.slot_norm_bias);
    const auto queries = linear(slots, batch * slot_count, world_dim_, rank_,
                                weights_.query_weight, nullptr);
    const auto keys = linear(patches, batch * patch_count, world_dim_, rank_,
                             weights_.key_weight, nullptr);
    std::vector<double> attention(batch * slot_count * patch_count);
    const double scale = std::sqrt(static_cast<double>(rank_));
    for (std::size_t item = 0; item != batch; ++item) {
        for (std::size_t slot = 0; slot != slot_count; ++slot) {
            for (std::size_t patch = 0; patch != patch_count; ++patch) {
                double score = 0.0;
                for (std::size_t component = 0; component != rank_; ++component) {
                    score += queries[(item * slot_count + slot) * rank_ + component] *
                             keys[(item * patch_count + patch) * rank_ + component];
                }
                attention[(item * slot_count + slot) * patch_count + patch] =
                    score / scale;
            }
        }
    }
    softmax_rows(attention, batch * slot_count, patch_count);
    const auto values = linear(patches, batch * patch_count, world_dim_, rank_,
                               weights_.value_down_weight, &weights_.value_down_bias);
    std::vector<double> attended(batch * slot_count * rank_, 0.0);
    for (std::size_t item = 0; item != batch; ++item) {
        for (std::size_t slot = 0; slot != slot_count; ++slot) {
            for (std::size_t patch = 0; patch != patch_count; ++patch) {
                const double coefficient =
                    attention[(item * slot_count + slot) * patch_count + patch];
                for (std::size_t component = 0; component != rank_; ++component) {
                    attended[(item * slot_count + slot) * rank_ + component] +=
                        coefficient *
                        values[(item * patch_count + patch) * rank_ + component];
                }
            }
        }
    }
    auto output = linear(attended, batch * slot_count, rank_, world_dim_,
                         weights_.value_up_weight, &weights_.value_up_bias);
    for (double& value : output) value = 0.25 * std::tanh(value);
    return Tensor(world_slots.dtype(),
                  {slots_shape[0], slots_shape[1],
                   static_cast<std::uint64_t>(world_dim_)},
                  std::move(output), std::string(world_slots.device()));
}

ExplicitObjectRelationGrounderWeights
ExplicitObjectRelationGrounderWeights::initialize(const std::size_t world_dim,
                                                  const std::size_t rank) {
    validate_dimensions(world_dim, rank, "object relation dimensions are invalid");
    const auto relation_rank = relation_rank_for(world_dim, rank);
    const auto object_width = checked_sum(rank, std::size_t{4}, "object projection");
    const auto relation_width = checked_product(world_dim, std::size_t{4},
                                                "relation input");
    return {
        ones(world_dim), zeros(world_dim), ones(world_dim), zeros(world_dim),
        linear_values(checked_product(rank, world_dim, "query.weight"), world_dim),
        linear_values(checked_product(rank, world_dim, "key.weight"), world_dim),
        linear_values(checked_product(rank, world_dim, "value.weight"), world_dim),
        linear_values(rank, world_dim),
        linear_values(checked_product(world_dim, object_width, "object_up.weight"),
                      object_width),
        linear_values(world_dim, object_width), ones(relation_width),
        zeros(relation_width),
        linear_values(checked_product(relation_rank, relation_width,
                                      "relation_down.weight"),
                      relation_width),
        linear_values(relation_rank, relation_width),
        linear_values(checked_product(world_dim, relation_rank,
                                      "relation_up.weight"),
                      relation_rank),
        linear_values(world_dim, relation_rank)};
}

void ExplicitObjectRelationGrounderWeights::validate(
    const std::size_t world_dim, const std::size_t rank) const {
    validate_dimensions(world_dim, rank, "object relation dimensions are invalid");
    const auto relation_rank = relation_rank_for(world_dim, rank);
    const auto object_width = checked_sum(rank, std::size_t{4}, "object projection");
    const auto relation_width = checked_product(world_dim, std::size_t{4},
                                                "relation input");
    require_size(descriptor_norm_weight, world_dim, "descriptor_norm.weight");
    require_size(descriptor_norm_bias, world_dim, "descriptor_norm.bias");
    require_size(patch_norm_weight, world_dim, "patch_norm.weight");
    require_size(patch_norm_bias, world_dim, "patch_norm.bias");
    require_size(query_weight, checked_product(rank, world_dim, "query.weight"),
                 "query.weight");
    require_size(key_weight, checked_product(rank, world_dim, "key.weight"),
                 "key.weight");
    require_size(value_weight, checked_product(rank, world_dim, "value.weight"),
                 "value.weight");
    require_size(value_bias, rank, "value.bias");
    require_size(object_up_weight,
                 checked_product(world_dim, object_width, "object_up.weight"),
                 "object_up.weight");
    require_size(object_up_bias, world_dim, "object_up.bias");
    require_size(relation_norm_weight, relation_width, "relation_norm.weight");
    require_size(relation_norm_bias, relation_width, "relation_norm.bias");
    require_size(relation_down_weight,
                 checked_product(relation_rank, relation_width,
                                 "relation_down.weight"),
                 "relation_down.weight");
    require_size(relation_down_bias, relation_rank, "relation_down.bias");
    require_size(relation_up_weight,
                 checked_product(world_dim, relation_rank, "relation_up.weight"),
                 "relation_up.weight");
    require_size(relation_up_bias, world_dim, "relation_up.bias");
}

ExplicitObjectRelationGrounder::ExplicitObjectRelationGrounder(
    const std::size_t world_dim, const std::size_t rank)
    : ExplicitObjectRelationGrounder(
          world_dim, rank,
          ExplicitObjectRelationGrounderWeights::initialize(world_dim, rank)) {}

ExplicitObjectRelationGrounder::ExplicitObjectRelationGrounder(
    const std::size_t world_dim, const std::size_t rank,
    ExplicitObjectRelationGrounderWeights weights)
    : world_dim_(world_dim), rank_(rank), weights_(std::move(weights)) {
    validate_dimensions(world_dim_, rank_, "object relation dimensions are invalid");
    weights_.validate(world_dim_, rank_);
}

ExplicitObjectRelationGrounderOutput ExplicitObjectRelationGrounder::forward(
    const Tensor& image_patches, const Tensor& subject_descriptor,
    const Tensor& object_descriptor, const std::size_t patch_height,
    const std::size_t patch_width) const {
    const auto patches_shape = image_patches.shape();
    if (patches_shape.size() != 3) {
        throw std::invalid_argument("image patches must have shape [batch, patches, dim]");
    }
    if (patch_height == 0 || patch_width == 0) {
        throw std::invalid_argument("image patch grid dimensions must be positive");
    }
    const auto batch = static_cast<std::size_t>(patches_shape[0]);
    const auto patch_count = checked_product(patch_height, patch_width, "image patch grid");
    const auto subject_shape = subject_descriptor.shape();
    const auto object_shape = object_descriptor.shape();
    if (subject_shape.size() != 2 || object_shape.size() != 2 ||
        subject_shape[0] != patches_shape[0] || object_shape[0] != patches_shape[0] ||
        subject_shape[1] != world_dim_ || object_shape[1] != world_dim_) {
        throw std::invalid_argument("object descriptors must match batch and world dim");
    }
    if (patches_shape[1] != patch_count || patches_shape[2] != world_dim_) {
        throw std::invalid_argument("image patch grid does not match the grounder");
    }
    require_tensor_compatibility(image_patches, subject_descriptor,
                                 "image patches and subject descriptor");
    require_tensor_compatibility(image_patches, object_descriptor,
                                 "image patches and object descriptor");
    std::vector<double> patches(image_patches.values().begin(),
                                image_patches.values().end());
    patches = layer_norm(patches, batch * patch_count, world_dim_,
                         weights_.patch_norm_weight, weights_.patch_norm_bias);
    const Tensor coordinate_tensor(
        image_patches.dtype(),
        {static_cast<std::uint64_t>(patch_count), std::uint64_t{4}},
        coordinate_grid(patch_height, patch_width),
        std::string(image_patches.device()));
    const std::vector<double> coordinates(coordinate_tensor.values().begin(),
                                          coordinate_tensor.values().end());
    const std::vector<double> subject_values(subject_descriptor.values().begin(),
                                             subject_descriptor.values().end());
    const std::vector<double> object_values(object_descriptor.values().begin(),
                                            object_descriptor.values().end());
    auto subject = bind_descriptor(subject_values, patches, coordinates, batch,
                                   patch_count, world_dim_, rank_, weights_);
    auto object = bind_descriptor(object_values, patches, coordinates, batch,
                                  patch_count, world_dim_, rank_, weights_);
    const auto relation_width = checked_product(world_dim_, std::size_t{4},
                                                "relation input");
    std::vector<double> relation_input(
        checked_product(batch, relation_width, "relation input"));
    for (std::size_t item = 0; item != batch; ++item) {
        for (std::size_t component = 0; component != world_dim_; ++component) {
            const auto source = item * world_dim_ + component;
            const auto target = item * relation_width;
            relation_input[target + component] = subject.slot[source];
            relation_input[target + world_dim_ + component] = object.slot[source];
            relation_input[target + 2 * world_dim_ + component] =
                subject.slot[source] - object.slot[source];
            relation_input[target + 3 * world_dim_ + component] =
                subject.slot[source] * object.slot[source];
        }
    }
    relation_input = layer_norm(relation_input, batch, relation_width,
                                weights_.relation_norm_weight,
                                weights_.relation_norm_bias);
    const auto relation_rank = relation_rank_for(world_dim_, rank_);
    auto relation = linear(relation_input, batch, relation_width, relation_rank,
                           weights_.relation_down_weight,
                           &weights_.relation_down_bias);
    for (double& value : relation) value = gelu(value);
    relation = linear(relation, batch, relation_rank, world_dim_,
                      weights_.relation_up_weight, &weights_.relation_up_bias);
    std::vector<double> slots(batch * 3 * world_dim_);
    std::vector<double> attention(batch * 2 * patch_count);
    for (std::size_t item = 0; item != batch; ++item) {
        std::copy_n(subject.slot.begin() + static_cast<std::ptrdiff_t>(item * world_dim_),
                    world_dim_,
                    slots.begin() + static_cast<std::ptrdiff_t>(item * 3 * world_dim_));
        std::copy_n(object.slot.begin() + static_cast<std::ptrdiff_t>(item * world_dim_),
                    world_dim_,
                    slots.begin() + static_cast<std::ptrdiff_t>(
                        item * 3 * world_dim_ + world_dim_));
        std::copy_n(relation.begin() + static_cast<std::ptrdiff_t>(item * world_dim_),
                    world_dim_,
                    slots.begin() + static_cast<std::ptrdiff_t>(
                        item * 3 * world_dim_ + 2 * world_dim_));
        std::copy_n(subject.attention.begin() +
                        static_cast<std::ptrdiff_t>(item * patch_count),
                    patch_count,
                    attention.begin() +
                        static_cast<std::ptrdiff_t>(item * 2 * patch_count));
        std::copy_n(object.attention.begin() +
                        static_cast<std::ptrdiff_t>(item * patch_count),
                    patch_count,
                    attention.begin() + static_cast<std::ptrdiff_t>(
                        item * 2 * patch_count + patch_count));
    }
    return {
        Tensor(image_patches.dtype(),
               {patches_shape[0], 3, static_cast<std::uint64_t>(world_dim_)},
               std::move(slots), std::string(image_patches.device())),
        Tensor(image_patches.dtype(),
               {patches_shape[0], 2, static_cast<std::uint64_t>(patch_count)},
               std::move(attention), std::string(image_patches.device()))};
}

ExplicitRelationHeadWeights ExplicitRelationHeadWeights::initialize(
    const std::size_t dimension, const std::size_t classes) {
    if (dimension == 0 || classes == 0) {
        throw std::invalid_argument("explicit relation head dimensions must be positive");
    }
    return {ones(dimension), zeros(dimension),
            linear_values(checked_product(classes, dimension, "output.weight"),
                          dimension),
            linear_values(classes, dimension)};
}

void ExplicitRelationHeadWeights::validate(const std::size_t dimension,
                                           const std::size_t classes) const {
    if (dimension == 0 || classes == 0) {
        throw std::invalid_argument("explicit relation head dimensions must be positive");
    }
    require_size(norm_weight, dimension, "norm.weight");
    require_size(norm_bias, dimension, "norm.bias");
    require_size(output_weight,
                 checked_product(classes, dimension, "output.weight"),
                 "output.weight");
    require_size(output_bias, classes, "output.bias");
}

ExplicitRelationHead::ExplicitRelationHead(const std::size_t dimension,
                                           const std::size_t classes)
    : ExplicitRelationHead(
          dimension, classes, ExplicitRelationHeadWeights::initialize(dimension, classes)) {}

ExplicitRelationHead::ExplicitRelationHead(
    const std::size_t dimension, const std::size_t classes,
    ExplicitRelationHeadWeights weights)
    : dimension_(dimension), classes_(classes), weights_(std::move(weights)) {
    if (dimension_ == 0 || classes_ == 0) {
        throw std::invalid_argument("explicit relation head dimensions must be positive");
    }
    weights_.validate(dimension_, classes_);
}

Tensor ExplicitRelationHead::forward(const Tensor& relation_slot) const {
    const auto shape = relation_slot.shape();
    if (shape.empty() || shape.back() != dimension_) {
        throw std::invalid_argument(
            "relation slot final dimension must match the explicit relation head");
    }
    const auto rows = relation_slot.values().size() / dimension_;
    std::vector<double> values(relation_slot.values().begin(),
                               relation_slot.values().end());
    values = layer_norm(values, rows, dimension_, weights_.norm_weight,
                        weights_.norm_bias);
    values = linear(values, rows, dimension_, classes_, weights_.output_weight,
                    &weights_.output_bias);
    std::vector<std::uint64_t> output_shape(shape.begin(), shape.end());
    output_shape.back() = static_cast<std::uint64_t>(classes_);
    return Tensor(relation_slot.dtype(), std::move(output_shape), std::move(values),
                  std::string(relation_slot.device()));
}

}  // namespace swegca::world
