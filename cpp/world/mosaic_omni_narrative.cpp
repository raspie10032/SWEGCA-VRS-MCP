#include "world/mosaic_omni_narrative.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <stdexcept>
#include <utility>

namespace swegca::world {
namespace {

struct Patches final {
    std::size_t batch{}, count{}, size{};
    std::vector<std::int64_t> values;
    std::vector<std::uint8_t> active;
};

void validate_text_batch(const MosaicTokenBatch& rows, const char* kind) {
    if (rows.empty()) throw std::invalid_argument(std::string(kind) + " inputs must be nonempty");
    const auto width = rows.front().size();
    if (width < 2) throw std::invalid_argument(std::string(kind) + " inputs must be BOS-prefixed text batches");
    for (const auto& row : rows)
        if (row.size() != width || row.front() != mosaic_bos_id)
            throw std::invalid_argument(std::string(kind) + " inputs must be BOS-prefixed text batches");
}

Patches patch_body(const MosaicTokenBatch& rows, const std::size_t patch_size, const char* kind) {
    validate_text_batch(rows, kind);
    const auto body = rows.front().size() - 1;
    const auto patches = (body + patch_size - 1) / patch_size;
    Patches result{rows.size(), patches, patch_size,
        std::vector<std::int64_t>(rows.size() * patches * patch_size, mosaic_pad_id),
        std::vector<std::uint8_t>(rows.size() * patches)};
    for (std::size_t batch = 0; batch < rows.size(); ++batch) {
        for (std::size_t index = 0; index < body; ++index)
            result.values[(batch * patches * patch_size) + index] = rows[batch][index + 1];
        for (std::size_t patch = 0; patch < patches; ++patch)
            for (std::size_t offset = 0; offset < patch_size; ++offset)
                result.active[batch * patches + patch] |=
                    result.values[(batch * patches + patch) * patch_size + offset] != mosaic_pad_id;
        if (std::none_of(result.active.begin() + static_cast<std::ptrdiff_t>(batch * patches),
                result.active.begin() + static_cast<std::ptrdiff_t>((batch + 1) * patches),
                [](const auto value) { return value != 0; }))
            throw std::invalid_argument(std::string("every ") + kind + " input must contain text");
    }
    return result;
}

std::vector<double> normalize_rows(std::vector<double> values, const std::size_t rows,
                                   const std::size_t columns) {
    for (std::size_t row = 0; row < rows; ++row) {
        double squared = 0.0;
        for (std::size_t column = 0; column < columns; ++column) {
            const auto value = values[row * columns + column]; squared += value * value;
        }
        const auto divisor = std::max(std::sqrt(squared), 1.0e-12);
        for (std::size_t column = 0; column < columns; ++column)
            values[row * columns + column] /= divisor;
    }
    return values;
}

double gelu(const double value) {
    return value * 0.5 * (1.0 + std::erf(value / std::sqrt(2.0)));
}

} // namespace

void NarrativeDescriptorProjection::validate(const std::size_t text_dim) const {
    if (!world_dim || weight.size() != world_dim * text_dim || bias.size() != world_dim)
        throw std::invalid_argument("narrative descriptor projection shape mismatch");
}

void NarrativeContinuityWeights::validate(const std::size_t text_dim) const {
    if (!hidden_dim || !world_dim || hidden_weight.size() != hidden_dim * text_dim * 8 ||
        hidden_bias.size() != hidden_dim || score_weight.size() != hidden_dim || score_bias.size() != 1 ||
        world_delta_weight.size() != world_dim)
        throw std::invalid_argument("narrative continuity weight shape mismatch");
}
std::vector<float> NarrativeContinuityWeights::zero_world_delta(const std::size_t world_dim) {
    if (!world_dim) throw std::invalid_argument("narrative world dimension must be positive");
    return std::vector<float>(world_dim, 0.0F);
}

Tensor encode_text_descriptor(const MosaicTextLM& text_core, const MosaicTokenBatch& input_ids,
    const NarrativeDescriptorProjection& projection) {
    const auto& config = text_core.config(); const auto& weights = text_core.weights();
    projection.validate(config.model_dim);
    const auto patches = patch_body(input_ids, config.patch_size, "descriptor");
    const auto flattened = config.patch_size * config.byte_embedding_dim;
    std::vector<double> encoded(patches.batch * patches.count * config.model_dim);
    for (std::size_t batch = 0; batch < patches.batch; ++batch)
        for (std::size_t patch = 0; patch < patches.count; ++patch) {
            std::vector<double> projected(config.model_dim);
            for (std::size_t out = 0; out < config.model_dim; ++out) {
                double value = weights.patch_projection_bias[out];
                for (std::size_t offset = 0; offset < config.patch_size; ++offset) {
                    const auto token = patches.values[(batch * patches.count + patch) * config.patch_size + offset];
                    if (token < 0 || static_cast<std::size_t>(token) >= static_cast<std::size_t>(mosaic_vocab_size))
                        throw std::invalid_argument("descriptor token is outside the vocabulary");
                    for (std::size_t dim = 0; dim < config.byte_embedding_dim; ++dim) {
                        const auto input = offset * config.byte_embedding_dim + dim;
                        value += weights.byte_embedding[static_cast<std::size_t>(token) * config.byte_embedding_dim + dim]
                            * weights.patch_projection_weight[out * flattened + input];
                    }
                }
                projected[out] = value;
            }
            double mean = 0.0; for (const auto value : projected) mean += value; mean /= config.model_dim;
            double variance = 0.0; for (const auto value : projected) { const auto difference = value - mean; variance += difference * difference; }
            const auto inverse = 1.0 / std::sqrt(variance / config.model_dim + 1.0e-5);
            for (std::size_t dim = 0; dim < config.model_dim; ++dim)
                encoded[(batch * patches.count + patch) * config.model_dim + dim] =
                    (projected[dim] - mean) * inverse * weights.patch_norm_weight[dim] + weights.patch_norm_bias[dim];
        }
    std::vector<double> pooled(patches.batch * projection.world_dim);
    for (std::size_t batch = 0; batch < patches.batch; ++batch) {
        std::size_t active = 0;
        for (std::size_t patch = 0; patch < patches.count; ++patch) if (patches.active[batch * patches.count + patch]) {
            ++active;
            for (std::size_t out = 0; out < projection.world_dim; ++out) {
                double value = projection.bias[out];
                for (std::size_t dim = 0; dim < config.model_dim; ++dim)
                    value += encoded[(batch * patches.count + patch) * config.model_dim + dim] *
                        projection.weight[out * config.model_dim + dim];
                pooled[batch * projection.world_dim + out] += value;
            }
        }
        for (std::size_t out = 0; out < projection.world_dim; ++out)
            pooled[batch * projection.world_dim + out] /= std::max<std::size_t>(active, 1);
    }
    return Tensor(TensorDType::float32, {patches.batch, projection.world_dim}, std::move(pooled), "cpu");
}

Tensor narrative_text_summary(const MosaicTextLM& text_core, const MosaicTokenBatch& input_ids) {
    const auto& config = text_core.config(); const auto patches = patch_body(input_ids, config.patch_size, "narrative");
    const auto output = text_core.forward(input_ids, nullptr, std::optional<std::size_t>{1});
    const auto& states = output.context_states; const auto shape = states.shape();
    if (shape.size() != 3 || shape[0] != patches.batch || shape[1] != patches.count || shape[2] != config.model_dim)
        throw std::runtime_error("text core did not return contextual states");
    std::vector<double> mean(patches.batch * config.model_dim), last(patches.batch * config.model_dim);
    for (std::size_t batch = 0; batch < patches.batch; ++batch) {
        std::size_t active = 0;
        for (std::size_t patch = 0; patch < patches.count; ++patch) if (patches.active[batch * patches.count + patch]) {
            ++active;
            for (std::size_t dim = 0; dim < config.model_dim; ++dim)
                mean[batch * config.model_dim + dim] += states.values()[(batch * patches.count + patch) * config.model_dim + dim];
        }
        const auto last_index = active - 1; // source indexes by patch_mask.sum() - 1
        for (std::size_t dim = 0; dim < config.model_dim; ++dim) {
            mean[batch * config.model_dim + dim] /= std::max<std::size_t>(active, 1);
            last[batch * config.model_dim + dim] = states.values()[(batch * patches.count + last_index) * config.model_dim + dim];
        }
    }
    mean = normalize_rows(std::move(mean), patches.batch, config.model_dim);
    last = normalize_rows(std::move(last), patches.batch, config.model_dim);
    std::vector<double> summary(patches.batch * config.model_dim * 2);
    for (std::size_t batch = 0; batch < patches.batch; ++batch) {
        std::copy_n(mean.begin() + static_cast<std::ptrdiff_t>(batch * config.model_dim), config.model_dim,
            summary.begin() + static_cast<std::ptrdiff_t>(batch * config.model_dim * 2));
        std::copy_n(last.begin() + static_cast<std::ptrdiff_t>(batch * config.model_dim), config.model_dim,
            summary.begin() + static_cast<std::ptrdiff_t>(batch * config.model_dim * 2 + config.model_dim));
    }
    return Tensor(TensorDType::float32, {patches.batch, config.model_dim * 2}, std::move(summary), "cpu");
}

NarrativeContinuityScorer::NarrativeContinuityScorer(const MosaicTextLM& text_core,
    NarrativeContinuityWeights weights) : text_core_(&text_core), weights_(std::move(weights)) {
    weights_.validate(text_core.config().model_dim);
}

NarrativeContinuityOutput NarrativeContinuityScorer::score_narrative_continuity(
    const MosaicTokenBatch& anchors, const MosaicTokenBatch& candidates) const {
    if (anchors.size() != candidates.size()) throw std::invalid_argument("anchor and candidate batches must match");
    const auto anchor = narrative_text_summary(*text_core_, anchors);
    const auto candidate = narrative_text_summary(*text_core_, candidates);
    const auto batch = anchors.size(), summary_dim = text_core_->config().model_dim * 2;
    std::vector<double> features(batch * summary_dim * 4);
    for (std::size_t row = 0; row < batch; ++row) for (std::size_t dim = 0; dim < summary_dim; ++dim) {
        const auto a = anchor.values()[row * summary_dim + dim], c = candidate.values()[row * summary_dim + dim];
        features[row * summary_dim * 4 + dim] = a;
        features[row * summary_dim * 4 + summary_dim + dim] = c;
        features[row * summary_dim * 4 + summary_dim * 2 + dim] = std::abs(a - c);
        features[row * summary_dim * 4 + summary_dim * 3 + dim] = a * c;
    }
    std::vector<double> scores(batch), deltas(batch * weights_.world_dim);
    for (std::size_t row = 0; row < batch; ++row) {
        std::vector<double> hidden(weights_.hidden_dim);
        for (std::size_t out = 0; out < weights_.hidden_dim; ++out) {
            double value = weights_.hidden_bias[out];
            for (std::size_t in = 0; in < summary_dim * 4; ++in)
                value += features[row * summary_dim * 4 + in] * weights_.hidden_weight[out * summary_dim * 4 + in];
            hidden[out] = gelu(value);
        }
        double score = weights_.score_bias[0];
        for (std::size_t hidden_index = 0; hidden_index < weights_.hidden_dim; ++hidden_index)
            score += hidden[hidden_index] * weights_.score_weight[hidden_index];
        scores[row] = score;
        for (std::size_t out = 0; out < weights_.world_dim; ++out)
            deltas[row * weights_.world_dim + out] = score * weights_.world_delta_weight[out];
    }
    return {Tensor(TensorDType::float32, {batch}, std::move(scores), "cpu"),
        Tensor(TensorDType::float32, {batch, weights_.world_dim}, std::move(deltas), "cpu")};
}

} // namespace swegca::world
