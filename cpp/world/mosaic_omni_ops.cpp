#include "world/mosaic_omni.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <stdexcept>

namespace swegca::world {
namespace {

std::size_t count(const std::span<const std::uint64_t> shape) {
    std::size_t result = 1;
    for (const auto value : shape) {
        if (value && result > std::numeric_limits<std::size_t>::max() / value)
            throw std::overflow_error("omni tensor size overflow");
        result *= value;
    }
    return result;
}

void require_storage(const Tensor& value) {
    if (count(value.shape()) != value.values().size())
        throw std::invalid_argument("omni tensor storage size mismatch");
}

Tensor tensor(const TensorDType dtype, std::vector<std::uint64_t> shape,
              std::vector<double> values) {
    return Tensor(dtype, std::move(shape), std::move(values), "cpu");
}

void rectangular(const MosaicTokenBatch& rows, const char* name) {
    if (rows.empty() || rows.front().empty())
        throw std::invalid_argument(std::string(name) + " must be nonempty");
    for (const auto& row : rows) if (row.size() != rows.front().size())
        throw std::invalid_argument(std::string(name) + " must be rectangular");
}

std::vector<double> unit(const std::span<const double> row) {
    double squared = 0.0;
    for (const auto value : row) squared += value * value;
    const auto scale = std::max(std::sqrt(squared), 1.0e-12);
    std::vector<double> result; result.reserve(row.size());
    for (const auto value : row) result.push_back(value / scale);
    return result;
}

std::vector<double> softmax(const std::vector<double>& scores) {
    const auto maximum = *std::max_element(scores.begin(), scores.end());
    std::vector<double> result(scores.size());
    double denominator = 0.0;
    for (std::size_t index = 0; index < scores.size(); ++index) {
        result[index] = std::isfinite(scores[index]) ? std::exp(scores[index] - maximum) : 0.0;
        denominator += result[index];
    }
    if (denominator) for (auto& value : result) value /= denominator;
    return result;
}

}  // namespace

MosaicTokenBatch compact_body_input_ids(const MosaicTokenBatch& input_ids) {
    rectangular(input_ids, "input_ids");
    MosaicTokenBatch result(input_ids.size(),
        std::vector<std::int64_t>(input_ids.front().size(), mosaic_pad_id));
    for (std::size_t batch = 0; batch < input_ids.size(); ++batch) {
        result[batch][0] = input_ids[batch][0];
        bool body = false; std::size_t output = 1;
        for (std::size_t index = 1; index < input_ids[batch].size(); ++index) {
            body = body || input_ids[batch][index] == '\n';
            if (body && input_ids[batch][index] != mosaic_pad_id)
                result[batch][output++] = input_ids[batch][index];
        }
    }
    return result;
}

Tensor byte_ngram_overlap_features(
    const MosaicTokenBatch& question_input_ids,
    const MosaicTokenBatch& evidence_input_ids,
    const std::span<const std::size_t> widths) {
    rectangular(question_input_ids, "question_input_ids");
    rectangular(evidence_input_ids, "evidence_input_ids");
    if (question_input_ids.size() != evidence_input_ids.size())
        throw std::invalid_argument("question and evidence batches must match");
    std::vector<double> output(question_input_ids.size() * widths.size());
    for (std::size_t batch = 0; batch < question_input_ids.size(); ++batch) {
        const auto& question = question_input_ids[batch]; const auto& evidence = evidence_input_ids[batch];
        for (std::size_t width_index = 0; width_index < widths.size(); ++width_index) {
            const auto width = widths[width_index];
            if (!width || question.size() - 1 < width || evidence.size() - 1 < width) continue;
            std::vector<std::uint64_t> evidence_hashes;
            for (std::size_t begin = 1; begin + width <= evidence.size(); ++begin) {
                std::uint64_t hash = 0; bool valid = true;
                for (std::size_t offset = 0; offset < width; ++offset) {
                    const auto value = evidence[begin + offset]; valid = valid && value <= 255;
                    hash = hash * 257U + static_cast<std::uint64_t>(value + 1);
                }
                if (valid) evidence_hashes.push_back(hash);
            }
            std::size_t valid_count = 0, matched = 0;
            for (std::size_t begin = 1; begin + width <= question.size(); ++begin) {
                std::uint64_t hash = 0; bool valid = true;
                for (std::size_t offset = 0; offset < width; ++offset) {
                    const auto value = question[begin + offset]; valid = valid && value <= 255;
                    hash = hash * 257U + static_cast<std::uint64_t>(value + 1);
                }
                if (valid) {
                    ++valid_count;
                    matched += std::ranges::find(evidence_hashes, hash) != evidence_hashes.end();
                }
            }
            output[batch * widths.size() + width_index] =
                static_cast<double>(matched) / std::max<std::size_t>(valid_count, 1);
        }
    }
    return tensor(TensorDType::float32,
        {question_input_ids.size(), widths.size()}, std::move(output));
}

Tensor time_center_object_frame_grid(const Tensor& frame_grid) {
    require_storage(frame_grid);
    const auto shape = frame_grid.shape();
    if (shape.size() != 3) throw std::invalid_argument("object frame grid must be [B*S,F,D]");
    const auto rows = static_cast<std::size_t>(shape[0]), frames = static_cast<std::size_t>(shape[1]);
    const auto dimension = static_cast<std::size_t>(shape[2]);
    std::vector<double> output(frame_grid.values().begin(), frame_grid.values().end());
    for (std::size_t row = 0; row < rows; ++row)
        for (std::size_t dim = 0; dim < dimension; ++dim) {
            double mean = 0.0;
            for (std::size_t frame = 0; frame < frames; ++frame)
                mean += frame_grid.values()[(row * frames + frame) * dimension + dim];
            mean /= frames;
            for (std::size_t frame = 0; frame < frames; ++frame)
                output[(row * frames + frame) * dimension + dim] -= mean;
        }
    return tensor(frame_grid.dtype(), {shape.begin(), shape.end()}, std::move(output));
}

Tensor select_object_temporal_evidence(const Tensor& frame_grid, const bool time_centered,
                                       const bool dual_evidence, const std::size_t raw_rows) {
    if (!time_centered) return frame_grid.clone();
    auto centered = time_center_object_frame_grid(frame_grid);
    if (!dual_evidence) return centered;
    if (!raw_rows || raw_rows >= frame_grid.shape()[0])
        throw std::invalid_argument("dual object evidence requires raw and normalized rows");
    auto values = centered.values();
    std::vector<double> output(values.begin(), values.end());
    const auto row_size = static_cast<std::size_t>(frame_grid.shape()[1] * frame_grid.shape()[2]);
    std::copy_n(frame_grid.values().begin(), raw_rows * row_size, output.begin());
    return tensor(frame_grid.dtype(), {frame_grid.shape().begin(), frame_grid.shape().end()}, std::move(output));
}

Tensor normalize_object_frontend_frames(const Tensor& frames) {
    require_storage(frames); const auto shape = frames.shape();
    if (shape.size() != 4) throw std::invalid_argument("object frontend frames must be [B,C,H,W]");
    const auto batch = static_cast<std::size_t>(shape[0]), channels = static_cast<std::size_t>(shape[1]);
    const auto height = static_cast<std::size_t>(shape[2]), width = static_cast<std::size_t>(shape[3]);
    std::vector<double> output(frames.values().size()); const auto pixels = height * width;
    for (std::size_t b = 0; b < batch; ++b) for (std::size_t c = 0; c < channels; ++c) {
        const auto base = (b * channels + c) * pixels;
        const auto begin = frames.values().begin() + base;
        const auto mean = std::accumulate(begin, begin + pixels, 0.0) / pixels;
        double variance = 0.0; for (std::size_t i = 0; i < pixels; ++i) {
            const auto delta = frames.values()[base + i] - mean; variance += delta * delta;
        }
        const auto scale = std::max(std::sqrt(variance / pixels), 1.0e-4);
        for (std::size_t i = 0; i < pixels; ++i) output[base + i] = (frames.values()[base + i] - mean) / scale;
    }
    return tensor(frames.dtype(), {shape.begin(), shape.end()}, std::move(output));
}

Tensor camera_invariant_object_frames(const Tensor& frames) {
    require_storage(frames); const auto shape = frames.shape();
    if (shape.size() != 4 || shape[1] != 3)
        throw std::invalid_argument("camera-invariant frames must be [B,3,H,W]");
    const auto batch = static_cast<std::size_t>(shape[0]), height = static_cast<std::size_t>(shape[2]);
    const auto width = static_cast<std::size_t>(shape[3]), pixels = height * width;
    std::vector<double> output(batch * 3 * pixels);
    for (std::size_t b = 0; b < batch; ++b) {
        std::vector<double> luminance(pixels); double mean = 0.0;
        for (std::size_t i = 0; i < pixels; ++i) {
            luminance[i] = 0.299 * frames.values()[(b * 3) * pixels + i] +
                0.587 * frames.values()[(b * 3 + 1) * pixels + i] +
                0.114 * frames.values()[(b * 3 + 2) * pixels + i]; mean += luminance[i];
        }
        mean /= pixels; double variance = 0.0;
        for (const auto value : luminance) { const auto delta = value - mean; variance += delta * delta; }
        const auto scale = std::max(std::sqrt(variance / pixels), 1.0e-4);
        for (std::size_t y = 0; y < height; ++y) for (std::size_t x = 0; x < width; ++x) {
            const auto i = y * width + x; const auto value = (luminance[i] - mean) / scale;
            output[(b * 3) * pixels + i] = value;
            output[(b * 3 + 1) * pixels + i] = x ? value - output[(b * 3) * pixels + i - 1] : 0.0;
            output[(b * 3 + 2) * pixels + i] = y ? value - output[(b * 3) * pixels + i - width] : 0.0;
        }
    }
    return tensor(frames.dtype(), {shape[0], 3, shape[2], shape[3]}, std::move(output));
}

Tensor video_camera_statistics(const Tensor& video) {
    require_storage(video); const auto shape = video.shape();
    if (shape.size() != 5 || shape[2] != 3) throw std::invalid_argument("video must be [B,F,3,H,W]");
    const auto batch = static_cast<std::size_t>(shape[0]), frames = static_cast<std::size_t>(shape[1]);
    const auto height = static_cast<std::size_t>(shape[3]), width = static_cast<std::size_t>(shape[4]);
    const auto pixels = height * width; std::vector<double> output(batch * 8);
    for (std::size_t b = 0; b < batch; ++b) {
        for (std::size_t c = 0; c < 3; ++c) {
            double mean = 0.0;
            for (std::size_t f = 0; f < frames; ++f) for (std::size_t i = 0; i < pixels; ++i)
                mean += video.values()[((b * frames + f) * 3 + c) * pixels + i];
            mean /= frames * pixels; output[b * 8 + c] = mean; double variance = 0.0;
            for (std::size_t f = 0; f < frames; ++f) for (std::size_t i = 0; i < pixels; ++i) {
                const auto delta = video.values()[((b * frames + f) * 3 + c) * pixels + i] - mean;
                variance += delta * delta;
            }
            output[b * 8 + 3 + c] = std::sqrt(variance / (frames * pixels));
        }
        double horizontal = 0.0, vertical = 0.0; std::size_t hc = 0, vc = 0;
        for (std::size_t f = 0; f < frames; ++f) for (std::size_t y = 0; y < height; ++y)
            for (std::size_t x = 0; x < width; ++x) {
                const auto luma = [&](const std::size_t yy, const std::size_t xx) {
                    double value = 0.0; for (std::size_t c = 0; c < 3; ++c)
                        value += video.values()[((b * frames + f) * 3 + c) * pixels + yy * width + xx];
                    return value / 3.0;
                };
                if (x) { horizontal += std::abs(luma(y, x) - luma(y, x - 1)); ++hc; }
                if (y) { vertical += std::abs(luma(y, x) - luma(y - 1, x)); ++vc; }
            }
        output[b * 8 + 6] = hc ? horizontal / hc : 0.0;
        output[b * 8 + 7] = vc ? vertical / vc : 0.0;
    }
    return tensor(video.dtype(), {shape[0], 8}, std::move(output));
}

std::pair<Tensor, Tensor> cross_modal_late_summaries(
    const Tensor& text_tokens, const BooleanMask& text_mask, const Tensor& video_tokens) {
    require_storage(text_tokens); require_storage(video_tokens);
    const auto ts = text_tokens.shape(), vs = video_tokens.shape();
    if (ts.size() != 3 || vs.size() != 3 || ts[0] != vs[0] || ts[2] != vs[2] ||
        text_mask.shape().size() != 2 || text_mask.shape()[0] != ts[0] || text_mask.shape()[1] != ts[1])
        throw std::invalid_argument("cross-modal summaries require aligned [B,T,D]/[B,V,D]");
    const auto batch = static_cast<std::size_t>(ts[0]), texts = static_cast<std::size_t>(ts[1]);
    const auto videos = static_cast<std::size_t>(vs[1]), dim = static_cast<std::size_t>(ts[2]);
    std::vector<double> text_out(batch * dim), video_out(batch * dim);
    for (std::size_t b = 0; b < batch; ++b) {
        std::vector<std::vector<double>> tn(texts), vn(videos);
        for (std::size_t t = 0; t < texts; ++t) tn[t] = unit(text_tokens.values().subspan((b * texts + t) * dim, dim));
        for (std::size_t v = 0; v < videos; ++v) vn[v] = unit(video_tokens.values().subspan((b * videos + v) * dim, dim));
        std::vector<double> text_scores(texts, -std::numeric_limits<double>::infinity());
        std::vector<double> video_scores(videos, -std::numeric_limits<double>::infinity());
        for (std::size_t t = 0; t < texts; ++t) if (text_mask.at(b, t))
            for (std::size_t v = 0; v < videos; ++v) {
                double similarity = 0.0; for (std::size_t d = 0; d < dim; ++d) similarity += tn[t][d] * vn[v][d];
                text_scores[t] = std::max(text_scores[t], similarity); video_scores[v] = std::max(video_scores[v], similarity);
            }
        const auto tw = softmax(text_scores), vw = softmax(video_scores);
        for (std::size_t d = 0; d < dim; ++d) {
            for (std::size_t t = 0; t < texts; ++t) text_out[b * dim + d] += tn[t][d] * tw[t];
            for (std::size_t v = 0; v < videos; ++v) video_out[b * dim + d] += vn[v][d] * vw[v];
        }
    }
    return {tensor(text_tokens.dtype(), {ts[0], ts[2]}, std::move(text_out)),
            tensor(video_tokens.dtype(), {vs[0], vs[2]}, std::move(video_out))};
}

void CrossModalQuerySummaryWeights::validate(const std::size_t dimension) const {
    if (!dimension || score_weight.size() != dimension)
        throw std::invalid_argument("cross-modal query score weight shape mismatch");
}

Tensor cross_modal_query_summary(
    const Tensor& tokens, const BooleanMask& mask,
    const CrossModalQuerySummaryWeights& weights) {
    require_storage(tokens); const auto shape = tokens.shape();
    if (shape.size() != 3 || mask.shape().size() != 2 ||
        mask.shape()[0] != shape[0] || mask.shape()[1] != shape[1])
        throw std::invalid_argument("query summary expects [B,T,D] tokens and [B,T] mask");
    const auto batch = static_cast<std::size_t>(shape[0]);
    const auto count_ = static_cast<std::size_t>(shape[1]);
    const auto dim = static_cast<std::size_t>(shape[2]); weights.validate(dim);
    std::vector<double> output(batch * dim);
    for (std::size_t b = 0; b < batch; ++b) {
        std::vector<std::vector<double>> normalized(count_);
        std::vector<double> logits(count_, -std::numeric_limits<double>::infinity());
        bool active = false;
        for (std::size_t t = 0; t < count_; ++t) {
            normalized[t] = unit(tokens.values().subspan((b * count_ + t) * dim, dim));
            if (!mask.at(b, t)) continue;
            active = true; logits[t] = weights.score_bias;
            for (std::size_t d = 0; d < dim; ++d)
                logits[t] += normalized[t][d] * weights.score_weight[d];
        }
        if (!active) throw std::invalid_argument("query summary requires at least one active token");
        const auto probabilities = softmax(logits);
        for (std::size_t t = 0; t < count_; ++t) for (std::size_t d = 0; d < dim; ++d)
            output[b * dim + d] += normalized[t][d] * probabilities[t];
    }
    return tensor(tokens.dtype(), {shape[0], shape[2]}, std::move(output));
}

void CrossModalSequenceSummaryWeights::validate(const std::size_t dimension) const {
    const auto matrix = 3 * dimension * dimension;
    if (!dimension || input_weight.size() != matrix || recurrent_weight.size() != matrix ||
        input_bias.size() != 3 * dimension || recurrent_bias.size() != 3 * dimension)
        throw std::invalid_argument("cross-modal sequence GRU weight shape mismatch");
}

Tensor cross_modal_sequence_summary(
    const Tensor& tokens, const BooleanMask& mask,
    const CrossModalSequenceSummaryWeights& weights) {
    require_storage(tokens); const auto shape = tokens.shape();
    if (shape.size() != 3 || mask.shape().size() != 2 ||
        mask.shape()[0] != shape[0] || mask.shape()[1] != shape[1])
        throw std::invalid_argument("sequence summary expects [B,T,D] tokens and [B,T] mask");
    const auto batch = static_cast<std::size_t>(shape[0]);
    const auto count_ = static_cast<std::size_t>(shape[1]);
    const auto dim = static_cast<std::size_t>(shape[2]); weights.validate(dim);
    std::vector<double> output(batch * dim);
    const auto sigmoid = [](const double value) { return 1.0 / (1.0 + std::exp(-value)); };
    for (std::size_t b = 0; b < batch; ++b) {
        std::size_t active = 0; for (std::size_t t = 0; t < count_; ++t) active += mask.at(b, t);
        if (!active) throw std::invalid_argument("sequence summary requires at least one active token");
        std::vector<double> hidden(dim), next(dim), input_gates(3 * dim), recurrent_gates(3 * dim);
        for (std::size_t t = 0; t < count_; ++t) {
            const auto input = unit(tokens.values().subspan((b * count_ + t) * dim, dim));
            for (std::size_t gate = 0; gate < 3 * dim; ++gate) {
                input_gates[gate] = weights.input_bias[gate];
                recurrent_gates[gate] = weights.recurrent_bias[gate];
                for (std::size_t d = 0; d < dim; ++d) {
                    input_gates[gate] += input[d] * weights.input_weight[gate * dim + d];
                    recurrent_gates[gate] += hidden[d] * weights.recurrent_weight[gate * dim + d];
                }
            }
            for (std::size_t d = 0; d < dim; ++d) {
                const auto reset = sigmoid(input_gates[d] + recurrent_gates[d]);
                const auto update = sigmoid(input_gates[dim + d] + recurrent_gates[dim + d]);
                const auto candidate = std::tanh(input_gates[2 * dim + d] + reset * recurrent_gates[2 * dim + d]);
                next[d] = candidate + update * (hidden[d] - candidate);
            }
            hidden.swap(next);
            if (t + 1 == active) {
                const auto normalized = unit(hidden);
                std::copy(normalized.begin(), normalized.end(), output.begin() + b * dim);
            }
        }
    }
    return tensor(tokens.dtype(), {shape[0], shape[2]}, std::move(output));
}

Tensor cross_modal_last_summary(const Tensor& tokens, const BooleanMask& mask) {
    require_storage(tokens); const auto shape = tokens.shape();
    if (shape.size() != 3 || mask.shape().size() != 2 || mask.shape()[0] != shape[0] || mask.shape()[1] != shape[1])
        throw std::invalid_argument("last summary expects [B,T,D] tokens and [B,T] mask");
    const auto batch = static_cast<std::size_t>(shape[0]), count_ = static_cast<std::size_t>(shape[1]);
    const auto dim = static_cast<std::size_t>(shape[2]); std::vector<double> output(batch * dim);
    for (std::size_t b = 0; b < batch; ++b) {
        std::size_t active = 0; for (std::size_t t = 0; t < count_; ++t) active += mask.at(b, t);
        if (!active) throw std::invalid_argument("last summary requires at least one active token");
        const auto normalized = unit(tokens.values().subspan((b * count_ + active - 1) * dim, dim));
        std::copy(normalized.begin(), normalized.end(), output.begin() + b * dim);
    }
    return tensor(tokens.dtype(), {shape[0], shape[2]}, std::move(output));
}

Tensor spatial_temporal_moment(const Tensor& feature_map, const std::size_t batch,
                               const std::size_t frames, const char axis) {
    require_storage(feature_map); const auto shape = feature_map.shape();
    if (shape.size() != 4 || shape[0] != batch * frames)
        throw std::invalid_argument("feature map must be [batch*frames,D,H,W]");
    if (axis != 'x' && axis != 'y') throw std::invalid_argument("spatial-temporal moment axis must be x or y");
    const auto dim = static_cast<std::size_t>(shape[1]), height = static_cast<std::size_t>(shape[2]);
    const auto width = static_cast<std::size_t>(shape[3]), frame_size = dim * height * width;
    std::vector<double> output(batch);
    for (std::size_t b = 0; b < batch; ++b) {
        double numerator = 0.0, denominator = 0.0;
        for (std::size_t f = 0; f < frames; ++f) {
            const auto time = frames == 1 ? -1.0 : -1.0 + 2.0 * f / (frames - 1);
            for (std::size_t y = 0; y < height; ++y) for (std::size_t x = 0; x < width; ++x) {
                double activity = 0.0;
                for (std::size_t d = 0; d < dim; ++d) {
                    double mean = 0.0; for (std::size_t other = 0; other < frames; ++other)
                        mean += feature_map.values()[(b * frames + other) * frame_size + (d * height + y) * width + x];
                    mean /= frames; const auto value = feature_map.values()[(b * frames + f) * frame_size + (d * height + y) * width + x] - mean;
                    activity += value * value;
                }
                activity /= dim;
                const auto spatial = axis == 'x' ? (width == 1 ? -1.0 : -1.0 + 2.0 * x / (width - 1))
                    : (height == 1 ? -1.0 : -1.0 + 2.0 * y / (height - 1));
                numerator += activity * time * spatial; denominator += activity;
            }
        }
        output[b] = numerator / std::max(denominator, 1.0e-6);
    }
    return tensor(feature_map.dtype(), {batch}, std::move(output));
}

Tensor spatial_event_features(const Tensor& frame_grid, const Tensor& attention) {
    require_storage(frame_grid); require_storage(attention);
    const auto fs = frame_grid.shape(), as = attention.shape();
    if (fs.size() != 4 || as.size() != 5)
        throw std::invalid_argument("spatial event inputs must be [B,S,F,D]/[B,S,F,H,W]");
    if (fs[0] != as[0] || fs[1] != as[1] || fs[2] != as[2] || fs[3] < 3)
        throw std::invalid_argument("spatial event inputs must share batch/slots/frames and have D>=3");
    const auto batch = static_cast<std::size_t>(fs[0]), slots = static_cast<std::size_t>(fs[1]);
    const auto frames = static_cast<std::size_t>(fs[2]), dim = static_cast<std::size_t>(fs[3]);
    const auto height = static_cast<std::size_t>(as[3]), width = static_cast<std::size_t>(as[4]);
    std::vector<double> output(batch * slots * dim);
    for (std::size_t b = 0; b < batch; ++b) for (std::size_t slot = 0; slot < slots; ++slot) {
        std::vector<double> means(dim);
        for (std::size_t frame = 0; frame < frames; ++frame) for (std::size_t d = 0; d < dim; ++d)
            means[d] += frame_grid.values()[((b * slots + slot) * frames + frame) * dim + d] / frames;
        double weighted_time = 0.0, activity_sum = 0.0;
        for (std::size_t frame = 0; frame < frames; ++frame) {
            double activity = 0.0; for (std::size_t d = 0; d < dim; ++d) {
                const auto delta = frame_grid.values()[((b * slots + slot) * frames + frame) * dim + d] - means[d];
                activity += delta * delta;
            }
            activity /= dim; const auto time = frames == 1 ? -1.0 : -1.0 + 2.0 * frame / (frames - 1);
            weighted_time += activity * time; activity_sum += activity;
        }
        const auto event_time = weighted_time / std::max(activity_sum, 1.0e-6);
        double position = 0.0;
        for (std::size_t y = 0; y < height; ++y) for (std::size_t x = 0; x < width; ++x) {
            const auto coordinate = width == 1 ? -1.0 : -1.0 + 2.0 * x / (width - 1);
            position += attention.values()[(((b * slots + slot) * frames) * height + y) * width + x] * coordinate;
        }
        const auto base = (b * slots + slot) * dim;
        output[base] = position; output[base + 1] = event_time; output[base + 2] = position * event_time;
    }
    return tensor(frame_grid.dtype(), {fs[0], fs[1], fs[3]}, std::move(output));
}

Tensor object_attention_trajectory(const Tensor& attention, const Tensor* match_logits) {
    require_storage(attention); const auto shape = attention.shape();
    if (shape.size() != 5 || !shape[0] || !shape[1] || !shape[2] || !shape[3] || !shape[4])
        throw std::invalid_argument("object attention must be positive [B,S,F,H,W]");
    const auto batch = static_cast<std::size_t>(shape[0]), slots = static_cast<std::size_t>(shape[1]);
    const auto frames = static_cast<std::size_t>(shape[2]), height = static_cast<std::size_t>(shape[3]);
    const auto width = static_cast<std::size_t>(shape[4]), pixels = height * width;
    std::vector<double> positions(batch * slots * frames * 2), peak(batch * slots * frames), entropy(peak.size());
    for (std::size_t b = 0; b < batch; ++b) for (std::size_t s = 0; s < slots; ++s)
        for (std::size_t f = 0; f < frames; ++f) {
            const auto base = ((b * slots + s) * frames + f) * pixels;
            double denominator = 0.0; for (std::size_t i = 0; i < pixels; ++i) denominator += attention.values()[base + i];
            denominator = std::max(denominator, 1.0e-6); double px = 0.0, py = 0.0, maximum = 0.0, disorder = 0.0;
            for (std::size_t y = 0; y < height; ++y) for (std::size_t x = 0; x < width; ++x) {
                const auto probability = attention.values()[base + y * width + x] / denominator;
                maximum = std::max(maximum, probability);
                disorder -= probability * std::log(std::max(probability, 1.0e-6));
                px += probability * (width == 1 ? -1.0 : -1.0 + 2.0 * x / (width - 1));
                py += probability * (height == 1 ? -1.0 : -1.0 + 2.0 * y / (height - 1));
            }
            const auto row = (b * slots + s) * frames + f;
            positions[row * 2] = px; positions[row * 2 + 1] = py; peak[row] = maximum;
            entropy[row] = disorder / std::log(static_cast<double>(std::max<std::size_t>(2, pixels)));
        }
    if (match_logits) {
        const auto gated = confidence_gated_sequence(
            tensor(attention.dtype(), {shape[0], shape[1], shape[2], 2}, std::move(positions)), *match_logits);
        positions.assign(gated.values().begin(), gated.values().end());
    }
    std::vector<double> output(batch * slots * 12);
    for (std::size_t b = 0; b < batch; ++b) for (std::size_t s = 0; s < slots; ++s) {
        const auto sequence = (b * slots + s) * frames; const auto out = (b * slots + s) * 12;
        for (std::size_t d = 0; d < 2; ++d) {
            const auto first = positions[sequence * 2 + d], last = positions[(sequence + frames - 1) * 2 + d];
            output[out + d] = first; output[out + 2 + d] = last; output[out + 4 + d] = last - first;
            for (std::size_t f = 0; f < frames; ++f) {
                const auto time = frames == 1 ? -1.0 : -1.0 + 2.0 * f / (frames - 1);
                output[out + 6 + d] += positions[(sequence + f) * 2 + d] * time / frames;
            }
        }
        for (std::size_t f = 0; f < frames; ++f) {
            const auto time = frames == 1 ? -1.0 : -1.0 + 2.0 * f / (frames - 1);
            output[out + 8] += peak[sequence + f] / frames;
            output[out + 9] += peak[sequence + f] * time / frames;
            output[out + 10] += entropy[sequence + f] / frames;
            output[out + 11] += entropy[sequence + f] * time / frames;
        }
    }
    return tensor(attention.dtype(), {shape[0], shape[1], 12}, std::move(output));
}

Tensor confidence_gated_sequence(const Tensor& values, const Tensor& match_logits) {
    require_storage(values); require_storage(match_logits); const auto shape = values.shape();
    if (shape.size() < 3 || match_logits.shape().size() + 1 != shape.size() ||
        !std::equal(match_logits.shape().begin(), match_logits.shape().end(), shape.begin()))
        throw std::invalid_argument("values/match logits shape mismatch");
    const auto frames = static_cast<std::size_t>(shape[shape.size() - 2]);
    const auto features = static_cast<std::size_t>(shape.back());
    const auto sequences = values.values().size() / (frames * features);
    std::vector<double> output(values.values().size());
    for (std::size_t sequence = 0; sequence < sequences; ++sequence) {
        std::vector<double> logits(frames); for (std::size_t f = 0; f < frames; ++f)
            logits[f] = match_logits.values()[sequence * frames + f];
        auto confidence = softmax(logits); for (auto& value : confidence) value = std::min(value * frames, 1.0);
        std::copy_n(values.values().begin() + sequence * frames * features, features,
            output.begin() + sequence * frames * features);
        for (std::size_t f = 1; f < frames; ++f) for (std::size_t d = 0; d < features; ++d) {
            const auto current = values.values()[(sequence * frames + f) * features + d];
            const auto previous = output[(sequence * frames + f - 1) * features + d];
            output[(sequence * frames + f) * features + d] = confidence[f] * current + (1.0 - confidence[f]) * previous;
        }
    }
    return tensor(values.dtype(), {shape.begin(), shape.end()}, std::move(output));
}

Tensor temporal_relative_visibility(const Tensor& visibility_logits) {
    require_storage(visibility_logits); const auto shape = visibility_logits.shape();
    if (shape.size() != 3 || shape[1] != 2)
        throw std::invalid_argument("temporal visibility logits must be [B,2,F]");
    const auto batch = static_cast<std::size_t>(shape[0]), frames = static_cast<std::size_t>(shape[2]);
    std::vector<double> output(visibility_logits.values().size());
    for (std::size_t b = 0; b < batch; ++b) for (std::size_t role = 0; role < 2; ++role) {
        const auto base = (b * 2 + role) * frames;
        const auto [low, high] = std::minmax_element(visibility_logits.values().begin() + base,
            visibility_logits.values().begin() + base + frames);
        const auto mean = std::accumulate(visibility_logits.values().begin() + base,
            visibility_logits.values().begin() + base + frames, 0.0) / frames;
        for (std::size_t f = 0; f < frames; ++f) {
            const auto value = visibility_logits.values()[base + f];
            output[base + f] = *high > *low ? static_cast<double>(value - mean >= 0)
                : 1.0 / (1.0 + std::exp(-value));
        }
    }
    return tensor(visibility_logits.dtype(), {shape.begin(), shape.end()}, std::move(output));
}

Tensor contrast_memory_summary(const Tensor& cosine_peak, const Tensor& cosine_margin) {
    require_storage(cosine_peak); require_storage(cosine_margin); const auto shape = cosine_peak.shape();
    if (shape.size() != 3 || shape[1] != 2 ||
        cosine_margin.shape().size() != 3 || !std::equal(shape.begin(), shape.end(), cosine_margin.shape().begin()))
        throw std::invalid_argument("contrast confidence must be matched [B,2,F] tensors");
    const auto batch = static_cast<std::size_t>(shape[0]), frames = static_cast<std::size_t>(shape[2]);
    std::vector<double> output(batch * 8);
    for (std::size_t b = 0; b < batch; ++b) for (std::size_t role = 0; role < 2; ++role) {
        const auto base = (b * 2 + role) * frames;
        const auto peak_begin = cosine_peak.values().begin() + base;
        const auto margin_begin = cosine_margin.values().begin() + base;
        const auto [peak_low, peak_high] = std::minmax_element(peak_begin, peak_begin + frames);
        const auto [margin_low, margin_high] = std::minmax_element(margin_begin, margin_begin + frames);
        output[b * 8 + role * 4] = std::accumulate(peak_begin, peak_begin + frames, 0.0) / frames;
        output[b * 8 + role * 4 + 1] = 0.5 * (*peak_high - *peak_low);
        output[b * 8 + role * 4 + 2] = 0.5 * std::accumulate(margin_begin, margin_begin + frames, 0.0) / frames;
        output[b * 8 + role * 4 + 3] = 0.5 * (*margin_high - *margin_low);
    }
    return tensor(cosine_peak.dtype(), {shape[0], 8}, std::move(output));
}

Tensor normalized_evidence_preference(const Tensor& weights, const double margin) {
    require_storage(weights); const auto shape = weights.shape();
    if (shape.size() != 2 || shape[1] != 2) throw std::invalid_argument("evidence weights must be [B,2]");
    if (margin < 0 || margin >= 1) throw std::invalid_argument("evidence preference margin must be in [0, 1)");
    std::vector<double> output(shape[0]);
    for (std::size_t batch = 0; batch < shape[0]; ++batch)
        output[batch] = 2.0 * std::max(0.0, weights.values()[batch * 2 + 1] -
            weights.values()[batch * 2] - margin) / (1.0 - margin);
    return tensor(weights.dtype(), {shape[0]}, std::move(output));
}

DescriptorObjectMemoryOutput descriptor_object_memory(
    const Tensor& attention, const Tensor& visibility_logits,
    const bool use_temporal_relative_visibility) {
    require_storage(attention); require_storage(visibility_logits); const auto shape = attention.shape();
    if (shape.size() != 5 || shape[1] != 2 || visibility_logits.shape().size() != 3 ||
        visibility_logits.shape()[0] != shape[0] || visibility_logits.shape()[1] != 2 ||
        visibility_logits.shape()[2] != shape[2])
        throw std::invalid_argument("object memory requires [B,2,F,H,W] and [B,2,F]");
    const auto batch = static_cast<std::size_t>(shape[0]), frames = static_cast<std::size_t>(shape[2]);
    const auto height = static_cast<std::size_t>(shape[3]), width = static_cast<std::size_t>(shape[4]);
    const auto pixels = height * width;
    std::vector<double> positions(batch * 2 * frames * 2);
    for (std::size_t b = 0; b < batch; ++b) for (std::size_t role = 0; role < 2; ++role)
        for (std::size_t f = 0; f < frames; ++f) {
            const auto base = ((b * 2 + role) * frames + f) * pixels; double denominator = 0.0;
            for (std::size_t i = 0; i < pixels; ++i) denominator += attention.values()[base + i];
            denominator = std::max(denominator, 1.0e-6);
            for (std::size_t y = 0; y < height; ++y) for (std::size_t x = 0; x < width; ++x) {
                const auto probability = attention.values()[base + y * width + x] / denominator;
                positions[((b * 2 + role) * frames + f) * 2] += probability *
                    (width == 1 ? -1.0 : -1.0 + 2.0 * x / (width - 1));
                positions[((b * 2 + role) * frames + f) * 2 + 1] += probability *
                    (height == 1 ? -1.0 : -1.0 + 2.0 * y / (height - 1));
            }
        }
    Tensor visibility = use_temporal_relative_visibility ? temporal_relative_visibility(visibility_logits)
        : visibility_logits.clone();
    std::vector<double> visible(visibility.values().size());
    if (use_temporal_relative_visibility) std::copy(visibility.values().begin(), visibility.values().end(), visible.begin());
    else std::ranges::transform(visibility.values(), visible.begin(), [](const double value) { return 1.0 / (1.0 + std::exp(-value)); });
    std::vector<double> features(batch * 19), margins(batch);
    for (std::size_t b = 0; b < batch; ++b) {
        double anchors[2][2]{}, reads[2][2]{}, anchor_weight[2]{}, occluded[2]{}, returned[2]{};
        for (std::size_t f = 0; f < frames; ++f) for (std::size_t role = 0; role < 2; ++role) {
            const auto v = visible[(b * 2 + role) * frames + f];
            const auto write = (1.0 - anchor_weight[role]) * v * (1.0 - occluded[role]);
            for (std::size_t d = 0; d < 2; ++d)
                anchors[role][d] += write * positions[((b * 2 + role) * frames + f) * 2 + d];
            anchor_weight[role] += write;
            const auto return_gate = occluded[role] * v * (1.0 - returned[role]);
            for (std::size_t d = 0; d < 2; ++d)
                reads[role][d] += return_gate * positions[((b * 2 + role) * frames + f) * 2 + d];
            returned[role] += return_gate;
            const auto occlusion_gate = anchor_weight[role] * (1.0 - v) * (1.0 - returned[role]);
            occluded[role] += (1.0 - occluded[role]) * occlusion_gate;
        }
        for (std::size_t role = 0; role < 2; ++role) for (std::size_t d = 0; d < 2; ++d) {
            anchors[role][d] /= std::max(anchor_weight[role], 1.0e-6);
            reads[role][d] /= std::max(returned[role], 1.0e-6);
        }
        double same_cost = 0.0, swap_cost = 0.0;
        for (std::size_t role = 0; role < 2; ++role) {
            for (std::size_t d = 0; d < 2; ++d) {
                const auto delta = reads[role][d] - anchors[role][d];
                const auto base = b * 19 + role * 8;
                features[base + d] = anchors[role][d]; features[base + 2 + d] = reads[role][d];
                features[base + 4 + d] = delta; same_cost += delta * delta;
                const auto swap_delta = reads[role][d] - anchors[1 - role][d]; swap_cost += swap_delta * swap_delta;
            }
            features[b * 19 + role * 8 + 6] = occluded[role];
            features[b * 19 + role * 8 + 7] = returned[role];
        }
        const auto margin = same_cost - swap_cost; margins[b] = margin;
        features[b * 19 + 16] = same_cost; features[b * 19 + 17] = swap_cost; features[b * 19 + 18] = margin;
    }
    return {tensor(attention.dtype(), {shape[0], 19}, std::move(features)),
            tensor(attention.dtype(), {shape[0]}, std::move(margins))};
}

Tensor sort_object_slots_by_temporal_activity(const Tensor& object_slots, const Tensor& frame_grid) {
    require_storage(object_slots); require_storage(frame_grid);
    const auto os = object_slots.shape(), fs = frame_grid.shape();
    if (os.size() != 3 || fs.size() != 4 || os[0] != fs[0] || os[1] != fs[1] || os[2] != fs[3])
        throw std::invalid_argument("object slots/frame grid must be [B,S,D]/[B,S,F,D]");
    const auto batch = static_cast<std::size_t>(os[0]), slots = static_cast<std::size_t>(os[1]);
    const auto dim = static_cast<std::size_t>(os[2]), frames = static_cast<std::size_t>(fs[2]);
    std::vector<double> output(object_slots.values().size());
    for (std::size_t b = 0; b < batch; ++b) {
        struct Key { std::size_t slot; double activity; double fingerprint[3]; };
        std::vector<Key> keys(slots);
        for (std::size_t slot = 0; slot < slots; ++slot) {
            keys[slot].slot = slot; std::vector<double> means(dim);
            for (std::size_t f = 0; f < frames; ++f) for (std::size_t d = 0; d < dim; ++d)
                means[d] += frame_grid.values()[((b * slots + slot) * frames + f) * dim + d] / frames;
            std::size_t feature = 1;
            for (std::size_t f = 0; f < frames; ++f) for (std::size_t d = 0; d < dim; ++d, ++feature) {
                const auto value = frame_grid.values()[((b * slots + slot) * frames + f) * dim + d] - means[d];
                keys[slot].activity += value * value / (frames * dim);
                keys[slot].fingerprint[0] += value * ((feature % 97) + 1);
                keys[slot].fingerprint[1] += value * (((feature * 17) % 193) + 1);
                keys[slot].fingerprint[2] += value * (((feature * feature) % 389) + 1);
            }
            for (std::size_t d = 0; d < dim; ++d, ++feature) {
                const auto value = object_slots.values()[(b * slots + slot) * dim + d];
                keys[slot].fingerprint[0] += value * ((feature % 97) + 1);
                keys[slot].fingerprint[1] += value * (((feature * 17) % 193) + 1);
                keys[slot].fingerprint[2] += value * (((feature * feature) % 389) + 1);
            }
        }
        std::stable_sort(keys.begin(), keys.end(), [](const Key& left, const Key& right) {
            if (left.activity != right.activity) return left.activity > right.activity;
            for (std::size_t index = 3; index-- > 0;) if (left.fingerprint[index] != right.fingerprint[index])
                return left.fingerprint[index] > right.fingerprint[index];
            return false;
        });
        for (std::size_t destination = 0; destination < slots; ++destination)
            std::copy_n(object_slots.values().begin() + (b * slots + keys[destination].slot) * dim, dim,
                output.begin() + (b * slots + destination) * dim);
    }
    return tensor(object_slots.dtype(), {os.begin(), os.end()}, std::move(output));
}

}  // namespace swegca::world
