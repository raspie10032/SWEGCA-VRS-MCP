#include "world/mosaic_te.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <limits>
#include <numeric>
#include <random>
#include <stdexcept>
#include <sys/resource.h>
#include <unistd.h>
#include <utility>

namespace swegca::world {
namespace {

using Clock = std::chrono::steady_clock;

std::size_t product(std::span<const std::size_t> shape) {
    std::size_t result = 1;
    for (const auto value : shape) {
        if (value && result > std::numeric_limits<std::size_t>::max() / value)
            throw std::overflow_error("MOSAIC-TE tensor size overflow");
        result *= value;
    }
    return result;
}

struct Random final {
    explicit Random(const std::uint64_t seed) : engine(seed) {}
    std::mt19937_64 engine;
    float uniform(const float low, const float high) {
        return std::uniform_real_distribution<float>(low, high)(engine);
    }
    float normal(const float standard_deviation) {
        return std::normal_distribution<float>(0.0F, standard_deviation)(engine);
    }
};

struct Linear final {
    std::size_t input{}, output{};
    std::vector<float> weight, bias;
    Linear() = default;
    Linear(const std::size_t in, const std::size_t out, const bool has_bias, Random& random)
        : input(in), output(out), weight(in * out), bias(has_bias ? out : 0) {
        const auto bound = 1.0F / std::sqrt(static_cast<float>(in));
        for (auto& value : weight) value = random.uniform(-bound, bound);
        for (auto& value : bias) value = random.uniform(-bound, bound);
    }
    std::vector<float> operator()(std::span<const float> values, const std::size_t rows) const {
        if (values.size() != rows * input) throw std::invalid_argument("linear input shape mismatch");
        std::vector<float> result(rows * output);
        for (std::size_t row = 0; row < rows; ++row)
            for (std::size_t out = 0; out < output; ++out) {
                float sum = bias.empty() ? 0.0F : bias[out];
                for (std::size_t in = 0; in < input; ++in)
                    sum += values[row * input + in] * weight[out * input + in];
                result[row * output + out] = sum;
            }
        return result;
    }
    std::size_t count() const noexcept { return weight.size() + bias.size(); }
};

struct LayerNorm final {
    std::size_t width{};
    std::vector<float> weight, bias;
    explicit LayerNorm(const std::size_t size = 0) : width(size), weight(size, 1.0F), bias(size) {}
    std::vector<float> operator()(std::span<const float> input, const std::size_t rows) const {
        if (input.size() != rows * width) throw std::invalid_argument("layer norm input shape mismatch");
        std::vector<float> output(input.size());
        for (std::size_t row = 0; row < rows; ++row) {
            const auto begin = input.begin() + static_cast<std::ptrdiff_t>(row * width);
            const float mean = std::accumulate(begin, begin + static_cast<std::ptrdiff_t>(width), 0.0F)
                / static_cast<float>(width);
            float variance = 0.0F;
            for (std::size_t col = 0; col < width; ++col) {
                const float difference = input[row * width + col] - mean;
                variance += difference * difference;
            }
            variance /= static_cast<float>(width); // torch LayerNorm unbiased=False
            const float inverse = 1.0F / std::sqrt(variance + 1.0e-5F);
            for (std::size_t col = 0; col < width; ++col)
                output[row * width + col] =
                    (input[row * width + col] - mean) * inverse * weight[col] + bias[col];
        }
        return output;
    }
    std::size_t count() const noexcept { return weight.size() + bias.size(); }
};

void add_in_place(std::vector<float>& left, std::span<const float> right) {
    if (left.size() != right.size()) throw std::invalid_argument("residual shape mismatch");
    for (std::size_t index = 0; index < left.size(); ++index) left[index] += right[index];
}

void gelu_in_place(std::vector<float>& values) {
    constexpr float inverse_sqrt_two = 0.7071067811865475244F;
    for (auto& value : values) value *= 0.5F * (1.0F + std::erf(value * inverse_sqrt_two));
}

struct Attention final {
    std::size_t width{}, heads{}, head_width{};
    Linear qkv, output;
    Attention() = default;
    Attention(const std::size_t size, const std::size_t count, Random& random)
        : width(size), heads(count), head_width(size / count), qkv(size, size * 3, true, random),
          output(size, size, true, random) {
        // torch MultiheadAttention overrides in_proj_weight with Xavier uniform and zeroes biases.
        const float bound = std::sqrt(6.0F / static_cast<float>(width + width * 3));
        for (auto& value : qkv.weight) value = random.uniform(-bound, bound);
        std::fill(qkv.bias.begin(), qkv.bias.end(), 0.0F);
        std::fill(output.bias.begin(), output.bias.end(), 0.0F);
    }
    std::vector<float> run(std::span<const float> query, const std::size_t query_rows,
        std::span<const float> key_value, const std::size_t key_rows,
        std::span<const std::uint8_t> key_mask) const {
        if (query.size() != query_rows * width || key_value.size() != key_rows * width ||
            key_mask.size() != key_rows) throw std::invalid_argument("attention shape mismatch");
        // Packed projection has the exact PyTorch row order [Q; K; V].
        const auto projected_query = qkv(query, query_rows);
        const auto projected_key = qkv(key_value, key_rows);
        std::vector<float> joined(query_rows * width);
        const float scale = 1.0F / std::sqrt(static_cast<float>(head_width));
        std::vector<float> logits(key_rows);
        for (std::size_t head = 0; head < heads; ++head)
            for (std::size_t q = 0; q < query_rows; ++q) {
                float maximum = -std::numeric_limits<float>::infinity();
                for (std::size_t k = 0; k < key_rows; ++k) {
                    if (!key_mask[k]) { logits[k] = -std::numeric_limits<float>::infinity(); continue; }
                    float dot = 0.0F;
                    for (std::size_t col = 0; col < head_width; ++col)
                        dot += projected_query[q * width * 3 + head * head_width + col]
                            * projected_key[k * width * 3 + width + head * head_width + col];
                    logits[k] = dot * scale;
                    maximum = std::max(maximum, logits[k]);
                }
                float denominator = 0.0F;
                for (std::size_t k = 0; k < key_rows; ++k) if (key_mask[k]) {
                    logits[k] = std::exp(logits[k] - maximum); denominator += logits[k];
                }
                for (std::size_t col = 0; col < head_width; ++col) {
                    float value = 0.0F;
                    for (std::size_t k = 0; k < key_rows; ++k) if (key_mask[k])
                        value += logits[k] / denominator
                            * projected_key[k * width * 3 + width * 2 + head * head_width + col];
                    joined[q * width + head * head_width + col] = value;
                }
            }
        return output(joined, query_rows);
    }
    std::size_t count() const noexcept { return qkv.count() + output.count(); }
};

struct EncoderLayer final {
    LayerNorm norm1, norm2;
    Attention attention;
    Linear first, second;
    EncoderLayer(const MosaicTEConfig& config, Random& random)
        : norm1(config.model_dim), norm2(config.model_dim),
          attention(config.model_dim, config.attention_heads, random),
          first(config.model_dim, config.ffn_dim, true, random),
          second(config.ffn_dim, config.model_dim, true, random) {}
    std::vector<float> run(std::vector<float> input, const std::size_t rows,
                           std::span<const std::uint8_t> mask) const {
        auto normalized = norm1(input, rows);
        add_in_place(input, attention.run(normalized, rows, normalized, rows, mask));
        normalized = norm2(input, rows);
        auto feed_forward = first(normalized, rows);
        gelu_in_place(feed_forward);
        add_in_place(input, second(feed_forward, rows));
        return input;
    }
    std::size_t count() const noexcept {
        return norm1.count() + norm2.count() + attention.count() + first.count() + second.count();
    }
};

JsonValue::Array shape_json(std::span<const std::size_t> shape) {
    JsonValue::Array result; result.reserve(shape.size());
    for (const auto value : shape) result.emplace_back(JsonInteger{std::to_string(value)});
    return result;
}
double rounded(const double value, const double scale) { return std::round(value * scale) / scale; }

JsonValue::Object process_memory() {
    std::ifstream statm("/proc/self/statm");
    std::uint64_t ignored = 0, pages = 0;
    const auto page_size = ::sysconf(_SC_PAGESIZE);
    rusage usage{};
    if (page_size <= 0 || !(statm >> ignored >> pages) || ::getrusage(RUSAGE_SELF, &usage))
        throw std::runtime_error("cannot read process memory");
    constexpr double mib = 1024.0 * 1024.0;
    return {{"rss_mib", rounded(static_cast<double>(pages) * page_size / mib, 1000.0)},
            {"peak_rss_mib", rounded(static_cast<double>(usage.ru_maxrss) * 1024.0 / mib, 1000.0)}};
}

} // namespace

void MosaicTEConfig::validate() const {
    if (!patch_size || !max_bytes || !model_dim || !conditioning_dim || !attention_heads ||
        !ffn_dim || !local_layers || !slot_count || !recurrent_rounds)
        throw std::invalid_argument("MOSAIC-TE dimensions and counts must be positive");
    if (max_bytes % patch_size) throw std::invalid_argument("max_bytes must be divisible by patch_size");
    if (model_dim % attention_heads) throw std::invalid_argument("model_dim must be divisible by attention_heads");
}

MosaicTETensor::MosaicTETensor(std::vector<std::size_t> dimensions, std::vector<float> data)
    : shape(std::move(dimensions)), values(std::move(data)) {
    if (product(shape) != values.size()) throw std::invalid_argument("MOSAIC-TE tensor shape mismatch");
}
MosaicTEMask::MosaicTEMask(std::vector<std::size_t> dimensions, std::vector<std::uint8_t> data)
    : shape(std::move(dimensions)), values(std::move(data)) {
    if (product(shape) != values.size()) throw std::invalid_argument("MOSAIC-TE mask shape mismatch");
    for (const auto value : values) if (value > 1) throw std::invalid_argument("mask values must be boolean");
}
MosaicTEByteIds::MosaicTEByteIds(const std::size_t rows, const std::size_t columns,
    std::vector<std::int64_t> data) : batch(rows), bytes(columns), values(std::move(data)) {
    if ((batch && bytes > std::numeric_limits<std::size_t>::max() / batch) || values.size() != batch * bytes)
        throw std::invalid_argument("byte_ids shape mismatch");
}

struct SharedSlotCell::Impl final {
    Random holder;
    MosaicTEConfig config;
    LayerNorm cross_norm, output_norm;
    Attention cross_attention;
    Linear first, second;
    Impl(const MosaicTEConfig& source, const std::uint64_t seed) : holder(seed), config(source),
        cross_norm(source.model_dim), output_norm(source.model_dim),
        cross_attention(source.model_dim, source.attention_heads, holder),
        first(source.model_dim, source.ffn_dim, true, holder),
        second(source.ffn_dim, source.model_dim, true, holder) {}
    std::size_t count() const noexcept { return cross_norm.count() + output_norm.count() +
        cross_attention.count() + first.count() + second.count(); }
};

SharedSlotCell::SharedSlotCell(const MosaicTEConfig& config, const std::uint64_t seed)
    : impl_([&config, seed] {
        config.validate();
        return std::make_unique<Impl>(config, seed);
    }()) {}
SharedSlotCell::~SharedSlotCell() = default;
SharedSlotCell::SharedSlotCell(SharedSlotCell&&) noexcept = default;
SharedSlotCell& SharedSlotCell::operator=(SharedSlotCell&&) noexcept = default;

MosaicTETensor SharedSlotCell::forward(const MosaicTETensor& slots,
    const MosaicTETensor& sequence, const MosaicTEMask& sequence_mask) const {
    const auto& c = impl_->config;
    if (slots.shape.size() != 3 || sequence.shape.size() != 3 || sequence_mask.shape.size() != 2 ||
        slots.shape[0] != sequence.shape[0] || sequence_mask.shape[0] != sequence.shape[0] ||
        slots.shape[2] != c.model_dim || sequence.shape[2] != c.model_dim ||
        sequence_mask.shape[1] != sequence.shape[1]) throw std::invalid_argument("SharedSlotCell shape mismatch");
    std::vector<float> result(slots.values.size());
    const auto slot_rows = slots.shape[1], sequence_rows = sequence.shape[1];
    for (std::size_t batch = 0; batch < slots.shape[0]; ++batch) {
        const auto slot_begin = slots.values.begin() + static_cast<std::ptrdiff_t>(batch * slot_rows * c.model_dim);
        const auto sequence_begin = sequence.values.begin() + static_cast<std::ptrdiff_t>(batch * sequence_rows * c.model_dim);
        const auto mask_begin = sequence_mask.values.begin() + static_cast<std::ptrdiff_t>(batch * sequence_rows);
        const std::span<const float> slot_span(&*slot_begin, slot_rows * c.model_dim);
        const std::span<const float> sequence_span(&*sequence_begin, sequence_rows * c.model_dim);
        const std::span<const std::uint8_t> mask_span(&*mask_begin, sequence_rows);
        auto output = std::vector<float>(slot_span.begin(), slot_span.end());
        const auto normalized = impl_->cross_norm(slot_span, slot_rows);
        add_in_place(output, impl_->cross_attention.run(normalized, slot_rows, sequence_span, sequence_rows, mask_span));
        auto feed_forward = impl_->first(impl_->output_norm(output, slot_rows), slot_rows);
        gelu_in_place(feed_forward);
        add_in_place(output, impl_->second(feed_forward, slot_rows));
        std::copy(output.begin(), output.end(), result.begin() + static_cast<std::ptrdiff_t>(batch * slot_rows * c.model_dim));
    }
    return {slots.shape, std::move(result)};
}
std::size_t SharedSlotCell::parameter_count() const noexcept { return impl_->count(); }

struct MosaicTextEncoderProbe::Impl final {
    MosaicTEConfig config;
    std::vector<float> byte_embedding, position_embedding, learned_slots;
    std::vector<EncoderLayer> local_encoder;
    SharedSlotCell slot_cell;
    LayerNorm sequence_norm, slot_norm;
    Linear sequence_projection, slot_projection;

