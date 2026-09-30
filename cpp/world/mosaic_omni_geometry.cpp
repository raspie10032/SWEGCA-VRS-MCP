#include "world/mosaic_omni_geometry.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>

namespace swegca::world {
namespace {

constexpr float kLayerNormEpsilon = 1.0e-5F;
constexpr float kProbabilityFloor = 1.0e-6F;

[[noreturn]] void invalid(const std::string& message) {
    throw std::invalid_argument(message);
}

std::size_t checked_size(const std::uint64_t value, const char* label) {
    if (value > std::numeric_limits<std::size_t>::max())
        throw std::overflow_error(std::string(label) + " exceeds size_t");
    return static_cast<std::size_t>(value);
}

std::size_t checked_product(const std::initializer_list<std::size_t> values,
                            const char* label) {
    std::size_t product = 1;
    for (const auto value : values) {
        if (value != 0 && product > std::numeric_limits<std::size_t>::max() / value)
            throw std::overflow_error(std::string(label) + " shape overflow");
        product *= value;
    }
    return product;
}

void require_size(const std::vector<float>& values, const std::size_t expected,
                  const char* label) {
    if (values.size() != expected)
        invalid(std::string(label) + " has the wrong number of values");
}

std::vector<float> as_float32(const Tensor& tensor) {
    std::vector<float> result;
    result.reserve(tensor.values().size());
    for (const auto value : tensor.values()) result.push_back(static_cast<float>(value));
    return result;
}

Tensor make_float32(std::vector<std::uint64_t> shape, std::vector<float> values,
                    const std::string_view device) {
    std::vector<double> stored(values.begin(), values.end());
    return Tensor(TensorDType::float32, std::move(shape), std::move(stored),
                  std::string(device));
}

float gelu(const float value) {
    return 0.5F * value * (1.0F + std::erf(value / std::sqrt(2.0F)));
}

void gelu_in_place(std::vector<float>& values) {
    for (auto& value : values) value = gelu(value);
}

struct Conv2dSpec final {
    std::size_t input_channels{};
    std::size_t output_channels{};
    std::size_t kernel{};
    std::size_t stride{};
    std::size_t padding{};
};

struct FeatureMap final {
    std::size_t batch{};
    std::size_t channels{};
    std::size_t height{};
    std::size_t width{};
    std::vector<float> values;
};

FeatureMap conv2d(const FeatureMap& input, const Conv2dSpec spec,
                  const std::span<const float> weight,
                  const std::span<const float> bias) {
    if (input.channels != spec.input_channels)
        invalid("Conv2d input channel count mismatch");
    const auto padded_height = input.height + 2 * spec.padding;
    const auto padded_width = input.width + 2 * spec.padding;
    if (padded_height < spec.kernel || padded_width < spec.kernel)
        invalid("Conv2d kernel does not fit the padded input");
    const auto output_height = (padded_height - spec.kernel) / spec.stride + 1;
    const auto output_width = (padded_width - spec.kernel) / spec.stride + 1;
    FeatureMap output{input.batch, spec.output_channels, output_height, output_width,
        std::vector<float>(checked_product(
            {input.batch, spec.output_channels, output_height, output_width},
            "Conv2d output"))};
    for (std::size_t batch = 0; batch < input.batch; ++batch)
        for (std::size_t destination = 0; destination < spec.output_channels; ++destination)
            for (std::size_t out_y = 0; out_y < output_height; ++out_y)
                for (std::size_t out_x = 0; out_x < output_width; ++out_x) {
                    float value = bias[destination];
                    for (std::size_t source = 0; source < spec.input_channels; ++source)
                        for (std::size_t kernel_y = 0; kernel_y < spec.kernel; ++kernel_y)
                            for (std::size_t kernel_x = 0; kernel_x < spec.kernel; ++kernel_x) {
                                const auto padded_y = out_y * spec.stride + kernel_y;
                                const auto padded_x = out_x * spec.stride + kernel_x;
                                if (padded_y < spec.padding || padded_x < spec.padding) continue;
                                const auto input_y = padded_y - spec.padding;
                                const auto input_x = padded_x - spec.padding;
                                if (input_y >= input.height || input_x >= input.width) continue;
                                const auto input_index =
                                    ((batch * input.channels + source) * input.height + input_y) * input.width + input_x;
                                const auto weight_index =
                                    ((destination * spec.input_channels + source) * spec.kernel + kernel_y) * spec.kernel + kernel_x;
                                value += input.values[input_index] * weight[weight_index];
                            }
                    output.values[((batch * spec.output_channels + destination) * output_height + out_y) *
                                      output_width + out_x] = value;
                }
    return output;
}

std::vector<float> adaptive_average_pool_one(const FeatureMap& input) {
    if (input.height == 0 || input.width == 0) invalid("adaptive average pool input is empty");
    std::vector<float> output(input.batch * input.channels);
    const auto pixels = input.height * input.width;
    for (std::size_t batch = 0; batch < input.batch; ++batch)
        for (std::size_t channel = 0; channel < input.channels; ++channel) {
            float total = 0.0F;
            const auto offset = (batch * input.channels + channel) * pixels;
            for (std::size_t pixel = 0; pixel < pixels; ++pixel)
                total += input.values[offset + pixel];
            output[batch * input.channels + channel] = total / static_cast<float>(pixels);
        }
    return output;
}

std::vector<float> linear(const std::span<const float> input, const std::size_t rows,
                          const std::size_t input_width, const std::size_t output_width,
                          const std::span<const float> weight,
                          const std::span<const float> bias) {
    std::vector<float> output(checked_product({rows, output_width}, "linear output"));
    for (std::size_t row = 0; row < rows; ++row)
        for (std::size_t destination = 0; destination < output_width; ++destination) {
            float value = bias[destination];
            for (std::size_t source = 0; source < input_width; ++source)
                value += input[row * input_width + source] *
                         weight[destination * input_width + source];
            output[row * output_width + destination] = value;
        }
    return output;
}

std::vector<float> layer_norm(const std::span<const float> input,
                              const std::size_t rows, const std::size_t width,
                              const std::span<const float> weight,
                              const std::span<const float> bias) {
    std::vector<float> output(input.size());
    for (std::size_t row = 0; row < rows; ++row) {
        const auto offset = row * width;
        float mean = 0.0F;
        for (std::size_t column = 0; column < width; ++column)
            mean += input[offset + column];
        mean /= static_cast<float>(width);
        float variance = 0.0F;
        for (std::size_t column = 0; column < width; ++column) {
            const auto difference = input[offset + column] - mean;
            variance += difference * difference;
        }
        variance /= static_cast<float>(width);
        const auto inverse = 1.0F / std::sqrt(variance + kLayerNormEpsilon);
        for (std::size_t column = 0; column < width; ++column)
            output[offset + column] = (input[offset + column] - mean) * inverse *
                weight[column] + bias[column];
    }
    return output;
}

std::vector<float> mean_and_max(const std::span<const float> input,
                                const std::size_t batch, const std::size_t rows,
                                const std::size_t width) {
    if (rows == 0) invalid("mean/max reduction requires at least one row");
    std::vector<float> output(batch * width * 2);
    for (std::size_t b = 0; b < batch; ++b)
        for (std::size_t column = 0; column < width; ++column) {
            float total = 0.0F;
            float maximum = -std::numeric_limits<float>::infinity();
            for (std::size_t row = 0; row < rows; ++row) {
                const auto value = input[(b * rows + row) * width + column];
                total += value;
                if (std::isnan(value) || value > maximum) maximum = value;
            }
            output[b * 2 * width + column] = total / static_cast<float>(rows);
            output[b * 2 * width + width + column] = maximum;
        }
    return output;
}

void require_float32(const Tensor& tensor, const char* label) {
    if (tensor.dtype() != TensorDType::float32)
        invalid(std::string(label) + " must have the module weight dtype float32");
}

}  // namespace

void VideoEgomotionWeights::validate(const std::size_t world_dim) const {
    if (world_dim == 0) invalid("egomotion world dimension must be positive");
    require_size(convolution1_weight, checked_product({32, 24, 7, 7}, "egomotion convolution 1"),
                 "egomotion convolution 1 weight");
    require_size(convolution1_bias, 32, "egomotion convolution 1 bias");
    require_size(convolution2_weight, checked_product({64, 32, 5, 5}, "egomotion convolution 2"),
                 "egomotion convolution 2 weight");
    require_size(convolution2_bias, 64, "egomotion convolution 2 bias");
    require_size(convolution3_weight, checked_product({64, 64, 3, 3}, "egomotion convolution 3"),
                 "egomotion convolution 3 weight");
    require_size(convolution3_bias, 64, "egomotion convolution 3 bias");
    require_size(output_weight, checked_product({world_dim, 64}, "egomotion output"),
                 "egomotion output weight");
    require_size(output_bias, world_dim, "egomotion output bias");
}

VideoEgomotionReasoner::VideoEgomotionReasoner(
    const std::size_t world_dim, VideoEgomotionWeights weights)
    : world_dim_(world_dim), weights_(std::move(weights)) {
    weights_.validate(world_dim_);
}

Tensor VideoEgomotionReasoner::forward(const Tensor& video) const {
    const auto shape = video.shape();
    if (shape.size() != 5 || shape[1] < 4 || shape[1] % 2 != 0)
        invalid("egomotion video must contain paired stereo endpoints");
    if (shape[2] != 3)
        invalid("egomotion endpoint frames must have three channels");
    const auto batch = checked_size(shape[0], "egomotion batch");
    const auto frames = checked_size(shape[1], "egomotion frames");
    const auto height = checked_size(shape[3], "egomotion height");
    const auto width = checked_size(shape[4], "egomotion width");
    if (height == 0 || width == 0)
        invalid("egomotion spatial dimensions must be non-zero");
    const auto source = as_float32(video);
    FeatureMap features{batch, 24, height, width,
        std::vector<float>(checked_product({batch, 24, height, width}, "egomotion endpoint features"))};
    const auto frame_size = checked_product({3, height, width}, "egomotion frame");
    const auto video_batch_size = checked_product({frames, frame_size}, "egomotion video batch");
    for (std::size_t b = 0; b < batch; ++b) {
        const std::array<std::size_t, 4> frame_indices{0, 1, frames - 2, frames - 1};
        for (std::size_t endpoint = 0; endpoint < frame_indices.size(); ++endpoint)
            std::copy_n(source.begin() + b * video_batch_size + frame_indices[endpoint] * frame_size,
                        frame_size,
                        features.values.begin() + (b * 24 + endpoint * 3) * height * width);
        for (std::size_t channel = 0; channel < 3; ++channel)
            for (std::size_t pixel = 0; pixel < height * width; ++pixel) {
                const auto input_at = [&](const std::size_t frame) {
                    return source[b * video_batch_size + frame * frame_size + channel * height * width + pixel];
                };
                const auto output_at = [&](const std::size_t feature) -> float& {
                    return features.values[(b * 24 + feature * 3 + channel) * height * width + pixel];
                };
                output_at(4) = input_at(frames - 2) - input_at(0);
                output_at(5) = input_at(frames - 1) - input_at(1);
                output_at(6) = input_at(1) - input_at(0);
                output_at(7) = input_at(frames - 1) - input_at(frames - 2);
            }
    }
    auto encoded = conv2d(features, {24, 32, 7, 4, 3},
                          weights_.convolution1_weight, weights_.convolution1_bias);
    gelu_in_place(encoded.values);
    encoded = conv2d(encoded, {32, 64, 5, 2, 2},
                     weights_.convolution2_weight, weights_.convolution2_bias);
    gelu_in_place(encoded.values);
    encoded = conv2d(encoded, {64, 64, 3, 2, 1},
                     weights_.convolution3_weight, weights_.convolution3_bias);
    gelu_in_place(encoded.values);
    const auto pooled = adaptive_average_pool_one(encoded);
    auto output = linear(pooled, batch, 64, world_dim_,
                         weights_.output_weight, weights_.output_bias);
    return make_float32({shape[0], world_dim_}, std::move(output), video.device());
}

Tensor video_egomotion_validity_statistics(const Tensor& video) {
    const auto shape = video.shape();
    if (shape.size() != 5 || shape[1] < 4 || shape[1] % 2 != 0)
        invalid("egomotion video must contain paired stereo endpoints");
    const auto batch = checked_size(shape[0], "egomotion batch");
    const auto frames = checked_size(shape[1], "egomotion frames");
    const auto channels = checked_size(shape[2], "egomotion channels");
    const auto height = checked_size(shape[3], "egomotion height");
    const auto width = checked_size(shape[4], "egomotion width");
    const auto frame_size = checked_product({channels, height, width}, "egomotion frame");
    if (frame_size == 0)
        invalid("egomotion validity input must be non-empty");
    const auto batch_size = checked_product({frames, frame_size}, "egomotion video batch");
    const auto two_frame_size = checked_product({2, frame_size}, "egomotion validity eye frames");
    std::vector<double> delta_values(checked_product({batch, two_frame_size},
                                                     "egomotion validity delta"));
    for (std::size_t b = 0; b < batch; ++b) {
        for (std::size_t eye = 0; eye < 2; ++eye)
            for (std::size_t element = 0; element < frame_size; ++element) {
                const auto before = video.values()[b * batch_size + eye * frame_size + element];
                const auto after = video.values()[b * batch_size + (frames - 2 + eye) * frame_size + element];
                delta_values[(b * 2 + eye) * frame_size + element] =
                    std::abs(after - before);
            }
    }
    Tensor delta(video.dtype(), {shape[0], 2, shape[2], shape[3], shape[4]},
                 std::move(delta_values), std::string(video.device()));
    std::vector<double> result(batch * 2);
    for (std::size_t b = 0; b < batch; ++b) {
        double total = 0.0;
        float total_float = 0.0F;
        double maximum = 0.0;
        for (std::size_t element = 0; element < two_frame_size; ++element) {
            const auto value = delta.values()[b * two_frame_size + element];
            total += value;
            total_float += static_cast<float>(value);
            if (std::isnan(value) || value > maximum) maximum = value;
        }
        result[b * 2] = video.dtype() == TensorDType::float64
            ? total / static_cast<double>(two_frame_size)
            : static_cast<double>(total_float / static_cast<float>(two_frame_size));
        result[b * 2 + 1] = maximum;
    }
    return Tensor(video.dtype(), {shape[0], 2}, std::move(result), std::string(video.device()));
}

void VideoSpatialGeometryWeights::validate(const std::size_t camera_pose_dim) const {
    if (camera_pose_dim == 0) invalid("camera pose dimension must be positive");
    if (camera_pose_dim > (std::numeric_limits<std::size_t>::max() - 28) / 3)
        throw std::overflow_error("stereo feature dimension overflows size_t");
    const auto frame_width = camera_pose_dim + 16;
    const auto stereo_width = camera_pose_dim * 3 + 28;
    require_size(frame_norm_weight, frame_width, "geometry frame norm weight");
    require_size(frame_norm_bias, frame_width, "geometry frame norm bias");
    require_size(frame_linear1_weight, checked_product({64, frame_width}, "geometry frame linear 1"),
                 "geometry frame linear 1 weight");
    require_size(frame_linear1_bias, 64, "geometry frame linear 1 bias");
    require_size(frame_linear2_weight, checked_product({64, 64}, "geometry frame linear 2"),
                 "geometry frame linear 2 weight");
    require_size(frame_linear2_bias, 64, "geometry frame linear 2 bias");
    require_size(stereo_norm_weight, stereo_width, "geometry stereo norm weight");
    require_size(stereo_norm_bias, stereo_width, "geometry stereo norm bias");
    require_size(stereo_linear1_weight, checked_product({64, stereo_width}, "geometry stereo linear 1"),
                 "geometry stereo linear 1 weight");
    require_size(stereo_linear1_bias, 64, "geometry stereo linear 1 bias");
    require_size(stereo_linear2_weight, checked_product({32, 64}, "geometry stereo linear 2"),
                 "geometry stereo linear 2 weight");
    require_size(stereo_linear2_bias, 32, "geometry stereo linear 2 bias");
}

VideoSpatialGeometryReasoner::VideoSpatialGeometryReasoner(
    const std::size_t camera_pose_dim, VideoSpatialGeometryWeights weights)
    : camera_pose_dim_(camera_pose_dim), weights_(std::move(weights)) {
    weights_.validate(camera_pose_dim_);
}

Tensor VideoSpatialGeometryReasoner::stereo_summary(
    const Tensor& role, const Tensor& camera_pose) const {
    const auto role_shape = role.shape();
    const auto batch = checked_size(role_shape[0], "stereo batch");
    const auto frames = checked_size(role_shape[2], "stereo frames");
    if (camera_pose_dim_ < 14 || frames % 2 != 0)
        return make_float32({role_shape[0], 64}, std::vector<float>(batch * 64, 0.0F),
                            role.device());
    const auto pairs = frames / 2;
    if (pairs == 0) invalid("stereo summary requires at least one frame pair");
    const auto stereo_width = camera_pose_dim_ * 3 + 28;
    std::vector<float> joined(checked_product({batch, pairs, stereo_width}, "stereo pair input"));
    const auto& roles = role.values();
    const auto& poses = camera_pose.values();
    const auto role_at = [&](const std::size_t b, const std::size_t object_role,
                             const std::size_t frame, const std::size_t feature) -> float {
        return static_cast<float>(roles[((b * 2 + object_role) * frames + frame) * 4 + feature]);
    };
    const auto pose_at = [&](const std::size_t b, const std::size_t frame,
                             const std::size_t feature) -> float {
        return static_cast<float>(poses[(b * frames + frame) * camera_pose_dim_ + feature]);
    };
    for (std::size_t b = 0; b < batch; ++b)
        for (std::size_t pair = 0; pair < pairs; ++pair) {
            const auto first_frame = pair * 2;
            const auto first_sign = pose_at(b, first_frame, 12);
            const auto second_sign = pose_at(b, first_frame + 1, 12);
            std::size_t left_eye = 0;
            std::size_t right_eye = 0;
            if (std::isnan(first_sign)) {
                left_eye = 0; right_eye = 0;
            } else if (std::isnan(second_sign)) {
                left_eye = 1; right_eye = 1;
            } else {
                left_eye = second_sign < first_sign ? 1 : 0;
                right_eye = second_sign > first_sign ? 1 : 0;
            }
            const auto left_frame = first_frame + left_eye;
            const auto right_frame = first_frame + right_eye;
            auto* destination = joined.data() + (b * pairs + pair) * stereo_width;
            std::array<float, 8> left_role{}, right_role{}, disparity{};
            for (std::size_t object_role = 0; object_role < 2; ++object_role)
                for (std::size_t feature = 0; feature < 4; ++feature) {
                    const auto index = object_role * 4 + feature;
                    left_role[index] = role_at(b, object_role, left_frame, feature);
                    right_role[index] = role_at(b, object_role, right_frame, feature);
                    disparity[index] = left_role[index] - right_role[index];
                }
            std::copy(left_role.begin(), left_role.end(), destination);
            std::copy(right_role.begin(), right_role.end(), destination + 8);
            std::copy(disparity.begin(), disparity.end(), destination + 16);
            for (std::size_t feature = 0; feature < 4; ++feature)
                destination[24 + feature] = disparity[feature] - disparity[4 + feature];
            auto cursor = std::size_t{28};
            for (std::size_t feature = 0; feature < camera_pose_dim_; ++feature)
                destination[cursor++] = pose_at(b, left_frame, feature);
            for (std::size_t feature = 0; feature < camera_pose_dim_; ++feature)
                destination[cursor++] = pose_at(b, right_frame, feature);
            for (std::size_t feature = 0; feature < camera_pose_dim_; ++feature)
                destination[cursor++] = pose_at(b, right_frame, feature) -
                                        pose_at(b, left_frame, feature);
        }
    auto normalized = layer_norm(joined, batch * pairs, stereo_width,
                                 weights_.stereo_norm_weight, weights_.stereo_norm_bias);
    auto hidden = linear(normalized, batch * pairs, stereo_width, 64,
                         weights_.stereo_linear1_weight, weights_.stereo_linear1_bias);
    gelu_in_place(hidden);
    auto pair_values = linear(hidden, batch * pairs, 64, 32,
                              weights_.stereo_linear2_weight, weights_.stereo_linear2_bias);
    gelu_in_place(pair_values);
    auto summarized = mean_and_max(pair_values, batch, pairs, 32);
    return make_float32({role_shape[0], 64}, std::move(summarized), role.device());
}

Tensor VideoSpatialGeometryReasoner::forward(
    const Tensor& attention, const Tensor& camera_pose) const {
    const auto shape = attention.shape();
    if (shape.size() != 5 || shape[1] != 2)
        invalid("spatial reasoning attention must be [B,2,F,H,W]");
    const auto pose_shape = camera_pose.shape();
    if (pose_shape.size() != 3 || pose_shape[0] != shape[0] ||
        pose_shape[1] != shape[2] || pose_shape[2] != camera_pose_dim_)
        invalid("camera pose must align with spatial attention frames");
    if (camera_pose.device() != attention.device())
        invalid("camera pose and spatial attention must use the same device");
    require_float32(attention, "spatial reasoning attention");
    const auto batch = checked_size(shape[0], "spatial batch");
    const auto frames = checked_size(shape[2], "spatial frames");
    const auto height = checked_size(shape[3], "spatial height");
    const auto width = checked_size(shape[4], "spatial width");
    if (frames == 0 || height == 0 || width == 0)
        invalid("spatial reasoning frame and map dimensions must be non-zero");
    const auto pixels = checked_product({height, width}, "spatial pixels");
    const auto attention_values = as_float32(attention);
    // camera_pose.to(attention.dtype) in the source. Constructing a temporary
    // Tensor applies the existing native dtype canonicalization before use.
    Tensor cast_pose(attention.dtype(),
        std::vector<std::uint64_t>(pose_shape.begin(), pose_shape.end()),
        std::vector<double>(camera_pose.values().begin(), camera_pose.values().end()),
        std::string(attention.device()));
    const auto pose_values = as_float32(cast_pose);
    std::vector<float> role(checked_product({batch, 2, frames, 4}, "spatial role features"));
    const auto coordinate = [](const std::size_t index, const std::size_t count) {
        if (count == 1) return -1.0F;
        return -1.0F + 2.0F * static_cast<float>(index) /
                           static_cast<float>(count - 1);
    };
    const auto entropy_denominator = std::log(static_cast<float>(std::max<std::size_t>(2, pixels)));
    for (std::size_t b = 0; b < batch; ++b)
        for (std::size_t object_role = 0; object_role < 2; ++object_role)
            for (std::size_t frame = 0; frame < frames; ++frame) {
                const auto input_offset = ((b * 2 + object_role) * frames + frame) * pixels;
                float denominator = 0.0F;
                for (std::size_t pixel = 0; pixel < pixels; ++pixel)
                    denominator += attention_values[input_offset + pixel];
                denominator = std::max(denominator, kProbabilityFloor);
                float x_position = 0.0F, y_position = 0.0F;
                float peak = -std::numeric_limits<float>::infinity();
                float entropy_sum = 0.0F;
                for (std::size_t y = 0; y < height; ++y)
                    for (std::size_t x = 0; x < width; ++x) {
                        const auto probability = attention_values[input_offset + y * width + x] / denominator;
                        x_position += probability * coordinate(x, width);
                        y_position += probability * coordinate(y, height);
                        if (std::isnan(probability) || probability > peak) peak = probability;
                        entropy_sum += probability * std::log(std::max(probability, kProbabilityFloor));
                    }
                const auto output_offset = ((b * 2 + object_role) * frames + frame) * 4;
                role[output_offset] = x_position;
                role[output_offset + 1] = y_position;
                role[output_offset + 2] = peak;
                role[output_offset + 3] = -entropy_sum / entropy_denominator;
            }
    const auto frame_width = camera_pose_dim_ + 16;
    std::vector<float> frame_input(checked_product({batch, frames, frame_width}, "geometry frame input"));
    for (std::size_t b = 0; b < batch; ++b)
        for (std::size_t frame = 0; frame < frames; ++frame) {
            auto* destination = frame_input.data() + (b * frames + frame) * frame_width;
            const auto* first = role.data() + ((b * 2) * frames + frame) * 4;
            const auto* second = role.data() + ((b * 2 + 1) * frames + frame) * 4;
            for (std::size_t feature = 0; feature < 4; ++feature) {
                destination[feature] = first[feature];
                destination[4 + feature] = second[feature];
                destination[8 + feature] = first[feature] - second[feature];
                destination[12 + feature] = first[feature] * second[feature];
            }
            std::copy_n(pose_values.begin() + (b * frames + frame) * camera_pose_dim_,
                        camera_pose_dim_, destination + 16);
        }
    auto normalized = layer_norm(frame_input, batch * frames, frame_width,
                                 weights_.frame_norm_weight, weights_.frame_norm_bias);
    auto frame_hidden = linear(normalized, batch * frames, frame_width, 64,
                               weights_.frame_linear1_weight, weights_.frame_linear1_bias);
    gelu_in_place(frame_hidden);
    auto frame_values = linear(frame_hidden, batch * frames, 64, 64,
                               weights_.frame_linear2_weight, weights_.frame_linear2_bias);
    gelu_in_place(frame_values);
    auto frame_summary = mean_and_max(frame_values, batch, frames, 64);
    auto role_tensor = make_float32({shape[0], 2, shape[2], 4}, role, attention.device());
    auto stereo = stereo_summary(role_tensor, cast_pose);
    std::vector<float> output(checked_product({batch, output_dim}, "spatial geometry output"));
    for (std::size_t b = 0; b < batch; ++b) {
        std::copy_n(frame_summary.begin() + b * 128, 128, output.begin() + b * output_dim);
        for (std::size_t feature = 0; feature < 64; ++feature)
            output[b * output_dim + 128 + feature] =
                static_cast<float>(stereo.values()[b * 64 + feature]);
    }
    return make_float32({shape[0], output_dim}, std::move(output), attention.device());
}

}  // namespace swegca::world
