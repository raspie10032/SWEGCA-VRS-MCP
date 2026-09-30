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

}  // namespace swegca::world