    Impl(MosaicTEConfig source, const std::uint64_t seed)
        : config(std::move(source)), slot_cell(config, seed + 1),
          sequence_norm(config.model_dim), slot_norm(config.model_dim) {
        config.validate();
        Random random(seed);
        byte_embedding.resize(257 * config.model_dim);
        // nn.Embedding::reset_parameters uses normal_(0, 1).
        for (auto& value : byte_embedding) value = random.normal(1.0F);
        std::fill(byte_embedding.begin(), byte_embedding.begin() + static_cast<std::ptrdiff_t>(config.model_dim), 0.0F);

        position_embedding.resize(config.max_bytes / config.patch_size * config.model_dim);
        learned_slots.resize(config.slot_count * config.model_dim);
        for (auto& value : position_embedding) value = random.normal(0.02F);
        for (auto& value : learned_slots) value = random.normal(0.02F);

        // TransformerEncoder deep-copies one initialized prototype layer.
        const EncoderLayer prototype(config, random);
        local_encoder.assign(config.local_layers, prototype);
        sequence_projection = Linear(config.model_dim, config.conditioning_dim, false, random);
        slot_projection = Linear(config.model_dim, config.conditioning_dim, false, random);
    }

    std::size_t count() const noexcept {
        std::size_t result = byte_embedding.size() + position_embedding.size() + learned_slots.size()
            + slot_cell.parameter_count() + sequence_norm.count() + slot_norm.count()
            + sequence_projection.count() + slot_projection.count();
        for (const auto& layer : local_encoder) result += layer.count();
        return result;
    }
};

MosaicTextEncoderProbe::MosaicTextEncoderProbe(MosaicTEConfig config, const std::uint64_t seed)
    : impl_([&config, seed] {
        config.validate();
        return std::make_unique<Impl>(std::move(config), seed);
    }()) {}
MosaicTextEncoderProbe::~MosaicTextEncoderProbe() = default;
MosaicTextEncoderProbe::MosaicTextEncoderProbe(MosaicTextEncoderProbe&&) noexcept = default;
MosaicTextEncoderProbe& MosaicTextEncoderProbe::operator=(MosaicTextEncoderProbe&&) noexcept = default;
const MosaicTEConfig& MosaicTextEncoderProbe::config() const noexcept { return impl_->config; }
std::size_t MosaicTextEncoderProbe::parameter_count() const noexcept { return impl_->count(); }

MosaicTEOutput MosaicTextEncoderProbe::forward(const MosaicTEByteIds& byte_ids,
    const std::span<const std::int64_t> byte_lengths,
    const std::optional<std::size_t> requested_rounds) const {
    const auto& c = impl_->config;
    if (byte_ids.batch != byte_lengths.size()) throw std::invalid_argument("byte_ids and byte_lengths batch mismatch");
    if (!byte_ids.bytes || byte_ids.bytes % c.patch_size)
        throw std::invalid_argument("byte dimension must be divisible by patch_size");
    const auto patches = byte_ids.bytes / c.patch_size;
    if (patches > c.max_bytes / c.patch_size) throw std::invalid_argument("byte dimension exceeds max_bytes");
    for (const auto id : byte_ids.values) if (id < 0 || id > 256)
        throw std::invalid_argument("byte_ids must be in [0, 256]");
    for (const auto length : byte_lengths) if (length < 0 || static_cast<std::size_t>(length) > byte_ids.bytes)
        throw std::invalid_argument("byte_lengths must be in [0, byte_count]");
    const auto rounds = requested_rounds.value_or(c.recurrent_rounds);
    if (!rounds || rounds > c.recurrent_rounds)
        throw std::invalid_argument("rounds must be in [1, recurrent_rounds]");

    std::vector<std::uint8_t> byte_mask(byte_ids.batch * byte_ids.bytes);
    MosaicTEMask patch_mask({byte_ids.batch, patches}, std::vector<std::uint8_t>(byte_ids.batch * patches));
    std::vector<float> sequence(byte_ids.batch * patches * c.model_dim);
    for (std::size_t batch = 0; batch < byte_ids.batch; ++batch) {
        for (std::size_t index = 0; index < byte_ids.bytes; ++index)
            byte_mask[batch * byte_ids.bytes + index] = index < static_cast<std::size_t>(byte_lengths[batch]);
        for (std::size_t patch = 0; patch < patches; ++patch) {
            std::size_t count = 0;
            for (std::size_t offset = 0; offset < c.patch_size; ++offset)
                count += byte_mask[batch * byte_ids.bytes + patch * c.patch_size + offset];
            patch_mask.values[batch * patches + patch] = count != 0;
            if (!patch) patch_mask.values[batch * patches] = 1; // prevent an all-masked encoder row
            const auto denominator = static_cast<float>(std::max<std::size_t>(count, 1));
            for (std::size_t column = 0; column < c.model_dim; ++column) {
                float sum = 0.0F;
                for (std::size_t offset = 0; offset < c.patch_size; ++offset) {
                    const auto location = batch * byte_ids.bytes + patch * c.patch_size + offset;
                    if (byte_mask[location]) {
                        const auto id = static_cast<std::size_t>(byte_ids.values[location]);
                        sum += impl_->byte_embedding[id * c.model_dim + column];
                    }
                }
                sequence[(batch * patches + patch) * c.model_dim + column] =
                    sum / denominator + impl_->position_embedding[patch * c.model_dim + column];
            }
        }
    }

    for (const auto& layer : impl_->local_encoder) {
        for (std::size_t batch = 0; batch < byte_ids.batch; ++batch) {
            const auto begin = sequence.begin() + static_cast<std::ptrdiff_t>(batch * patches * c.model_dim);
            std::vector<float> row(begin, begin + static_cast<std::ptrdiff_t>(patches * c.model_dim));
            const auto mask_begin = patch_mask.values.begin() + static_cast<std::ptrdiff_t>(batch * patches);
            row = layer.run(std::move(row), patches,
                std::span<const std::uint8_t>(&*mask_begin, patches));
            std::copy(row.begin(), row.end(), begin);
        }
    }

    std::vector<float> slots(byte_ids.batch * c.slot_count * c.model_dim);
    for (std::size_t batch = 0; batch < byte_ids.batch; ++batch) {
        std::vector<float> pooled(c.model_dim);
        std::size_t active = 0;
        for (std::size_t patch = 0; patch < patches; ++patch) if (patch_mask.values[batch * patches + patch]) {
            ++active;
            for (std::size_t column = 0; column < c.model_dim; ++column)
                pooled[column] += sequence[(batch * patches + patch) * c.model_dim + column];
        }
        for (auto& value : pooled) value /= static_cast<float>(active);
        for (std::size_t slot = 0; slot < c.slot_count; ++slot)
            for (std::size_t column = 0; column < c.model_dim; ++column)
                slots[(batch * c.slot_count + slot) * c.model_dim + column] =
                    impl_->learned_slots[slot * c.model_dim + column] + pooled[column];
    }
    MosaicTETensor slot_tensor({byte_ids.batch, c.slot_count, c.model_dim}, std::move(slots));
    const MosaicTETensor sequence_tensor({byte_ids.batch, patches, c.model_dim}, sequence);
    for (std::size_t round = 0; round < rounds; ++round)
        slot_tensor = impl_->slot_cell.forward(slot_tensor, sequence_tensor, patch_mask);

    auto normalized_sequence = impl_->sequence_norm(sequence, byte_ids.batch * patches);
    auto normalized_slots = impl_->slot_norm(slot_tensor.values, byte_ids.batch * c.slot_count);
    auto projected_sequence = impl_->sequence_projection(normalized_sequence, byte_ids.batch * patches);
    auto projected_slots = impl_->slot_projection(normalized_slots, byte_ids.batch * c.slot_count);
    std::vector<float> pooled(byte_ids.batch * c.conditioning_dim);
    for (std::size_t batch = 0; batch < byte_ids.batch; ++batch)
        for (std::size_t column = 0; column < c.conditioning_dim; ++column) {
            float sum = 0.0F;
            for (std::size_t slot = 0; slot < c.slot_count; ++slot)
                sum += projected_slots[(batch * c.slot_count + slot) * c.conditioning_dim + column];
            pooled[batch * c.conditioning_dim + column] = sum / static_cast<float>(c.slot_count);
        }

    std::vector<std::int64_t> spans(byte_ids.batch * patches * 2, -1);
    for (std::size_t batch = 0; batch < byte_ids.batch; ++batch)
        for (std::size_t patch = 0; patch < patches; ++patch) if (patch_mask.values[batch * patches + patch]) {
            const auto start = patch * c.patch_size;
            const auto end = std::min(start + c.patch_size, static_cast<std::size_t>(byte_lengths[batch]));
            spans[(batch * patches + patch) * 2] = static_cast<std::int64_t>(start);
            spans[(batch * patches + patch) * 2 + 1] = static_cast<std::int64_t>(end);
        }
    return {MosaicTETensor({byte_ids.batch, patches, c.conditioning_dim},
                std::move(projected_sequence)),
            MosaicTETensor({byte_ids.batch, c.slot_count, c.conditioning_dim},
                std::move(projected_slots)),
            MosaicTETensor({byte_ids.batch, c.conditioning_dim}, std::move(pooled)),
            std::move(patch_mask), std::move(spans)};
}

MosaicTEByteIds encode_mosaic_te_texts(const std::vector<std::string>& texts,
    const std::size_t max_bytes, const std::size_t patch_size) {
    if (texts.empty()) throw std::invalid_argument("prompts must be non-empty");
    if (!max_bytes || !patch_size) throw std::invalid_argument("max_bytes and patch_size must be positive");
    std::size_t longest = 0;
    for (const auto& text : texts) longest = std::max(longest, std::min(text.size(), max_bytes));
    const auto padded = std::min(max_bytes,
        ((std::max<std::size_t>(longest, 1) + patch_size - 1) / patch_size) * patch_size);
    std::vector<std::int64_t> values(texts.size() * padded);
    for (std::size_t row = 0; row < texts.size(); ++row)
        for (std::size_t index = 0; index < std::min(texts[row].size(), max_bytes); ++index)
            values[row * padded + index] = static_cast<unsigned char>(texts[row][index]) + 1;
    return {texts.size(), padded, std::move(values)};
}

MosaicTEOutput MosaicTextEncoderProbe::encode(const std::vector<std::string>& prompts,
    const std::optional<std::size_t> rounds) const {
    auto ids = encode_mosaic_te_texts(prompts, impl_->config.max_bytes, impl_->config.patch_size);
    std::vector<std::int64_t> lengths; lengths.reserve(prompts.size());
    for (const auto& text : prompts)
        lengths.push_back(static_cast<std::int64_t>(std::min(text.size(), ids.bytes)));
    return forward(ids, lengths, rounds);
}

JsonValue::Object run_mosaic_te_probe(const MosaicTEConfig& config,
    const std::string_view device, const std::size_t repeats) {
    if (!repeats) throw std::invalid_argument("repeats must be positive");
    if (device != "cpu")
        throw std::invalid_argument("dependency-free MOSAIC-TE probe supports only the cpu device");
    config.validate();
    const std::vector<std::string> prompts{
        "붉은 우산을 든 로봇이 왼쪽의 파란 고양이를 바라본다.",
        "A glass sphere above two small copper cubes.",
        "간판에 정확히 MOSAIC-TE라고 적힌 밤거리", ""};
    MosaicTextEncoderProbe model(config, 47);
    MosaicTEOutput output;
    for (std::size_t index = 0; index < 3; ++index) output = model.encode(prompts);
    std::vector<double> latencies; latencies.reserve(repeats);
    for (std::size_t index = 0; index < repeats; ++index) {
        const auto started = Clock::now();
        output = model.encode(prompts);
        latencies.push_back(std::chrono::duration<double, std::milli>(Clock::now() - started).count());
    }

    bool ordered_spans = true;
    const auto patches = output.attention_mask.shape[1];
    for (std::size_t row = 0; row < prompts.size(); ++row) {
        std::int64_t previous_end = -1;
        for (std::size_t patch = 0; patch < patches; ++patch)
            if (output.attention_mask.values[row * patches + patch]) {
                const auto start = output.byte_spans[(row * patches + patch) * 2];
                const auto end = output.byte_spans[(row * patches + patch) * 2 + 1];
                if (previous_end >= 0 && start < previous_end) ordered_spans = false;
                previous_end = end;
            }
    }
    const bool dimensions = output.sequence_states.shape.back() == config.conditioning_dim &&
        output.global_slots.shape.back() == config.conditioning_dim;
    const bool slots = output.global_slots.shape[1] == config.slot_count;
    const bool finite = std::all_of(output.sequence_states.values.begin(), output.sequence_states.values.end(),
            [](const float value) { return std::isfinite(value); }) &&
        std::all_of(output.global_slots.values.begin(), output.global_slots.values.end(),
            [](const float value) { return std::isfinite(value); });
    std::sort(latencies.begin(), latencies.end());
    const auto middle = latencies.size() / 2;
    const double median = latencies.size() % 2 ? latencies[middle]
        : (latencies[middle - 1] + latencies[middle]) / 2.0;
    const auto p95_index = std::min(latencies.size() - 1,
        static_cast<std::size_t>(std::ceil(static_cast<double>(latencies.size()) * 0.95)) - 1);
    const auto integer = [](const std::size_t value) -> JsonValue {
        return JsonInteger{std::to_string(value)};
    };
    const JsonValue::Object config_report{
        {"patch_size", integer(config.patch_size)}, {"max_bytes", integer(config.max_bytes)},
        {"model_dim", integer(config.model_dim)}, {"conditioning_dim", integer(config.conditioning_dim)},
        {"attention_heads", integer(config.attention_heads)}, {"ffn_dim", integer(config.ffn_dim)},
        {"local_layers", integer(config.local_layers)}, {"slot_count", integer(config.slot_count)},
        {"recurrent_rounds", integer(config.recurrent_rounds)}};
    const JsonValue::Object shapes{
        {"sequence_states", shape_json(output.sequence_states.shape)},
        {"global_slots", shape_json(output.global_slots.shape)},
        {"pooled_state", shape_json(output.pooled_state.shape)},
        {"attention_mask", shape_json(output.attention_mask.shape)},
        {"byte_spans", shape_json(std::vector<std::size_t>{prompts.size(), patches, 2})}};
    const JsonValue::Object checks{
        {"sequence_and_slot_dimensions_match", dimensions},
        {"fixed_global_slot_count", slots}, {"byte_spans_are_ordered", ordered_spans},
        {"outputs_are_finite", finite}};
    return {{"status", "interface_probe_not_quality_validation"}, {"config", config_report},
        {"device", std::string(device)}, {"prompt_count", integer(prompts.size())},
        {"parameter_count", integer(model.parameter_count())},
        {"parameter_storage_fp16_mib",
            rounded(static_cast<double>(model.parameter_count()) * 2.0 / (1024.0 * 1024.0), 1000.0)},
        {"shapes", shapes},
        {"latency_ms", JsonValue::Object{{"batch_median", rounded(median, 10000.0)},
            {"batch_p95", rounded(latencies[p95_index], 10000.0)}, {"repeats", integer(repeats)}}},
        {"process_memory_mib", process_memory()}, {"checks", checks},
        {"interface_probe_passed", dimensions && slots && ordered_spans && finite}};
}

} // namespace swegca::world
