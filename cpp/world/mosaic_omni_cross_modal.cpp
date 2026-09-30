#include "world/mosaic_omni_cross_modal.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

namespace swegca::world {
namespace {

void require_size(const std::vector<float>& value, const std::size_t size, const char* name) {
    if (value.size() != size) throw std::invalid_argument(std::string(name) + " shape mismatch");
}

Tensor make_like(const Tensor& source, std::vector<std::uint64_t> shape,
                 std::vector<double> values) {
    if (source.device() != "cpu") throw std::invalid_argument("cross-modal fusion requires CPU tensor");
    return Tensor(source.dtype(), std::move(shape), std::move(values), "cpu");
}

Tensor linear(const Tensor& input, const std::vector<float>& weight,
              const std::vector<float>& bias, const std::size_t output_width) {
    const auto shape = input.shape();
    if (shape.empty()) throw std::invalid_argument("cross-modal linear input must have rank");
    const auto input_width = static_cast<std::size_t>(shape.back());
    require_size(weight, output_width * input_width, "cross-modal linear weight");
    if (!bias.empty()) require_size(bias, output_width, "cross-modal linear bias");
    const auto rows = input.values().size() / input_width;
    std::vector<double> output(rows * output_width);
    for (std::size_t row = 0; row < rows; ++row)
        for (std::size_t destination = 0; destination < output_width; ++destination) {
            double value = bias.empty() ? 0.0 : bias[destination];
            for (std::size_t source = 0; source < input_width; ++source)
                value += input.values()[row * input_width + source] *
                    weight[destination * input_width + source];
            output[row * output_width + destination] = value;
        }
    std::vector<std::uint64_t> output_shape(shape.begin(), shape.end());
    output_shape.back() = output_width;
    return make_like(input, std::move(output_shape), std::move(output));
}

Tensor normalize_rows(const Tensor& input) {
    const auto shape = input.shape(); const auto width = static_cast<std::size_t>(shape.back());
    const auto rows = input.values().size() / width; std::vector<double> output(input.values().size());
    for (std::size_t row = 0; row < rows; ++row) {
        double squared = 0.0; for (std::size_t d = 0; d < width; ++d) {
            const auto value = input.values()[row * width + d]; squared += value * value;
        }
        const auto scale = std::max(std::sqrt(squared), 1.0e-12);
        for (std::size_t d = 0; d < width; ++d)
            output[row * width + d] = input.values()[row * width + d] / scale;
    }
    return make_like(input, {shape.begin(), shape.end()}, std::move(output));
}

Tensor columns(const std::vector<const Tensor*>& values) {
    if (values.empty()) throw std::invalid_argument("cross-modal concatenation is empty");
    const auto batch = values.front()->shape()[0]; std::size_t width = 0;
    for (const auto* value : values) {
        if (value->shape().size() != 2 || value->shape()[0] != batch)
            throw std::invalid_argument("cross-modal features must be aligned [B,D]");
        width += static_cast<std::size_t>(value->shape()[1]);
    }
    std::vector<double> output(static_cast<std::size_t>(batch) * width);
    for (std::size_t b = 0; b < batch; ++b) {
        std::size_t offset = b * width;
        for (const auto* value : values) {
            const auto part = static_cast<std::size_t>(value->shape()[1]);
            std::copy_n(value->values().begin() + static_cast<std::ptrdiff_t>(b * part), part,
                        output.begin() + static_cast<std::ptrdiff_t>(offset));
            offset += part;
        }
    }
    return make_like(*values.front(), {batch, width}, std::move(output));
}

Tensor product(const Tensor& left, const Tensor& right) {
    if (left.shape().size() != right.shape().size() ||
        !std::equal(left.shape().begin(), left.shape().end(), right.shape().begin()))
        throw std::invalid_argument("cross-modal product shapes differ");
    std::vector<double> output(left.values().size());
    for (std::size_t i = 0; i < output.size(); ++i) output[i] = left.values()[i] * right.values()[i];
    return make_like(left, {left.shape().begin(), left.shape().end()}, std::move(output));
}

Tensor batch_norm(const Tensor& input, const std::vector<float>& mean,
                  const std::vector<float>& variance) {
    const auto width = static_cast<std::size_t>(input.shape().back());
    require_size(mean, width, "cross-modal batch norm mean");
    require_size(variance, width, "cross-modal batch norm variance");
    std::vector<double> output(input.values().size());
    for (std::size_t i = 0; i < output.size(); ++i)
        output[i] = (input.values()[i] - mean[i % width]) /
            std::sqrt(static_cast<double>(variance[i % width]) + 1.0e-5);
    return make_like(input, {input.shape().begin(), input.shape().end()}, std::move(output));
}

Tensor layer_norm(const Tensor& input, const std::vector<float>& weight,
                  const std::vector<float>& bias) {
    const auto width = static_cast<std::size_t>(input.shape().back());
    require_size(weight, width, "cross-modal head norm weight");
    require_size(bias, width, "cross-modal head norm bias");
    const auto rows = input.values().size() / width; std::vector<double> output(input.values().size());
    for (std::size_t row = 0; row < rows; ++row) {
        double mean = 0.0; for (std::size_t d = 0; d < width; ++d) mean += input.values()[row * width + d];
        mean /= width; double variance = 0.0;
        for (std::size_t d = 0; d < width; ++d) { const auto delta = input.values()[row * width + d] - mean; variance += delta * delta; }
        const auto inverse = 1.0 / std::sqrt(variance / width + 1.0e-5);
        for (std::size_t d = 0; d < width; ++d)
            output[row * width + d] = (input.values()[row * width + d] - mean) * inverse * weight[d] + bias[d];
    }
    return make_like(input, {input.shape().begin(), input.shape().end()}, std::move(output));
}

Tensor gelu(Tensor input) {
    std::vector<double> output(input.values().size());
    for (std::size_t i = 0; i < output.size(); ++i)
        output[i] = 0.5 * input.values()[i] *
            (1.0 + std::erf(input.values()[i] / std::sqrt(2.0)));
    return make_like(input, {input.shape().begin(), input.shape().end()}, std::move(output));
}

}  // namespace

void CrossModalEvidenceWeights::validate(const MosaicUnifiedConfig& config) const {
    const auto world = config.omni.world_dim;
    const auto rank = static_cast<std::size_t>(config.cross_modal_evidence_rank);
    const auto factor = config.cross_modal_evidence_direct_features ? 6U : 3U;
    require_size(text_projection, rank * world, "cross-modal text projection");
    require_size(audio_projection, rank * world, "cross-modal audio projection");
    require_size(video_projection, rank * world, "cross-modal video projection");
    if (config.cross_modal_text_query_pooling) {
        if (!text_query) throw std::invalid_argument("cross-modal query weights are missing");
        text_query->validate(rank);
    }
    if (config.cross_modal_text_sequence_pooling) {
        if (!text_sequence) throw std::invalid_argument("cross-modal sequence weights are missing");
        text_sequence->validate(rank);
    }
    require_size(normalization_mean, rank * factor, "cross-modal norm mean");
    require_size(normalization_variance, rank * factor, "cross-modal norm variance");
    require_size(to_world_weight, world * rank * factor, "cross-modal world weight");
    require_size(to_world_bias, world, "cross-modal world bias");
    require_size(head_norm_weight, world, "cross-modal head norm weight");
    require_size(head_norm_bias, world, "cross-modal head norm bias");
    require_size(head_weight, 2 * world, "cross-modal head weight");
    require_size(head_bias, 2, "cross-modal head bias");
}

CrossModalEvidenceFusion::CrossModalEvidenceFusion(
    MosaicUnifiedConfig config, CrossModalEvidenceWeights weights)
    : config_(std::move(config)), weights_(std::move(weights)) {
    config_.validate();
    if (!config_.cross_modal_evidence_head)
        throw std::invalid_argument("cross-modal evidence head is disabled");
    weights_.validate(config_);
}

CrossModalEvidenceOutput CrossModalEvidenceFusion::forward(
    const CrossModalEvidenceInput& input) const {
    const auto rank = static_cast<std::size_t>(config_.cross_modal_evidence_rank);
    auto text = linear(input.text_summary, weights_.text_projection, {}, rank);
    auto audio = input.audio_summary
        ? linear(*input.audio_summary, weights_.audio_projection, {}, rank)
        : make_like(text, {text.shape().begin(), text.shape().end()}, std::vector<double>(text.values().size()));
    auto video = input.video_summary
        ? linear(*input.video_summary, weights_.video_projection, {}, rank)
        : make_like(text, {text.shape().begin(), text.shape().end()}, std::vector<double>(text.values().size()));
    Tensor interaction = text.clone();
    if (config_.cross_modal_evidence_direct_features) {
        const auto projected_text_tokens = linear(input.text_tokens, weights_.text_projection, {}, rank);
        if (!input.audio_summary && input.video_summary) {
            if (!input.video_tokens) throw std::invalid_argument("video evidence tokens are unavailable");
            const auto projected_video_tokens = linear(*input.video_tokens, weights_.video_projection, {}, rank);
            auto [late_text_value, late_video] = cross_modal_late_summaries(
                projected_text_tokens, input.text_mask, projected_video_tokens);
            if (config_.cross_modal_text_contextual_pooling)
                late_text_value = cross_modal_last_summary(projected_text_tokens, input.text_mask);
            else if (weights_.text_sequence)
                late_text_value = cross_modal_sequence_summary(projected_text_tokens, input.text_mask, *weights_.text_sequence);
            else if (weights_.text_query)
                late_text_value = cross_modal_query_summary(projected_text_tokens, input.text_mask, *weights_.text_query);
            auto summary_product = product(text, video), late_product = product(late_text_value, late_video);
            interaction = columns({&text, &late_text_value, &video, &late_video, &summary_product, &late_product});
        } else if (!input.video_summary && input.audio_summary) {
            if (!input.audio_tokens) throw std::invalid_argument("audio evidence tokens are unavailable");
            const auto projected_audio_tokens = linear(*input.audio_tokens, weights_.audio_projection, {}, rank);
            auto [late_text_value, late_audio] = cross_modal_late_summaries(
                projected_text_tokens, input.text_mask, projected_audio_tokens);
            if (config_.cross_modal_text_contextual_pooling)
                late_text_value = cross_modal_last_summary(projected_text_tokens, input.text_mask);
            else if (weights_.text_sequence)
                late_text_value = cross_modal_sequence_summary(projected_text_tokens, input.text_mask, *weights_.text_sequence);
            else if (weights_.text_query)
                late_text_value = cross_modal_query_summary(projected_text_tokens, input.text_mask, *weights_.text_query);
            auto summary_product = product(text, audio), late_product = product(late_text_value, late_audio);
            interaction = columns({&text, &late_text_value, &audio, &late_audio, &summary_product, &late_product});
        } else {
            auto text_audio = product(text, audio), text_video = product(text, video), audio_video = product(audio, video);
            interaction = columns({&text, &audio, &video, &text_audio, &text_video, &audio_video});
        }
    } else {
        text = normalize_rows(text); audio = normalize_rows(audio); video = normalize_rows(video);
        auto text_audio = product(text, audio), text_video = product(text, video), audio_video = product(audio, video);
        interaction = columns({&text_audio, &text_video, &audio_video});
    }
    auto world = gelu(linear(batch_norm(interaction, weights_.normalization_mean,
        weights_.normalization_variance), weights_.to_world_weight, weights_.to_world_bias,
        config_.omni.world_dim));
    auto logits = linear(layer_norm(world, weights_.head_norm_weight, weights_.head_norm_bias),
        weights_.head_weight, weights_.head_bias, 2);
    return {std::move(world), std::move(logits)};
}

}  // namespace swegca::world
