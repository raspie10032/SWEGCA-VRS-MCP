#include "world/mosaic_omni_text_adapters.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <stdexcept>

namespace swegca::world {
namespace {

constexpr std::size_t vocab = static_cast<std::size_t>(mosaic_vocab_size);

std::size_t count(std::span<const std::uint64_t> shape) {
    std::size_t result = 1;
    for (const auto extent : shape) {
        if (extent && result > std::numeric_limits<std::size_t>::max() / extent)
            throw std::overflow_error("text adapter tensor size overflow");
        result *= static_cast<std::size_t>(extent);
    }
    return result;
}
void tensor_ok(const Tensor& tensor) {
    if (count(tensor.shape()) != tensor.values().size())
        throw std::invalid_argument("text adapter tensor storage mismatch");
}
Tensor make_like(const Tensor& source, std::vector<std::uint64_t> shape, std::vector<double> values) {
    return Tensor(source.dtype(), std::move(shape), std::move(values), std::string(source.device()));
}
void linear_ok(const TextAdapterLinearWeights& weights, const std::size_t in,
               const std::size_t out, const bool bias = true) {
    if (weights.weight.size() != in * out || (bias ? weights.bias.size() != out : !weights.bias.empty()))
        throw std::invalid_argument("text adapter linear weight shape mismatch");
}
void norm_ok(const TextAdapterNormWeights& weights, const std::size_t dim) {
    if (weights.weight.size() != dim || weights.bias.size() != dim)
        throw std::invalid_argument("text adapter norm weight shape mismatch");
}
void attention_ok(const TextAdapterAttentionWeights& weights, const std::size_t dim) {
    linear_ok(weights.in_projection, dim, dim * 3);
    linear_ok(weights.out_projection, dim, dim);
}

Tensor linear(const Tensor& input, const TextAdapterLinearWeights& weights, const std::size_t out) {
    tensor_ok(input); const auto shape = input.shape();
    if (shape.empty()) throw std::invalid_argument("linear input must have a final dimension");
    const auto in = static_cast<std::size_t>(shape.back()); linear_ok(weights, in, out, !weights.bias.empty());
    const auto rows = input.values().size() / in; std::vector<double> values(rows * out);
    for (std::size_t row = 0; row < rows; ++row) for (std::size_t o = 0; o < out; ++o) {
        double sum = weights.bias.empty() ? 0.0 : weights.bias[o];
        for (std::size_t i = 0; i < in; ++i) sum += input.values()[row * in + i] * weights.weight[o * in + i];
        values[row * out + o] = sum;
    }
    std::vector<std::uint64_t> result_shape(shape.begin(), shape.end()); result_shape.back() = out;
    return make_like(input, std::move(result_shape), std::move(values));
}
Tensor norm(const Tensor& input, const TextAdapterNormWeights& weights) {
    tensor_ok(input); const auto shape = input.shape();
    if (shape.empty()) throw std::invalid_argument("norm input must have a final dimension");
    const auto dim = static_cast<std::size_t>(shape.back()); norm_ok(weights, dim);
    const auto rows = input.values().size() / dim; std::vector<double> values(input.values().size());
    for (std::size_t row = 0; row < rows; ++row) {
        double mean = 0.0; for (std::size_t d = 0; d < dim; ++d) mean += input.values()[row * dim + d]; mean /= dim;
        double variance = 0.0; for (std::size_t d = 0; d < dim; ++d) { const auto x = input.values()[row * dim + d] - mean; variance += x * x; }
        const auto inverse = 1.0 / std::sqrt(variance / dim + 1.0e-5);
        for (std::size_t d = 0; d < dim; ++d)
            values[row * dim + d] = (input.values()[row * dim + d] - mean) * inverse * weights.weight[d] + weights.bias[d];
    }
    return make_like(input, {shape.begin(), shape.end()}, std::move(values));
}
Tensor gelu(Tensor input) {
    std::vector<double> values(input.values().begin(), input.values().end());
    for (auto& value : values) value *= 0.5 * (1.0 + std::erf(value / std::sqrt(2.0)));
    return make_like(input, {input.shape().begin(), input.shape().end()}, std::move(values));
}
Tensor attention(const Tensor& query, const Tensor& key_value, const BooleanMask& mask,
                 const std::size_t heads, const TextAdapterAttentionWeights& weights) {
    tensor_ok(query); tensor_ok(key_value); const auto qs = query.shape(), ks = key_value.shape();
    if (qs.size() != 3 || ks.size() != 3 || qs[0] != ks[0] || qs[2] != ks[2] ||
        mask.shape().size() != 2 || mask.shape()[0] != ks[0] || mask.shape()[1] != ks[1])
        throw std::invalid_argument("text adapter attention shape mismatch");
    const auto batch = static_cast<std::size_t>(qs[0]), queries = static_cast<std::size_t>(qs[1]);
    const auto keys = static_cast<std::size_t>(ks[1]), dim = static_cast<std::size_t>(qs[2]);
    if (!heads || dim % heads) throw std::invalid_argument("attention heads must divide dimension");
    attention_ok(weights, dim); const auto qkv_q = linear(query, weights.in_projection, dim * 3);
    const auto qkv_k = linear(key_value, weights.in_projection, dim * 3); const auto hd = dim / heads;
    std::vector<double> joined(batch * queries * dim), scores(keys); const auto scale = 1.0 / std::sqrt(static_cast<double>(hd));
    for (std::size_t b = 0; b < batch; ++b) for (std::size_t h = 0; h < heads; ++h)
        for (std::size_t q = 0; q < queries; ++q) {
            double maximum = -std::numeric_limits<double>::infinity();
            for (std::size_t k = 0; k < keys; ++k) {
                if (!mask.at(b, k)) { scores[k] = -std::numeric_limits<double>::infinity(); continue; }
                double score = 0.0; for (std::size_t d = 0; d < hd; ++d)
                    score += qkv_q.values()[(b * queries + q) * dim * 3 + h * hd + d] *
                        qkv_k.values()[(b * keys + k) * dim * 3 + dim + h * hd + d];
                scores[k] = score * scale; maximum = std::max(maximum, scores[k]);
            }
            double denominator = 0.0; for (std::size_t k = 0; k < keys; ++k) if (mask.at(b, k)) {
                scores[k] = std::exp(scores[k] - maximum); denominator += scores[k];
            }
            for (std::size_t d = 0; d < hd; ++d) {
                double value = 0.0; for (std::size_t k = 0; k < keys; ++k) if (mask.at(b, k))
                    value += scores[k] / denominator * qkv_k.values()[(b * keys + k) * dim * 3 + 2 * dim + h * hd + d];
                joined[(b * queries + q) * dim + h * hd + d] = value;
            }
        }
    return linear(make_like(query, {qs[0], qs[1], qs[2]}, std::move(joined)), weights.out_projection, dim);
}
Tensor add_positions(const Tensor& input) {
    const auto shape = input.shape(); if (shape.size() != 3) throw std::invalid_argument("position input rank mismatch");
    const auto batch = static_cast<std::size_t>(shape[0]), length = static_cast<std::size_t>(shape[1]), dim = static_cast<std::size_t>(shape[2]);
    std::vector<double> values(input.values().begin(), input.values().end());
    for (std::size_t b = 0; b < batch; ++b) for (std::size_t p = 0; p < length; ++p)
        for (std::size_t d = 0; d < dim; ++d) {
            const auto pair = d / 2; const auto frequency = std::exp(static_cast<double>(pair * 2) * (-std::log(10000.0) / dim));
            values[(b * length + p) * dim + d] += d % 2 ? std::cos(p * frequency) : std::sin(p * frequency);
        }
    return make_like(input, {shape.begin(), shape.end()}, std::move(values));
}
Tensor encoder(const Tensor& input, const BooleanMask& mask, const std::size_t heads,
               const TextAdapterEncoderWeights& weights) {
    const auto dim = static_cast<std::size_t>(input.shape().back());
    auto first = norm(input, weights.norm1); auto update = attention(first, first, mask, heads, weights.attention);
    std::vector<double> residual(input.values().begin(), input.values().end());
    for (std::size_t i = 0; i < residual.size(); ++i) residual[i] += update.values()[i];
    auto state = make_like(input, {input.shape().begin(), input.shape().end()}, std::move(residual));
    auto hidden = gelu(linear(norm(state, weights.norm2), weights.feedforward_in, dim * 2));
    auto output = linear(hidden, weights.feedforward_out, dim); residual.assign(state.values().begin(), state.values().end());
    for (std::size_t i = 0; i < residual.size(); ++i) residual[i] += output.values()[i];
    return make_like(input, {input.shape().begin(), input.shape().end()}, std::move(residual));
}
std::pair<std::vector<double>, std::vector<double>> pool(const Tensor& states, const BooleanMask& mask) {
    const auto shape = states.shape(); if (shape.size() != 3 || mask.shape().size() != 2 || mask.shape()[0] != shape[0] || mask.shape()[1] != shape[1])
        throw std::invalid_argument("answerability pool shape mismatch");
    const auto batch = static_cast<std::size_t>(shape[0]), tokens = static_cast<std::size_t>(shape[1]), dim = static_cast<std::size_t>(shape[2]);
    std::vector<double> mean(batch * dim), maximum(batch * dim, -std::numeric_limits<double>::infinity());
    for (std::size_t b = 0; b < batch; ++b) {
        std::size_t active = 0; for (std::size_t t = 0; t < tokens; ++t) if (mask.at(b, t) || (!active && t == 0 && std::none_of(mask.values().begin() + static_cast<std::ptrdiff_t>(b * tokens), mask.values().begin() + static_cast<std::ptrdiff_t>((b + 1) * tokens), [](auto x){ return x != 0; }))) {
            ++active; for (std::size_t d = 0; d < dim; ++d) { const auto value = states.values()[(b * tokens + t) * dim + d]; mean[b * dim + d] += value; maximum[b * dim + d] = std::max(maximum[b * dim + d], value); }
        }
        for (std::size_t d = 0; d < dim; ++d) mean[b * dim + d] /= std::max<std::size_t>(active, 1);
    }
    return {std::move(mean), std::move(maximum)};
}
BooleanMask safe_mask(const BooleanMask& source) {
    const auto shape = source.shape();
    if (shape.size() != 2 || !shape[1]) throw std::invalid_argument("safe mask must have shape [B,T] with T positive");
    auto values = std::vector<std::uint8_t>(source.values().begin(), source.values().end());
    for (std::size_t b = 0; b < shape[0]; ++b) {
        bool any = false; for (std::size_t t = 0; t < shape[1]; ++t) any |= values[b * shape[1] + t] != 0;
        if (!any) values[b * shape[1]] = 1;
    }
    return BooleanMask({shape.begin(), shape.end()}, std::move(values));
}

} // namespace

TextOnlyCrossMemoryAdapter::TextOnlyCrossMemoryAdapter(const std::size_t model_dim,
    const std::size_t attention_heads, TextOnlyCrossMemoryWeights weights)
    : dim_(model_dim), heads_(attention_heads), weights_(std::move(weights)) {
    if (!dim_ || !heads_ || dim_ % heads_) throw std::invalid_argument("invalid text cross-memory dimensions");
    linear_ok(weights_.query, vocab, dim_); attention_ok(weights_.cross_attention, dim_);
    linear_ok(weights_.output, dim_, vocab);
}
Tensor TextOnlyCrossMemoryAdapter::forward(const Tensor& logits, const Tensor& evidence,
    const BooleanMask& mask) const {
    tensor_ok(logits); const auto shape = logits.shape();
    if (shape.size() != 4 || shape[3] != vocab) throw std::invalid_argument("logits must have shape [B,P,S,V]");
    auto flat = make_like(logits, {shape[0], shape[1] * shape[2], shape[3]},
        {logits.values().begin(), logits.values().end()});
    auto result = linear(gelu(attention(linear(flat, weights_.query, dim_), evidence, mask,
        heads_, weights_.cross_attention)), weights_.output, vocab);
    return make_like(logits, {shape.begin(), shape.end()}, {result.values().begin(), result.values().end()});
}

TextOnlyHiddenCrossMemoryAdapter::TextOnlyHiddenCrossMemoryAdapter(const std::size_t model_dim,
    const std::size_t attention_heads, TextOnlyHiddenCrossMemoryWeights weights)
    : dim_(model_dim), heads_(attention_heads), weights_(std::move(weights)) {
    if (!dim_ || !heads_ || dim_ % heads_) throw std::invalid_argument("invalid hidden cross-memory dimensions");
    norm_ok(weights_.query_norm, dim_); linear_ok(weights_.query, dim_, dim_);
    attention_ok(weights_.cross_attention, dim_); linear_ok(weights_.output, dim_, vocab);
}
Tensor TextOnlyHiddenCrossMemoryAdapter::forward(const Tensor& decoder, const Tensor& evidence,
    const BooleanMask& mask) const {
    tensor_ok(decoder); const auto shape = decoder.shape();
    if (shape.size() != 4 || shape[3] != dim_) throw std::invalid_argument("decoder states must have shape [B,P,S,D]");
    auto flat = make_like(decoder, {shape[0], shape[1] * shape[2], shape[3]},
        {decoder.values().begin(), decoder.values().end()});
    auto query = linear(norm(flat, weights_.query_norm), weights_.query, dim_);
    auto result = linear(gelu(attention(query, evidence, mask, heads_, weights_.cross_attention)),
        weights_.output, vocab);
    return make_like(decoder, {shape[0], shape[1], shape[2], vocab},
        {result.values().begin(), result.values().end()});
}

TextEpistemicOutputAdapter::TextEpistemicOutputAdapter(const std::size_t model_dim,
    const std::size_t rank, TextEpistemicOutputWeights weights)
    : dim_(model_dim), rank_(rank), weights_(std::move(weights)) {
    if (!dim_ || !rank_) throw std::invalid_argument("epistemic adapter dimensions must be positive");
    linear_ok(weights_.down, dim_, rank_, false); linear_ok(weights_.output, rank_, vocab);
}
Tensor TextEpistemicOutputAdapter::forward(const Tensor& decoder,
    const std::span<const std::uint8_t> active) const {
    tensor_ok(decoder); const auto shape = decoder.shape();
    if (shape.size() != 4 || shape[3] != dim_ || active.size() != shape[0])
        throw std::invalid_argument("epistemic adapter input shape mismatch");
    for (const auto value : active) if (value > 1) throw std::invalid_argument("active values must be boolean");
    auto result = linear(gelu(linear(decoder, weights_.down, rank_)), weights_.output, vocab);
    auto values = std::vector<double>(result.values().begin(), result.values().end());
    if (active.empty()) return make_like(decoder, {shape[0], shape[1], shape[2], vocab}, std::move(values));
    const auto row = values.size() / active.size();
    for (std::size_t b = 0; b < active.size(); ++b)
        for (std::size_t index = 0; index < row; ++index)
            values[b * row + index] *= static_cast<double>(active[b]);
    return make_like(decoder, {shape[0], shape[1], shape[2], vocab}, std::move(values));
}

void TextAnswerabilityConfig::validate() const {
    if (!world_dim || !attention_heads || world_dim % attention_heads || !output_classes)
        throw std::invalid_argument("invalid answerability dimensions");
}
TextAnswerabilityVerifier::TextAnswerabilityVerifier(TextAnswerabilityConfig config,
    TextAnswerabilityWeights weights) : config_([&config] {
        if (!config.source_dim) config.source_dim = config.world_dim; // Python source_dim=None
        return config;
    }()), weights_(std::move(weights)) {
    config_.validate(); const auto& c = config_;
    const bool projected = c.source_dim != c.world_dim;
    if (projected != (weights_.input_norm.has_value() && weights_.input_projection.has_value()))
        throw std::invalid_argument("answerability input projection topology mismatch");
    if (projected) { norm_ok(*weights_.input_norm, c.source_dim); linear_ok(*weights_.input_projection, c.source_dim, c.world_dim); }
    if (c.contextual != weights_.context_encoder.has_value())
        throw std::invalid_argument("answerability context encoder topology mismatch");
    if (c.contextual) {
        const auto& e = *weights_.context_encoder; norm_ok(e.norm1, c.world_dim); norm_ok(e.norm2, c.world_dim);
        attention_ok(e.attention, c.world_dim); linear_ok(e.feedforward_in, c.world_dim, c.world_dim * 2);
        linear_ok(e.feedforward_out, c.world_dim * 2, c.world_dim);
    }
    norm_ok(weights_.question_norm, c.world_dim); norm_ok(weights_.evidence_norm, c.world_dim);
    attention_ok(weights_.cross_attention, c.world_dim); norm_ok(weights_.token_norm, c.world_dim * 4);
    linear_ok(weights_.token_projection, c.world_dim * 4, c.world_dim);
    const auto final_dim = c.world_dim * (c.evidence_consistency ? 4 : 2) +
        (c.evidence_consistency ? 3 : 2) + c.extra_feature_dim;
    norm_ok(weights_.output_norm, final_dim); linear_ok(weights_.output_hidden, final_dim, c.world_dim);
    linear_ok(weights_.output, c.world_dim, c.output_classes);
}

Tensor TextAnswerabilityVerifier::forward(const Tensor& raw_question, const BooleanMask& question_mask,
    const Tensor& raw_evidence, const BooleanMask& evidence_mask,
    const BooleanMask* title_mask, const BooleanMask* body_mask, const Tensor* extra) const {
    tensor_ok(raw_question); tensor_ok(raw_evidence); const auto qs = raw_question.shape(), es = raw_evidence.shape();
    if (qs.size() != 3 || es.size() != 3 || qs[0] != es[0] || qs[2] != config_.source_dim || es[2] != config_.source_dim)
        throw std::invalid_argument("answerability state shape mismatch");
    if (question_mask.shape().size() != 2 || question_mask.shape()[0] != qs[0] || question_mask.shape()[1] != qs[1] ||
        evidence_mask.shape().size() != 2 || evidence_mask.shape()[0] != es[0] || evidence_mask.shape()[1] != es[1])
        throw std::invalid_argument("answerability mask shape mismatch");
    auto question_states = config_.source_dim == config_.world_dim ? raw_question.clone()
        : linear(norm(raw_question, *weights_.input_norm), *weights_.input_projection, config_.world_dim);
    auto evidence_states = config_.source_dim == config_.world_dim ? raw_evidence.clone()
        : linear(norm(raw_evidence, *weights_.input_norm), *weights_.input_projection, config_.world_dim);
    BooleanMask body_safe = evidence_mask;
    if (config_.question_body_only) {
        if (!body_mask) throw std::invalid_argument("body mask is required for body-only verification");
        body_safe = safe_mask(*body_mask);
    }
    if (config_.contextual) {
        question_states = encoder(add_positions(question_states), question_mask, config_.attention_heads,
            *weights_.context_encoder);
        evidence_states = encoder(add_positions(evidence_states), config_.question_body_only ? body_safe : evidence_mask,
            config_.attention_heads, *weights_.context_encoder);
    }
    const auto question = norm(question_states, weights_.question_norm);
    const auto evidence = norm(evidence_states, weights_.evidence_norm);
    const auto context = attention(question, evidence, config_.question_body_only ? body_safe : evidence_mask,
        config_.attention_heads, weights_.cross_attention);
    const auto batch = static_cast<std::size_t>(qs[0]), qtokens = static_cast<std::size_t>(qs[1]), dim = config_.world_dim;
    std::vector<double> interactions(batch * qtokens * dim * 4);
    for (std::size_t row = 0; row < batch * qtokens; ++row) for (std::size_t d = 0; d < dim; ++d) {
        const auto q = question.values()[row * dim + d], x = context.values()[row * dim + d];
        interactions[row * dim * 4 + d] = q; interactions[row * dim * 4 + dim + d] = x;
        interactions[row * dim * 4 + dim * 2 + d] = std::abs(q - x);
        interactions[row * dim * 4 + dim * 3 + d] = q * x;
    }
    auto token = linear(norm(make_like(question, {qs[0], qs[1], dim * 4}, std::move(interactions)),
        weights_.token_norm), weights_.token_projection, dim); token = gelu(std::move(token));
    auto [qmean, qmax] = pool(token, question_mask);
    std::vector<double> feature; feature.reserve(batch * (dim * (config_.evidence_consistency ? 4 : 2) + 3 + config_.extra_feature_dim));
    std::vector<double> emean, emax;
    if (config_.evidence_consistency) {
        if (!title_mask || !body_mask) throw std::invalid_argument("evidence consistency masks are required");
        const auto safe_body = safe_mask(*body_mask);
        const auto title_context = attention(evidence, evidence, safe_body, config_.attention_heads, weights_.cross_attention);
        std::vector<double> joined(evidence.values().size() * 4);
        for (std::size_t row = 0; row < evidence.values().size() / dim; ++row) for (std::size_t d = 0; d < dim; ++d) {
            const auto e = evidence.values()[row * dim + d], x = title_context.values()[row * dim + d];
            joined[row * dim * 4 + d] = e; joined[row * dim * 4 + dim + d] = x;
            joined[row * dim * 4 + 2 * dim + d] = std::abs(e - x); joined[row * dim * 4 + 3 * dim + d] = e * x;
        }
        auto projected = gelu(linear(norm(make_like(evidence, {es[0], es[1], dim * 4}, std::move(joined)), weights_.token_norm), weights_.token_projection, dim));
        auto pooled = pool(projected, *title_mask); emean = std::move(pooled.first); emax = std::move(pooled.second);
    }
    const auto density = [](const BooleanMask& mask, const std::size_t b) {
        double sum = 0.0; for (std::size_t t = 0; t < mask.shape()[1]; ++t) sum += mask.at(b, t); return sum / mask.shape()[1];
    };
    const auto final_dim = dim * (config_.evidence_consistency ? 4 : 2) + (config_.evidence_consistency ? 3 : 2) + config_.extra_feature_dim;
    feature.resize(batch * final_dim);
    if (config_.extra_feature_dim && (!extra || extra->shape().size() != 2 || extra->shape()[0] != qs[0] || extra->shape()[1] != config_.extra_feature_dim))
        throw std::invalid_argument("answerability extra features have wrong shape");
    for (std::size_t b = 0; b < batch; ++b) {
        auto out = feature.begin() + static_cast<std::ptrdiff_t>(b * final_dim);
        out = std::copy_n(qmean.begin() + static_cast<std::ptrdiff_t>(b * dim), dim, out);
        out = std::copy_n(qmax.begin() + static_cast<std::ptrdiff_t>(b * dim), dim, out);
        if (config_.evidence_consistency) {
            out = std::copy_n(emean.begin() + static_cast<std::ptrdiff_t>(b * dim), dim, out);
            out = std::copy_n(emax.begin() + static_cast<std::ptrdiff_t>(b * dim), dim, out);
            *out++ = density(question_mask, b); *out++ = density(*title_mask, b); *out++ = density(*body_mask, b);
        } else { *out++ = density(question_mask, b); *out++ = density(config_.question_body_only ? *body_mask : evidence_mask, b); }
        if (config_.extra_feature_dim)
            std::copy_n(extra->values().begin() + static_cast<std::ptrdiff_t>(b * config_.extra_feature_dim), config_.extra_feature_dim, out);
    }
    auto combined = make_like(question, {qs[0], final_dim}, std::move(feature));
    return linear(gelu(linear(norm(combined, weights_.output_norm), weights_.output_hidden, dim)),
        weights_.output, config_.output_classes);
}

} // namespace swegca::world
