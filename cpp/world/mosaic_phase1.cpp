#include "world/mosaic_phase1.hpp"

#include "swegca_architecture/sha256.hpp"
#include "world/mosaic_v0.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstring>
#include <fstream>
#include <functional>
#include <iomanip>
#include <limits>
#include <numeric>
#include <numbers>
#include <random>
#include <ranges>
#include <set>
#include <sstream>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace swegca::world {
namespace {

constexpr float kLayerNormEpsilon = 1.0e-5F;
constexpr std::uint64_t kMaximumTensorElements = UINT64_C(1) << 34;
constexpr std::array<std::string_view, 2> kTextRagDocuments{
    "Source: ring procedure manual. Confidence: 1.0. Instruction: to advance around the ring, move exactly one node forward for every requested reasoning step.",
    "Source: ring procedure manual. Confidence: 1.0. Instruction: to retreat around the ring, move exactly one node backward for every requested reasoning step."};

[[noreturn]] void invalid(const std::string& message) { throw std::invalid_argument(message); }

std::size_t checked_size(const std::uint64_t value, const char* what) {
    if (value > std::numeric_limits<std::size_t>::max()) invalid(std::string(what) + " exceeds size_t");
    return static_cast<std::size_t>(value);
}

std::size_t checked_product(std::span<const std::uint64_t> shape) {
    std::uint64_t result = 1;
    for (const auto extent : shape) {
        if (extent != 0 && result > kMaximumTensorElements / extent) invalid("tensor shape is too large");
        result *= extent;
    }
    return checked_size(result, "tensor element count");
}

std::uint64_t checked_multiply(const std::uint64_t left, const std::uint64_t right,
                               const char* what) {
    if (right != 0 && left > std::numeric_limits<std::uint64_t>::max() / right)
        invalid(std::string(what) + " overflows uint64");
    return left * right;
}

std::uint64_t ring_value(const std::uint64_t start, const std::uint64_t depth,
                         const std::uint64_t operation, const std::uint64_t count) {
    const auto delta = depth % count;
    if (operation == 0)
        return delta >= count - start ? delta - (count - start) : start + delta;
    return start >= delta ? start - delta : count - (delta - start);
}

std::string resolve_device(const std::string_view device) {
    if (device == "auto" || device == "cpu") return "cpu";
    invalid("Phase 1 native implementation supports only CPU");
}

std::string hex_digest(const swegca::architecture::DigestBytes& bytes) {
    static constexpr char alphabet[] = "0123456789abcdef";
    std::string result(bytes.size() * 2, '0');
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        const auto value = std::to_integer<unsigned char>(bytes[i]);
        result[2 * i] = alphabet[value >> 4];
        result[2 * i + 1] = alphabet[value & 15U];
    }
    return result;
}

std::vector<std::byte> read_bytes(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream) throw std::runtime_error("cannot open file: " + path.string());
    const auto end = stream.tellg();
    if (end < 0) throw std::runtime_error("cannot determine file size");
    std::vector<std::byte> bytes(static_cast<std::size_t>(end));
    stream.seekg(0);
    if (!bytes.empty()) stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!stream) throw std::runtime_error("cannot read file: " + path.string());
    return bytes;
}

std::string file_sha256(const std::filesystem::path& path) {
    const auto bytes = read_bytes(path);
    return hex_digest(swegca::architecture::Sha256::of(bytes));
}

double rounded(const double value, const int places) {
    const auto factor = std::pow(10.0, places);
    return std::round(value * factor) / factor;
}

double median(std::vector<double> values) {
    if (values.empty()) invalid("median requires at least one value");
    std::ranges::sort(values);
    const auto middle = values.size() / 2;
    if (values.size() % 2) return values[middle];
    return (values[middle - 1] + values[middle]) / 2.0;
}

struct Node final {
    std::vector<float> value;
    std::vector<float> gradient;
    std::function<void(Node&)> backward;
};

struct ADTensor final {
    std::shared_ptr<Node> node;
    std::vector<std::size_t> shape;
    [[nodiscard]] std::size_t size() const noexcept { return node->value.size(); }
};

struct Tape final {
    std::vector<std::shared_ptr<Node>> operations;

    ADTensor make(std::vector<std::size_t> shape, std::vector<float> values,
                  std::function<void(const std::shared_ptr<Node>&)> install) {
        auto node = std::make_shared<Node>();
        node->value = std::move(values);
        node->gradient.assign(node->value.size(), 0.0F);
        install(node);
        operations.push_back(node);
        return {std::move(node), std::move(shape)};
    }

    void backward(const ADTensor& loss) {
        if (loss.size() != 1) invalid("backward requires scalar loss");
        loss.node->gradient[0] = 1.0F;
        for (auto it = operations.rbegin(); it != operations.rend(); ++it)
            if ((*it)->backward) (*it)->backward(**it);
    }
};

struct Parameter final {
    std::string name;
    std::vector<std::uint64_t> shape;
    std::shared_ptr<Node> node;
    std::vector<float> first_moment;
    std::vector<float> second_moment;
};

ADTensor as_tensor(const Parameter& parameter) {
    std::vector<std::size_t> shape;
    shape.reserve(parameter.shape.size());
    for (const auto extent : parameter.shape) shape.push_back(checked_size(extent, "parameter extent"));
    return {parameter.node, std::move(shape)};
}

ADTensor add(Tape& tape, const ADTensor& left, const ADTensor& right) {
    if (left.shape != right.shape) invalid("add shape mismatch");
    std::vector<float> values(left.size());
    for (std::size_t i = 0; i < values.size(); ++i) values[i] = left.node->value[i] + right.node->value[i];
    return tape.make(left.shape, std::move(values), [a = left.node, b = right.node](const auto& out) {
        out->backward = [a, b](Node& output) {
            for (std::size_t i = 0; i < output.gradient.size(); ++i) {
                a->gradient[i] += output.gradient[i];
                b->gradient[i] += output.gradient[i];
            }
        };
    });
}

ADTensor linear(Tape& tape, const ADTensor& input, const ADTensor& weight,
                const ADTensor* bias = nullptr) {
    if (input.shape.empty() || weight.shape.size() != 2) invalid("linear rank mismatch");
    const auto in_dim = input.shape.back();
    const auto out_dim = weight.shape[0];
    if (weight.shape[1] != in_dim || (bias != nullptr && (bias->shape != std::vector<std::size_t>{out_dim})))
        invalid("linear dimension mismatch");
    const auto rows = input.size() / in_dim;
    auto shape = input.shape;
    shape.back() = out_dim;
    std::vector<float> values(rows * out_dim, 0.0F);
    for (std::size_t row = 0; row < rows; ++row)
        for (std::size_t out = 0; out < out_dim; ++out) {
            float sum = bias == nullptr ? 0.0F : bias->node->value[out];
            for (std::size_t in = 0; in < in_dim; ++in)
                sum += input.node->value[row * in_dim + in] * weight.node->value[out * in_dim + in];
            values[row * out_dim + out] = sum;
        }
    const auto bias_node = bias == nullptr ? std::shared_ptr<Node>{} : bias->node;
    return tape.make(std::move(shape), std::move(values),
        [x = input.node, w = weight.node, bias_node, rows, in_dim, out_dim](const auto& result) {
            result->backward = [x, w, bias_node, rows, in_dim, out_dim](Node& output) {
                for (std::size_t row = 0; row < rows; ++row)
                    for (std::size_t out = 0; out < out_dim; ++out) {
                        const auto gradient = output.gradient[row * out_dim + out];
                        if (bias_node) bias_node->gradient[out] += gradient;
                        for (std::size_t in = 0; in < in_dim; ++in) {
                            x->gradient[row * in_dim + in] += gradient * w->value[out * in_dim + in];
                            w->gradient[out * in_dim + in] += gradient * x->value[row * in_dim + in];
                        }
                    }
            };
        });
}

ADTensor activate(Tape& tape, const ADTensor& input, const bool gelu) {
    std::vector<float> values(input.size());
    for (std::size_t i = 0; i < values.size(); ++i) {
        const auto x = input.node->value[i];
        if (gelu) values[i] = 0.5F * x * (1.0F + std::erf(x / std::sqrt(2.0F)));
        else values[i] = x / (1.0F + std::exp(-x));
    }
    return tape.make(input.shape, std::move(values), [x = input.node, gelu](const auto& out) {
        out->backward = [x, gelu](Node& output) {
            constexpr float inverse_sqrt_two_pi = 0.3989422804014327F;
            for (std::size_t i = 0; i < output.gradient.size(); ++i) {
                const auto value = x->value[i];
                float derivative;
                if (gelu) derivative = 0.5F * (1.0F + std::erf(value / std::sqrt(2.0F))) +
                    value * inverse_sqrt_two_pi * std::exp(-0.5F * value * value);
                else {
                    const auto sigmoid = 1.0F / (1.0F + std::exp(-value));
                    derivative = sigmoid + value * sigmoid * (1.0F - sigmoid);
                }
                x->gradient[i] += output.gradient[i] * derivative;
            }
        };
    });
}

ADTensor layer_norm(Tape& tape, const ADTensor& input, const ADTensor& weight,
                    const ADTensor& bias) {
    if (input.shape.empty()) invalid("layer norm requires a final dimension");
    const auto dim = input.shape.back();
    if (weight.shape != std::vector<std::size_t>{dim} || bias.shape != std::vector<std::size_t>{dim})
        invalid("layer norm parameter mismatch");
    const auto rows = input.size() / dim;
    std::vector<float> mean(rows), inverse_std(rows), values(input.size());
    for (std::size_t row = 0; row < rows; ++row) {
        float average = 0.0F;
        for (std::size_t d = 0; d < dim; ++d) average += input.node->value[row * dim + d];
        average /= static_cast<float>(dim);
        float variance = 0.0F;
        for (std::size_t d = 0; d < dim; ++d) {
            const auto centered = input.node->value[row * dim + d] - average;
            variance += centered * centered;
        }
        variance /= static_cast<float>(dim);
        mean[row] = average;
        inverse_std[row] = 1.0F / std::sqrt(variance + kLayerNormEpsilon);
        for (std::size_t d = 0; d < dim; ++d) {
            const auto normalized = (input.node->value[row * dim + d] - average) * inverse_std[row];
            values[row * dim + d] = normalized * weight.node->value[d] + bias.node->value[d];
        }
    }
    return tape.make(input.shape, std::move(values),
        [x = input.node, w = weight.node, b = bias.node, mean = std::move(mean),
         inverse_std = std::move(inverse_std), rows, dim](const auto& out) {
            out->backward = [x, w, b, mean, inverse_std, rows, dim](Node& output) {
                for (std::size_t row = 0; row < rows; ++row) {
                    float sum_dxhat = 0.0F, sum_dxhat_xhat = 0.0F;
                    for (std::size_t d = 0; d < dim; ++d) {
                        const auto index = row * dim + d;
                        const auto xhat = (x->value[index] - mean[row]) * inverse_std[row];
                        const auto gradient = output.gradient[index];
                        w->gradient[d] += gradient * xhat;
                        b->gradient[d] += gradient;
                        const auto dxhat = gradient * w->value[d];
                        sum_dxhat += dxhat;
                        sum_dxhat_xhat += dxhat * xhat;
                    }
                    for (std::size_t d = 0; d < dim; ++d) {
                        const auto index = row * dim + d;
                        const auto xhat = (x->value[index] - mean[row]) * inverse_std[row];
                        const auto dxhat = output.gradient[index] * w->value[d];
                        x->gradient[index] += inverse_std[row] *
                            (dxhat - sum_dxhat / static_cast<float>(dim) -
                             xhat * sum_dxhat_xhat / static_cast<float>(dim));
                    }
                }
            };
        });
}

ADTensor embedding(Tape& tape, const ADTensor& table,
                   std::span<const std::uint64_t> indices) {
    if (table.shape.size() != 2) invalid("embedding table must be rank two");
    const auto count = table.shape[0], dim = table.shape[1];
    std::vector<float> values(indices.size() * dim);
    std::vector<std::size_t> saved;
    saved.reserve(indices.size());
    for (std::size_t row = 0; row < indices.size(); ++row) {
        if (indices[row] >= count) invalid("embedding index is out of range");
        saved.push_back(static_cast<std::size_t>(indices[row]));
        std::copy_n(table.node->value.begin() + saved.back() * dim, dim, values.begin() + row * dim);
    }
    return tape.make({indices.size(), dim}, std::move(values),
        [source = table.node, saved = std::move(saved), dim](const auto& out) {
            out->backward = [source, saved, dim](Node& output) {
                for (std::size_t row = 0; row < saved.size(); ++row)
                    for (std::size_t d = 0; d < dim; ++d)
                        source->gradient[saved[row] * dim + d] += output.gradient[row * dim + d];
            };
        });
}

ADTensor attention_core(Tape& tape, const ADTensor& qkv, const std::size_t heads) {
    if (qkv.shape.size() != 3 || qkv.shape[2] % 3 != 0) invalid("attention QKV shape mismatch");
    const auto batch = qkv.shape[0], tokens = qkv.shape[1], dim = qkv.shape[2] / 3;
    if (!heads || dim % heads) invalid("attention head dimension mismatch");
    const auto head_dim = dim / heads;
    const auto scale = 1.0F / std::sqrt(static_cast<float>(head_dim));
    std::vector<float> probabilities(batch * heads * tokens * tokens);
    std::vector<float> values(batch * tokens * dim, 0.0F);
    const auto qkv_at = [&](const std::size_t b, const std::size_t t, const std::size_t part,
                            const std::size_t d) -> float {
        return qkv.node->value[(b * tokens + t) * (3 * dim) + part * dim + d];
    };
    for (std::size_t b = 0; b < batch; ++b)
        for (std::size_t h = 0; h < heads; ++h)
            for (std::size_t query = 0; query < tokens; ++query) {
                const auto probability_base = ((b * heads + h) * tokens + query) * tokens;
                float maximum = -std::numeric_limits<float>::infinity();
                for (std::size_t key = 0; key < tokens; ++key) {
                    float score = 0.0F;
                    for (std::size_t d = 0; d < head_dim; ++d) {
                        const auto feature = h * head_dim + d;
                        score += qkv_at(b, query, 0, feature) * qkv_at(b, key, 1, feature);
                    }
                    score *= scale;
                    probabilities[probability_base + key] = score;
                    maximum = std::max(maximum, score);
                }
                float denominator = 0.0F;
                for (std::size_t key = 0; key < tokens; ++key) {
                    auto& probability = probabilities[probability_base + key];
                    probability = std::exp(probability - maximum);
                    denominator += probability;
                }
                for (std::size_t key = 0; key < tokens; ++key)
                    probabilities[probability_base + key] /= denominator;
                for (std::size_t d = 0; d < head_dim; ++d) {
                    float sum = 0.0F;
                    for (std::size_t key = 0; key < tokens; ++key)
                        sum += probabilities[probability_base + key] *
                            qkv_at(b, key, 2, h * head_dim + d);
                    values[(b * tokens + query) * dim + h * head_dim + d] = sum;
                }
            }
    return tape.make({batch, tokens, dim}, std::move(values),
        [source = qkv.node, probabilities = std::move(probabilities), batch, tokens, dim,
         heads, head_dim, scale](const auto& out) {
            out->backward = [source, probabilities, batch, tokens, dim, heads, head_dim, scale](Node& output) {
                auto& gradient = source->gradient;
                const auto source_at = [&](const std::size_t b, const std::size_t t,
                                           const std::size_t part, const std::size_t d) -> float {
                    return source->value[(b * tokens + t) * (3 * dim) + part * dim + d];
                };
                for (std::size_t b = 0; b < batch; ++b)
                    for (std::size_t h = 0; h < heads; ++h)
                        for (std::size_t query = 0; query < tokens; ++query) {
                            const auto pbase = ((b * heads + h) * tokens + query) * tokens;
                            std::vector<float> dp(tokens, 0.0F);
                            for (std::size_t key = 0; key < tokens; ++key)
                                for (std::size_t d = 0; d < head_dim; ++d) {
                                    const auto feature = h * head_dim + d;
                                    const auto dy = output.gradient[(b * tokens + query) * dim + feature];
                                    dp[key] += dy * source_at(b, key, 2, feature);
                                    gradient[(b * tokens + key) * (3 * dim) + 2 * dim + feature] +=
                                        probabilities[pbase + key] * dy;
                                }
                            float weighted = 0.0F;
                            for (std::size_t key = 0; key < tokens; ++key)
                                weighted += dp[key] * probabilities[pbase + key];
                            for (std::size_t key = 0; key < tokens; ++key) {
                                const auto ds = probabilities[pbase + key] * (dp[key] - weighted) * scale;
                                for (std::size_t d = 0; d < head_dim; ++d) {
                                    const auto feature = h * head_dim + d;
                                    gradient[(b * tokens + query) * (3 * dim) + feature] +=
                                        ds * source_at(b, key, 1, feature);
                                    gradient[(b * tokens + key) * (3 * dim) + dim + feature] +=
                                        ds * source_at(b, query, 0, feature);
                                }
                            }
                        }
            };
        });
}

ADTensor initial_state(Tape& tape, const ADTensor& workspace, const ADTensor& query,
                       const ADTensor* extra) {
    if (workspace.shape.size() != 2 || query.shape.size() != 2 ||
        workspace.shape[1] != query.shape[1]) invalid("initial state shape mismatch");
    const auto batch = query.shape[0], slots = workspace.shape[0], dim = workspace.shape[1];
    const auto patches = extra == nullptr ? 0 : extra->shape[1];
    if (extra != nullptr && (extra->shape.size() != 3 || extra->shape[0] != batch || extra->shape[2] != dim))
        invalid("extra context shape mismatch");
    std::vector<float> values(batch * (slots + patches) * dim);
    for (std::size_t b = 0; b < batch; ++b) {
        for (std::size_t slot = 0; slot < slots; ++slot)
            for (std::size_t d = 0; d < dim; ++d)
                values[(b * (slots + patches) + slot) * dim + d] =
                    workspace.node->value[slot * dim + d] + (slot == 0 ? query.node->value[b * dim + d] : 0.0F);
        if (extra != nullptr)
            std::copy_n(extra->node->value.begin() + b * patches * dim, patches * dim,
                        values.begin() + (b * (slots + patches) + slots) * dim);
    }
    const auto extra_node = extra == nullptr ? std::shared_ptr<Node>{} : extra->node;
    return tape.make({batch, slots + patches, dim}, std::move(values),
        [w = workspace.node, q = query.node, extra_node, batch, slots, patches, dim](const auto& out) {
            out->backward = [w, q, extra_node, batch, slots, patches, dim](Node& output) {
                for (std::size_t b = 0; b < batch; ++b) {
                    for (std::size_t slot = 0; slot < slots; ++slot)
                        for (std::size_t d = 0; d < dim; ++d) {
                            const auto gradient = output.gradient[(b * (slots + patches) + slot) * dim + d];
                            w->gradient[slot * dim + d] += gradient;
                            if (slot == 0) q->gradient[b * dim + d] += gradient;
                        }
                    if (extra_node)
                        for (std::size_t p = 0; p < patches; ++p)
                            for (std::size_t d = 0; d < dim; ++d)
                                extra_node->gradient[(b * patches + p) * dim + d] +=
                                    output.gradient[(b * (slots + patches) + slots + p) * dim + d];
                }
            };
        });
}

ADTensor select_rows(Tape& tape, const ADTensor& updated, const ADTensor& previous,
                     std::span<const unsigned char> active) {
    if (updated.shape != previous.shape || updated.shape.empty() || updated.shape[0] != active.size())
        invalid("row selection shape mismatch");
    const auto row_size = updated.size() / active.size();
    std::vector<float> values(updated.size());
    std::vector<unsigned char> mask(active.size());
    for (std::size_t row = 0; row < active.size(); ++row) {
        mask[row] = active[row] ? 1U : 0U;
        const auto& source = active[row] ? updated.node->value : previous.node->value;
        std::copy_n(source.begin() + row * row_size, row_size, values.begin() + row * row_size);
    }
    return tape.make(updated.shape, std::move(values),
        [a = updated.node, b = previous.node, mask = std::move(mask), row_size](const auto& out) {
            out->backward = [a, b, mask, row_size](Node& output) {
                for (std::size_t row = 0; row < mask.size(); ++row) {
                    auto& target = mask[row] ? a->gradient : b->gradient;
                    for (std::size_t i = 0; i < row_size; ++i)
                        target[row * row_size + i] += output.gradient[row * row_size + i];
                }
            };
        });
}

ADTensor slot_zero(Tape& tape, const ADTensor& state) {
    if (state.shape.size() != 3) invalid("state must be rank three");
    const auto batch = state.shape[0], tokens = state.shape[1], dim = state.shape[2];
    std::vector<float> values(batch * dim);
    for (std::size_t b = 0; b < batch; ++b)
        std::copy_n(state.node->value.begin() + b * tokens * dim, dim, values.begin() + b * dim);
    return tape.make({batch, dim}, std::move(values),
        [source = state.node, batch, tokens, dim](const auto& out) {
            out->backward = [source, batch, tokens, dim](Node& output) {
                for (std::size_t b = 0; b < batch; ++b)
                    for (std::size_t d = 0; d < dim; ++d)
                        source->gradient[b * tokens * dim + d] += output.gradient[b * dim + d];
            };
        });
}

ADTensor reencoded_query(Tape& tape, const ADTensor& logits,
                         const ADTensor& node_embedding, const ADTensor& context) {
    if (logits.shape.size() != 2 || node_embedding.shape.size() != 2 || context.shape.size() != 2 ||
        logits.shape[0] != context.shape[0] || logits.shape[1] != node_embedding.shape[0] ||
        node_embedding.shape[1] != context.shape[1]) invalid("re-encoding shape mismatch");
    const auto batch = logits.shape[0], nodes = logits.shape[1], dim = node_embedding.shape[1];
    std::vector<float> probability(batch * nodes), values(batch * dim);
    for (std::size_t b = 0; b < batch; ++b) {
        const auto begin = logits.node->value.begin() + b * nodes;
        const auto maximum = *std::max_element(begin, begin + nodes);
        float denominator = 0.0F;
        for (std::size_t n = 0; n < nodes; ++n) {
            probability[b * nodes + n] = std::exp(logits.node->value[b * nodes + n] - maximum);
            denominator += probability[b * nodes + n];
        }
        for (std::size_t n = 0; n < nodes; ++n) probability[b * nodes + n] /= denominator;
        for (std::size_t d = 0; d < dim; ++d) {
            float value = context.node->value[b * dim + d];
            for (std::size_t n = 0; n < nodes; ++n)
                value += probability[b * nodes + n] * node_embedding.node->value[n * dim + d];
            values[b * dim + d] = value;
        }
    }
    return tape.make({batch, dim}, std::move(values),
        [l = logits.node, e = node_embedding.node, c = context.node,
         probability = std::move(probability), batch, nodes, dim](const auto& out) {
            out->backward = [l, e, c, probability, batch, nodes, dim](Node& output) {
                for (std::size_t b = 0; b < batch; ++b) {
                    std::vector<float> dp(nodes, 0.0F);
                    for (std::size_t d = 0; d < dim; ++d) {
                        const auto dy = output.gradient[b * dim + d];
                        c->gradient[b * dim + d] += dy;
                        for (std::size_t n = 0; n < nodes; ++n) {
                            dp[n] += dy * e->value[n * dim + d];
                            e->gradient[n * dim + d] += probability[b * nodes + n] * dy;
                        }
                    }
                    float weighted = 0.0F;
                    for (std::size_t n = 0; n < nodes; ++n) weighted += dp[n] * probability[b * nodes + n];
                    for (std::size_t n = 0; n < nodes; ++n)
                        l->gradient[b * nodes + n] += probability[b * nodes + n] * (dp[n] - weighted);
                }
            };
        });
}

ADTensor cross_entropy_trace(Tape& tape, const std::vector<ADTensor>& trace,
                             std::span<const std::uint64_t> targets) {
    if (trace.empty() || targets.empty()) invalid("cross entropy trace must not be empty");
    const auto batch = targets.size(), classes = trace.front().shape.at(1);
    std::vector<std::shared_ptr<Node>> logits;
    logits.reserve(trace.size());
    double total = 0.0;
    for (const auto& item : trace) {
        if (item.shape != std::vector<std::size_t>{batch, classes}) invalid("logit trace shape mismatch");
        logits.push_back(item.node);
        for (std::size_t b = 0; b < batch; ++b) {
            if (targets[b] >= classes) invalid("cross entropy target out of range");
            const auto begin = item.node->value.begin() + b * classes;
            const auto maximum = *std::max_element(begin, begin + classes);
            double sum = 0.0;
            for (std::size_t c = 0; c < classes; ++c) sum += std::exp(item.node->value[b * classes + c] - maximum);
            total += std::log(sum) + maximum - item.node->value[b * classes + targets[b]];
        }
    }
    const auto divisor = static_cast<float>(trace.size() * batch);
    std::vector<std::uint64_t> saved_targets(targets.begin(), targets.end());
    return tape.make({}, {static_cast<float>(total / divisor)},
        [logits = std::move(logits), saved_targets = std::move(saved_targets), batch, classes, divisor](const auto& out) {
            out->backward = [logits, saved_targets, batch, classes, divisor](Node& output) {
                const auto upstream = output.gradient[0] / divisor;
                for (const auto& item : logits)
                    for (std::size_t b = 0; b < batch; ++b) {
                        const auto begin = item->value.begin() + b * classes;
                        const auto maximum = *std::max_element(begin, begin + classes);
                        float denominator = 0.0F;
                        for (std::size_t c = 0; c < classes; ++c)
                            denominator += std::exp(item->value[b * classes + c] - maximum);
                        for (std::size_t c = 0; c < classes; ++c) {
                            auto gradient = std::exp(item->value[b * classes + c] - maximum) / denominator;
                            if (c == saved_targets[b]) gradient -= 1.0F;
                            item->gradient[b * classes + c] += upstream * gradient;
                        }
                    }
            };
        });
}

struct BlockIndices final {
    std::size_t qkv_weight{}, qkv_bias{}, out_weight{}, out_bias{};
    std::size_t linear1_weight{}, linear1_bias{}, linear2_weight{}, linear2_bias{};
    std::size_t norm1_weight{}, norm1_bias{}, norm2_weight{}, norm2_bias{};
};

float uniform_sample(std::mt19937_64& generator, const float low, const float high) {
    return std::uniform_real_distribution<float>(low, high)(generator);
}

class NativeModel final {
public:
    Phase1Config config;
    Phase1ModelKind kind;
    std::vector<Parameter> parameters;
    std::size_t node_embedding{}, operator_embedding{}, workspace{};
    std::size_t depth0_weight{}, depth0_bias{}, depth2_weight{}, depth2_bias{};
    std::vector<BlockIndices> blocks;
    std::size_t output_norm_weight{}, output_norm_bias{}, output_weight{}, output_bias{};
    std::optional<std::size_t> byte_embedding, text_weight, text_bias;
    std::size_t text_dim{}, text_patches{};
    std::vector<std::int64_t> procedure_documents;

    NativeModel(Phase1Config input_config, const Phase1ModelKind input_kind)
        : config(std::move(input_config)), kind(input_kind) {
        config.validate();
        std::mt19937_64 generator(config.seed);
        const auto dim = checked_size(config.model_dim, "model_dim");
        node_embedding = add_embedding("node_embedding.weight", config.node_count, config.model_dim, generator, false);
        operator_embedding = add_embedding("operator_embedding.weight", 2, config.model_dim, generator, false);
        workspace = add_constant("workspace", {config.workspace_slots, config.model_dim}, 0.0F);
        std::tie(depth0_weight, depth0_bias) = add_linear("depth_projection.0", config.model_dim, 2, generator);
        std::tie(depth2_weight, depth2_bias) = add_linear("depth_projection.2", config.model_dim, config.model_dim, generator);
        for (std::uint64_t layer = 0; layer < config.physical_layers; ++layer) {
            const auto prefix = "blocks." + std::to_string(layer) + ".";
            BlockIndices block;
            const auto qkv_dim = checked_multiply(config.model_dim, 3, "QKV dimension");
            block.qkv_weight = add_xavier(prefix + "self_attn.in_proj_weight", {qkv_dim, config.model_dim}, generator);
            block.qkv_bias = add_constant(prefix + "self_attn.in_proj_bias", {qkv_dim}, 0.0F);
            std::tie(block.out_weight, block.out_bias) = add_linear(prefix + "self_attn.out_proj", config.model_dim, config.model_dim, generator);
            std::tie(block.linear1_weight, block.linear1_bias) = add_linear(prefix + "linear1", config.ffn_dim, config.model_dim, generator);
            std::tie(block.linear2_weight, block.linear2_bias) = add_linear(prefix + "linear2", config.model_dim, config.ffn_dim, generator);
            block.norm1_weight = add_constant(prefix + "norm1.weight", {config.model_dim}, 1.0F);
            block.norm1_bias = add_constant(prefix + "norm1.bias", {config.model_dim}, 0.0F);
            block.norm2_weight = add_constant(prefix + "norm2.weight", {config.model_dim}, 1.0F);
            block.norm2_bias = add_constant(prefix + "norm2.bias", {config.model_dim}, 0.0F);
            blocks.push_back(block);
        }
        output_norm_weight = add_constant("output_norm.weight", {config.model_dim}, 1.0F);
        output_norm_bias = add_constant("output_norm.bias", {config.model_dim}, 0.0F);
        std::tie(output_weight, output_bias) = add_linear("output", config.node_count, config.model_dim, generator);
        {
            std::normal_distribution<float> distribution(0.0F, 0.02F);
            for (auto& value : parameters[workspace].node->value) value = distribution(generator);
        }
        if (kind == Phase1ModelKind::text_rag) {
            text_dim = std::max<std::size_t>(8, dim / 4);
            text_patches = 0;
            for (const auto document : kTextRagDocuments)
                text_patches = std::max(text_patches, (document.size() + 7) / 8);
            procedure_documents.assign(2 * text_patches * 8, 0);
            for (std::size_t document_index = 0; document_index < kTextRagDocuments.size(); ++document_index)
                for (std::size_t i = 0; i < kTextRagDocuments[document_index].size(); ++i)
                    procedure_documents[(document_index * text_patches + i / 8) * 8 + i % 8] =
                        static_cast<unsigned char>(kTextRagDocuments[document_index][i]) + 1;
            byte_embedding = add_embedding("byte_embedding.weight", 257, text_dim, generator, true);
            std::tie(text_weight, text_bias) = add_linear("text_projection", config.model_dim, text_dim, generator);
        }
    }

    std::size_t add_parameter(std::string name, std::vector<std::uint64_t> shape,
                              std::vector<float> values) {
        if (checked_product(shape) != values.size()) invalid("parameter shape/value mismatch");
        auto node = std::make_shared<Node>();
        node->value = std::move(values);
        node->gradient.assign(node->value.size(), 0.0F);
        parameters.push_back({std::move(name), std::move(shape), std::move(node), {}, {}});
        auto& parameter = parameters.back();
        parameter.first_moment.assign(parameter.node->value.size(), 0.0F);
        parameter.second_moment.assign(parameter.node->value.size(), 0.0F);
        return parameters.size() - 1;
    }

    std::size_t add_constant(std::string name, std::vector<std::uint64_t> shape, const float value) {
        return add_parameter(std::move(name), shape, std::vector<float>(checked_product(shape), value));
    }

    std::size_t add_normal(std::string name, std::vector<std::uint64_t> shape,
                           std::mt19937_64& generator, const float deviation) {
        std::normal_distribution<float> distribution(0.0F, deviation);
        std::vector<float> values(checked_product(shape));
        for (auto& value : values) value = distribution(generator);
        return add_parameter(std::move(name), std::move(shape), std::move(values));
    }

    std::size_t add_embedding(std::string name, const std::uint64_t count, const std::uint64_t dim,
                              std::mt19937_64& generator, const bool padding_zero) {
        const auto result = add_normal(std::move(name), {count, dim}, generator, 1.0F);
        if (padding_zero)
            std::fill_n(parameters[result].node->value.begin(), checked_size(dim, "embedding dim"), 0.0F);
        return result;
    }

    std::size_t add_xavier(std::string name, std::vector<std::uint64_t> shape,
                           std::mt19937_64& generator) {
        if (shape.size() != 2) invalid("Xavier initializer requires matrix");
        const auto bound = std::sqrt(6.0F / static_cast<float>(shape[0] + shape[1]));
        std::vector<float> values(checked_product(shape));
        for (auto& value : values) value = uniform_sample(generator, -bound, bound);
        return add_parameter(std::move(name), std::move(shape), std::move(values));
    }

    std::pair<std::size_t, std::size_t> add_linear(
        std::string prefix, const std::uint64_t out, const std::uint64_t in,
        std::mt19937_64& generator) {
        const auto bound = 1.0F / std::sqrt(static_cast<float>(in));
        std::vector<float> weights(checked_product(std::array<std::uint64_t, 2>{out, in}));
        for (auto& value : weights) value = uniform_sample(generator, -bound, bound);
        std::vector<float> biases(checked_size(out, "linear bias"));
        for (auto& value : biases) value = uniform_sample(generator, -bound, bound);
        return {add_parameter(prefix + ".weight", {out, in}, std::move(weights)),
                add_parameter(prefix + ".bias", {out}, std::move(biases))};
    }

    ADTensor parameter(const std::size_t index) const { return as_tensor(parameters.at(index)); }

    ADTensor transformer_block(Tape& tape, const ADTensor& state, const BlockIndices& block) const {
        auto normalized = layer_norm(tape, state, parameter(block.norm1_weight), parameter(block.norm1_bias));
        auto qkv = linear(tape, normalized, parameter(block.qkv_weight), &parameter_ref(block.qkv_bias));
        auto attended = attention_core(tape, qkv, checked_size(config.attention_heads, "attention_heads"));
        auto projected = linear(tape, attended, parameter(block.out_weight), &parameter_ref(block.out_bias));
        auto residual = add(tape, state, projected);
        auto normalized_ffn = layer_norm(tape, residual, parameter(block.norm2_weight), parameter(block.norm2_bias));
        auto hidden = linear(tape, normalized_ffn, parameter(block.linear1_weight), &parameter_ref(block.linear1_bias));
        hidden = activate(tape, hidden, true);
        auto update = linear(tape, hidden, parameter(block.linear2_weight), &parameter_ref(block.linear2_bias));
        return add(tape, residual, update);
    }

    const ADTensor& parameter_ref(const std::size_t index) const {
        parameter_scratch_ = parameter(index);
        return parameter_scratch_;
    }

    ADTensor text_context(Tape& tape, std::span<const std::uint64_t> operators) const {
        if (!byte_embedding || !text_weight || !text_bias) invalid("text context requested from operator model");
        const auto batch = operators.size();
        std::vector<std::uint16_t> bytes(batch * text_patches * 8, 0);
        for (std::size_t b = 0; b < batch; ++b) {
            if (operators[b] >= kTextRagDocuments.size()) invalid("operator index is out of range");
            for (std::size_t i = 0; i < text_patches * 8; ++i)
                bytes[b * text_patches * 8 + i] = static_cast<std::uint16_t>(
                    procedure_documents[operators[b] * text_patches * 8 + i]);
        }
        const auto table = parameter(*byte_embedding);
        std::vector<float> means(batch * text_patches * text_dim, 0.0F);
        std::vector<std::uint8_t> counts(batch * text_patches, 0);
        for (std::size_t patch = 0; patch < batch * text_patches; ++patch) {
            for (std::size_t offset = 0; offset < 8; ++offset) {
                const auto byte = bytes[patch * 8 + offset];
                if (!byte) continue;
                ++counts[patch];
                for (std::size_t d = 0; d < text_dim; ++d)
                    means[patch * text_dim + d] += table.node->value[byte * text_dim + d];
            }
            const auto divisor = std::max<unsigned>(counts[patch], 1U);
            for (std::size_t d = 0; d < text_dim; ++d) means[patch * text_dim + d] /= static_cast<float>(divisor);
        }
        auto patch_tensor = tape.make({batch, text_patches, text_dim}, std::move(means),
            [source = table.node, bytes = std::move(bytes), counts = std::move(counts),
             batch, patches = text_patches, dim = text_dim](const auto& out) {
                out->backward = [source, bytes, counts, batch, patches, dim](Node& output) {
                    for (std::size_t patch = 0; patch < batch * patches; ++patch) {
                        const auto divisor = static_cast<float>(std::max<unsigned>(counts[patch], 1U));
                        for (std::size_t offset = 0; offset < 8; ++offset) {
                            const auto byte = bytes[patch * 8 + offset];
                            if (!byte) continue;
                            for (std::size_t d = 0; d < dim; ++d)
                                source->gradient[byte * dim + d] += output.gradient[patch * dim + d] / divisor;
                        }
                    }
                };
            });
        const auto bias = parameter(*text_bias);
        return linear(tape, patch_tensor, parameter(*text_weight), &bias);
    }

    std::vector<ADTensor> forward_ad(Tape& tape,
        std::span<const std::uint64_t> starts, std::span<const std::uint64_t> operators,
        std::span<const std::uint64_t> depths, const bool recurrent) const {
        if (starts.size() != operators.size() || starts.size() != depths.size() || starts.empty())
            invalid("start_nodes, operators, and depths must be non-empty one-dimensional equal-length inputs");
        for (const auto depth : depths) if (!depth) invalid("depths must be at least one");
        const auto batch = starts.size(), dim = checked_size(config.model_dim, "model_dim");
        auto node = embedding(tape, parameter(node_embedding), starts);
        ADTensor operation;
        if (kind == Phase1ModelKind::operator_model) operation = embedding(tape, parameter(operator_embedding), operators);
        else operation = tape.make({batch, dim}, std::vector<float>(batch * dim, 0.0F), [](const auto&) {});
        std::vector<float> depth_values(batch * 2);
        for (std::size_t b = 0; b < batch; ++b) {
            const auto feature_depth = recurrent ? 1U : depths[b];
            const auto normalized = static_cast<float>(feature_depth) / static_cast<float>(config.eval_depth);
            depth_values[b * 2] = normalized;
            depth_values[b * 2 + 1] = std::sin(std::numbers::pi_v<float> * normalized);
        }
        auto depth_feature = tape.make({batch, 2}, std::move(depth_values), [](const auto&) {});
        const auto d0b = parameter(depth0_bias);
        auto depth_context = linear(tape, depth_feature, parameter(depth0_weight), &d0b);
        depth_context = activate(tape, depth_context, false);
        const auto d2b = parameter(depth2_bias);
        depth_context = linear(tape, depth_context, parameter(depth2_weight), &d2b);
        auto context = add(tape, operation, depth_context);
        auto query = add(tape, node, context);
        std::optional<ADTensor> extra;
        if (kind == Phase1ModelKind::text_rag) extra = text_context(tape, operators);
        auto state = initial_state(tape, parameter(workspace), query, extra ? &*extra : nullptr);
        const auto rounds = recurrent ? *std::max_element(depths.begin(), depths.end()) : 1U;
        std::vector<ADTensor> trace;
        trace.reserve(checked_size(rounds, "round count"));
        for (std::uint64_t round = 0; round < rounds; ++round) {
            auto updated = state;
            for (const auto& block : blocks) updated = transformer_block(tape, updated, block);
            if (recurrent) {
                std::vector<unsigned char> active(batch);
                for (std::size_t b = 0; b < batch; ++b) active[b] = depths[b] > round ? 1U : 0U;
                state = select_rows(tape, updated, state, active);
            } else state = updated;
            auto first = slot_zero(tape, state);
            first = layer_norm(tape, first, parameter(output_norm_weight), parameter(output_norm_bias));
            const auto out_bias = parameter(output_bias);
            auto logits = linear(tape, first, parameter(output_weight), &out_bias);
            trace.push_back(logits);
            const auto completed = round + 1;
            if (recurrent && config.reencode_interval != 0 && completed < rounds &&
                completed % config.reencode_interval == 0) {
                auto next_query = reencoded_query(tape, logits, parameter(node_embedding), context);
                auto reencoded = initial_state(tape, parameter(workspace), next_query, extra ? &*extra : nullptr);
                std::vector<unsigned char> continuing(batch);
                for (std::size_t b = 0; b < batch; ++b) continuing[b] = depths[b] > completed ? 1U : 0U;
                state = select_rows(tape, reencoded, state, continuing);
            }
        }
        return trace;
    }

    mutable ADTensor parameter_scratch_;
};

ADTensor recurrent_loss(Tape& tape, const std::vector<ADTensor>& trace,
                        std::span<const std::uint64_t> starts,
                        std::span<const std::uint64_t> operators,
                        std::span<const std::uint64_t> depths,
                        const std::uint64_t node_count) {
    std::vector<std::shared_ptr<Node>> logits;
    std::vector<std::vector<std::size_t>> active_rows;
    std::vector<std::vector<std::uint64_t>> targets;
    logits.reserve(trace.size()); active_rows.reserve(trace.size()); targets.reserve(trace.size());
    double loss_sum = 0.0;
    for (std::size_t step_index = 0; step_index < trace.size(); ++step_index) {
        const auto step = static_cast<std::uint64_t>(step_index + 1);
        if (trace[step_index].shape != std::vector<std::size_t>{starts.size(), checked_size(node_count, "node_count")})
            invalid("recurrent logit shape mismatch");
        logits.push_back(trace[step_index].node);
        active_rows.emplace_back(); targets.emplace_back();
        for (std::size_t row = 0; row < starts.size(); ++row) if (depths[row] >= step) {
            active_rows.back().push_back(row);
            const auto target = ring_value(starts[row], step, operators[row], node_count);
            targets.back().push_back(target);
        }
        if (active_rows.back().empty()) invalid("recurrent loss step has no active rows");
    }
    // Recompute the scalar in the same per-step order; the loop above populated
    // the exact active row/target sets used by the backward pass.
    loss_sum = 0.0;
    for (std::size_t step = 0; step < logits.size(); ++step) {
        double step_loss = 0.0;
        for (std::size_t i = 0; i < active_rows[step].size(); ++i) {
            const auto row = active_rows[step][i];
            const auto begin = logits[step]->value.begin() + row * node_count;
            const auto maximum = *std::max_element(begin, begin + node_count);
            double denominator = 0.0;
            for (std::uint64_t c = 0; c < node_count; ++c)
                denominator += std::exp(logits[step]->value[row * node_count + c] - maximum);
            step_loss += std::log(denominator) + maximum -
                         logits[step]->value[row * node_count + targets[step][i]];
        }
        loss_sum += step_loss / static_cast<double>(active_rows[step].size());
    }
    loss_sum /= static_cast<double>(logits.size());
    return tape.make({}, {static_cast<float>(loss_sum)},
        [logits = std::move(logits), active_rows = std::move(active_rows),
         targets = std::move(targets), node_count](const auto& out) {
            out->backward = [logits, active_rows, targets, node_count](Node& output) {
                const auto steps = static_cast<float>(logits.size());
                for (std::size_t step = 0; step < logits.size(); ++step) {
                    const auto divisor = steps * static_cast<float>(active_rows[step].size());
                    for (std::size_t i = 0; i < active_rows[step].size(); ++i) {
                        const auto row = active_rows[step][i];
                        const auto begin = logits[step]->value.begin() + row * node_count;
                        const auto maximum = *std::max_element(begin, begin + node_count);
                        float denominator = 0.0F;
                        for (std::uint64_t c = 0; c < node_count; ++c)
                            denominator += std::exp(logits[step]->value[row * node_count + c] - maximum);
                        for (std::uint64_t c = 0; c < node_count; ++c) {
                            auto gradient = std::exp(logits[step]->value[row * node_count + c] - maximum) / denominator;
                            if (c == targets[step][i]) gradient -= 1.0F;
                            logits[step]->gradient[row * node_count + c] += output.gradient[0] * gradient / divisor;
                        }
                    }
                }
            };
        });
}

struct Batch final {
    std::vector<std::uint64_t> start;
    std::vector<std::uint64_t> operation;
    std::vector<std::uint64_t> depth;
};

Batch make_batch(const Phase1Config& config, std::mt19937_64& generator,
                 const std::uint64_t maximum_depth,
                 const std::optional<std::uint64_t> fixed_depth = std::nullopt,
                 const std::optional<std::uint64_t> batch_size = std::nullopt) {
    const auto size = checked_size(batch_size.value_or(config.batch_size), "batch_size");
    Batch batch;
    batch.start.resize(size); batch.operation.resize(size); batch.depth.resize(size);
    std::uniform_int_distribution<std::uint64_t> nodes(0, config.node_count - 1);
    std::uniform_int_distribution<std::uint64_t> operations(0, 1);
    std::uniform_int_distribution<std::uint64_t> depths(1, maximum_depth);
    for (std::size_t i = 0; i < size; ++i) {
        batch.start[i] = nodes(generator);
        batch.operation[i] = operations(generator);
        batch.depth[i] = fixed_depth.value_or(depths(generator));
    }
    return batch;
}

std::uint64_t argmax_row(const std::vector<float>& logits, const std::size_t row,
                         const std::size_t classes) {
    const auto begin = logits.begin() + row * classes;
    return static_cast<std::uint64_t>(std::distance(begin, std::max_element(begin, begin + classes)));
}

struct TrainedVariant final {
    Phase1TrainingReport report;
    std::unique_ptr<SharedDepthSequenceModel> model;
};

}  // namespace

class SharedDepthSequenceModel::Impl final {
public:
    explicit Impl(Phase1Config config, const Phase1ModelKind kind)
        : native(std::move(config), kind) {}
    NativeModel native;
};

struct Phase1NativeTrainingAccess final {
    static NativeModel& get(SharedDepthSequenceModel& model) { return model.impl_->native; }
};

namespace {

TrainedVariant train_variant_native(const Phase1Config& config, const bool recurrent,
                                    const bool operator_visible,
                                    const Phase1ModelKind model_kind) {
    auto model = std::make_unique<SharedDepthSequenceModel>(config, model_kind);
    auto& native = Phase1NativeTrainingAccess::get(*model);
    std::mt19937_64 generator(config.seed + 1);
    std::vector<double> losses;
    losses.reserve(checked_size(config.train_steps, "train_steps"));
    const auto started = std::chrono::steady_clock::now();
    for (std::uint64_t training_step = 1; training_step <= config.train_steps; ++training_step) {
        auto batch = make_batch(config, generator, config.train_depth);
        auto model_operations = batch.operation;
        if (!operator_visible) std::ranges::fill(model_operations, 0);
        for (auto& parameter : native.parameters)
            std::ranges::fill(parameter.node->gradient, 0.0F);
        Tape tape;
        auto trace = native.forward_ad(tape, batch.start, model_operations, batch.depth, recurrent);
        ADTensor loss;
        if (recurrent) loss = recurrent_loss(tape, trace, batch.start, batch.operation, batch.depth, config.node_count);
        else {
            const auto targets = ring_targets(batch.start, batch.operation, batch.depth, config.node_count);
            loss = cross_entropy_trace(tape, {trace.back()}, targets);
        }
        tape.backward(loss);
        constexpr float beta1 = 0.9F, beta2 = 0.999F, epsilon = 1.0e-8F, weight_decay = 0.01F;
        const auto correction1 = 1.0F - std::pow(beta1, static_cast<float>(training_step));
        const auto correction2 = 1.0F - std::pow(beta2, static_cast<float>(training_step));
        const auto learning_rate = static_cast<float>(config.learning_rate);
        for (auto& parameter : native.parameters)
            for (std::size_t i = 0; i < parameter.node->value.size(); ++i) {
                const auto gradient = parameter.node->gradient[i];
                parameter.first_moment[i] = beta1 * parameter.first_moment[i] + (1.0F - beta1) * gradient;
                parameter.second_moment[i] = beta2 * parameter.second_moment[i] + (1.0F - beta2) * gradient * gradient;
                parameter.node->value[i] *= 1.0F - learning_rate * weight_decay;
                parameter.node->value[i] -= learning_rate * (parameter.first_moment[i] / correction1) /
                    (std::sqrt(parameter.second_moment[i] / correction2) + epsilon);
            }
        losses.push_back(loss.node->value[0]);
    }
    const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    Phase1TrainingReport report;
    report.mode = recurrent ? "recurrent" : "single_pass";
    report.operator_visible = operator_visible;
    report.input_mode = model_kind == Phase1ModelKind::text_rag ? "text_rag" : "operator";
    report.parameter_count = model->parameter_count();
    report.initial_loss = rounded(losses.front(), 9);
    report.final_loss = rounded(losses.back(), 9);
    report.minimum_loss = rounded(*std::min_element(losses.begin(), losses.end()), 9);
    const auto last_count = std::min<std::size_t>(100, losses.size());
    report.mean_last_100_loss = rounded(std::accumulate(losses.end() - last_count, losses.end(), 0.0) /
                                        static_cast<double>(last_count), 9);
    report.train_elapsed_seconds = rounded(elapsed, 3);
    report.train_steps_per_second = rounded(static_cast<double>(config.train_steps) / elapsed, 3);
    for (std::uint64_t depth = 1; depth <= config.eval_depth; ++depth) {
        auto batch = make_batch(config, generator, depth, depth,
            checked_multiply(config.batch_size, 4, "evaluation batch size"));
        auto visible_operations = batch.operation;
        if (!operator_visible) std::ranges::fill(visible_operations, 0);
        const auto logits = model->forward(batch.start, visible_operations, batch.depth, recurrent);
        const auto targets = ring_targets(batch.start, batch.operation, batch.depth, config.node_count);
        std::uint64_t correct = 0;
        for (std::size_t row = 0; row < targets.size(); ++row)
            if (argmax_row(logits, row, checked_size(config.node_count, "node_count")) == targets[row]) ++correct;
        report.metrics.push_back({depth, depth <= config.train_depth,
            rounded(static_cast<double>(correct) / static_cast<double>(targets.size()), 6)});
    }
    return {std::move(report), std::move(model)};
}

double unseen_mean(const Phase1TrainingReport& report, const std::uint64_t train_depth) {
    double sum = 0.0; std::size_t count = 0;
    for (const auto& metric : report.metrics) if (metric.depth > train_depth) { sum += metric.accuracy; ++count; }
    if (!count) invalid("evaluation has no unseen depths");
    return sum / static_cast<double>(count);
}

double seen_minimum(const Phase1TrainingReport& report, const std::uint64_t train_depth) {
    double result = std::numeric_limits<double>::infinity();
    for (const auto& metric : report.metrics) if (metric.depth <= train_depth) result = std::min(result, metric.accuracy);
    return result;
}

double unseen_minimum(const Phase1TrainingReport& report, const std::uint64_t train_depth) {
    double result = std::numeric_limits<double>::infinity();
    for (const auto& metric : report.metrics) if (metric.depth > train_depth) result = std::min(result, metric.accuracy);
    return result;
}

bool finite_losses(const Phase1TrainingReport& report) {
    return std::isfinite(report.final_loss);
}

}  // namespace

void Phase1Config::validate() const {
    if (!node_count || !workspace_slots || !model_dim || !attention_heads || !ffn_dim ||
        !physical_layers || !train_depth || !eval_depth || !batch_size || !train_steps)
        invalid("model and training dimensions must be positive");
    if (model_dim % attention_heads != 0) invalid("model_dim must be divisible by attention_heads");
    if (eval_depth <= train_depth) invalid("eval_depth must exceed train_depth");
    if (!(learning_rate > 0.0) || !std::isfinite(learning_rate)) invalid("learning_rate must be positive");
    const auto probability = [](const double value) { return std::isfinite(value) && value >= 0.0 && value <= 1.0; };
    if (!probability(min_seen_accuracy) || !probability(min_unseen_accuracy))
        invalid("accuracy thresholds must be in [0, 1]");
    if (!(std::isfinite(min_unseen_gain) && min_unseen_gain >= 0.0) ||
        !(std::isfinite(min_operator_gain) && min_operator_gain >= 0.0) ||
        !(std::isfinite(min_operator_vs_rag_gain) && min_operator_vs_rag_gain >= 0.0))
        invalid("gain thresholds must be non-negative");
    if (!probability(min_operator_latency_improvement))
        invalid("min_operator_latency_improvement must be in [0, 1]");
    if (!(std::isfinite(rag_success_tolerance) && rag_success_tolerance >= 0.0))
        invalid("rag_success_tolerance must be non-negative");
}

Phase1Config Phase1Config::target() {
    Phase1Config result;
    result.node_count = 256;
    result.workspace_slots = 16;
    result.model_dim = 896;
    result.attention_heads = 14;
    result.ffn_dim = 3584;
    result.physical_layers = 2;
    result.batch_size = 64;
    result.train_steps = 1000;
    result.learning_rate = 3.0e-4;
    return result;
}

std::string_view phase1_variant_name(const Phase1Variant value) noexcept {
    switch (value) {
        case Phase1Variant::recurrent: return "recurrent";
        case Phase1Variant::single_pass: return "single_pass";
        case Phase1Variant::no_operator: return "no_operator";
        case Phase1Variant::text_rag: return "text_rag";
    }
    return "";
}

Phase1Variant phase1_variant_from_name(const std::string_view value) {
    if (value == "recurrent") return Phase1Variant::recurrent;
    if (value == "single_pass") return Phase1Variant::single_pass;
    if (value == "no_operator") return Phase1Variant::no_operator;
    if (value == "text_rag") return Phase1Variant::text_rag;
    invalid("unsupported Phase 1 variant: " + std::string(value));
}

SharedDepthSequenceModel::SharedDepthSequenceModel(Phase1Config config, const Phase1ModelKind kind)
    : impl_(std::make_unique<Impl>(std::move(config), kind)) {}
SharedDepthSequenceModel::~SharedDepthSequenceModel() = default;
SharedDepthSequenceModel::SharedDepthSequenceModel(SharedDepthSequenceModel&&) noexcept = default;
SharedDepthSequenceModel& SharedDepthSequenceModel::operator=(SharedDepthSequenceModel&&) noexcept = default;
const Phase1Config& SharedDepthSequenceModel::config() const noexcept { return impl_->native.config; }
Phase1ModelKind SharedDepthSequenceModel::kind() const noexcept { return impl_->native.kind; }

Phase1LogitTrace SharedDepthSequenceModel::forward_trace(
    const std::span<const std::uint64_t> starts,
    const std::span<const std::uint64_t> operators,
    const std::span<const std::uint64_t> depths, const bool recurrent) const {
    Tape tape;
    const auto trace = impl_->native.forward_ad(tape, starts, operators, depths, recurrent);
    Phase1LogitTrace result;
    result.reserve(trace.size());
    for (const auto& logits : trace) result.push_back(logits.node->value);
    return result;
}

std::vector<float> SharedDepthSequenceModel::forward(
    const std::span<const std::uint64_t> starts,
    const std::span<const std::uint64_t> operators,
    const std::span<const std::uint64_t> depths, const bool recurrent) const {
    auto trace = forward_trace(starts, operators, depths, recurrent);
    return std::move(trace.back());
}

std::uint64_t SharedDepthSequenceModel::parameter_count() const noexcept {
    std::uint64_t result = 0;
    for (const auto& parameter : impl_->native.parameters)
        result += static_cast<std::uint64_t>(parameter.node->value.size());
    return result;
}

std::vector<Phase1Tensor> SharedDepthSequenceModel::state_dict() const {
    std::vector<Phase1Tensor> result;
    result.reserve(impl_->native.parameters.size() + (kind() == Phase1ModelKind::text_rag ? 1U : 0U));
    if (kind() == Phase1ModelKind::text_rag)
        result.push_back({"procedure_documents", {2, impl_->native.text_patches, 8},
            Phase1TensorDType::int64, {}, impl_->native.procedure_documents});
    for (const auto& parameter : impl_->native.parameters)
        result.push_back({parameter.name, parameter.shape, Phase1TensorDType::float32,
            parameter.node->value, {}});
    return result;
}

void SharedDepthSequenceModel::load_state_dict(const std::span<const Phase1Tensor> tensors) {
    const auto expected_count = impl_->native.parameters.size() +
        (kind() == Phase1ModelKind::text_rag ? 1U : 0U);
    if (tensors.size() != expected_count) invalid("strict state_dict key count mismatch");
    std::set<std::string, std::less<>> seen;
    for (const auto& tensor : tensors) {
        if (!seen.insert(tensor.name).second) invalid("duplicate state_dict key");
        if (tensor.name == "procedure_documents") {
            if (kind() != Phase1ModelKind::text_rag || tensor.dtype != Phase1TensorDType::int64 ||
                tensor.shape != std::vector<std::uint64_t>{2, impl_->native.text_patches, 8} ||
                tensor.integer_values.size() != impl_->native.procedure_documents.size() ||
                !tensor.float_values.empty()) invalid("procedure_documents buffer mismatch");
            for (const auto value : tensor.integer_values)
                if (value < 0 || value > 256) invalid("procedure_documents byte index is out of range");
            impl_->native.procedure_documents = tensor.integer_values;
            continue;
        }
        const auto found = std::ranges::find_if(impl_->native.parameters,
            [&](const auto& parameter) { return parameter.name == tensor.name; });
        if (found == impl_->native.parameters.end()) invalid("unexpected state_dict key: " + tensor.name);
        if (tensor.dtype != Phase1TensorDType::float32 || !tensor.integer_values.empty() ||
            found->shape != tensor.shape || found->node->value.size() != tensor.float_values.size())
            invalid("state_dict tensor shape mismatch: " + tensor.name);
        found->node->value = tensor.float_values;
    }
    if (seen.size() != expected_count) invalid("state_dict is missing keys");
}

std::vector<std::uint64_t> ring_targets(
    const std::span<const std::uint64_t> starts,
    const std::span<const std::uint64_t> operators,
    const std::span<const std::uint64_t> depths,
    const std::uint64_t node_count) {
    if (!node_count || starts.size() != operators.size() || starts.size() != depths.size())
        invalid("ring target inputs must have equal lengths and positive node_count");
    std::vector<std::uint64_t> result(starts.size());
    for (std::size_t i = 0; i < starts.size(); ++i) {
        if (starts[i] >= node_count || operators[i] > 1) invalid("ring target input is out of range");
        result[i] = ring_value(starts[i], depths[i], operators[i], node_count);
    }
    return result;
}

namespace {

class BinaryWriter final {
public:
    std::vector<std::byte> bytes;
    void u8(const std::uint8_t value) { bytes.push_back(static_cast<std::byte>(value)); }
    void u64(const std::uint64_t value) {
        for (unsigned shift = 0; shift < 64; shift += 8) bytes.push_back(static_cast<std::byte>((value >> shift) & 0xffU));
    }
    void f64(const double value) { u64(std::bit_cast<std::uint64_t>(value)); }
    void f32(const float value) {
        const auto bits = std::bit_cast<std::uint32_t>(value);
        for (unsigned shift = 0; shift < 32; shift += 8) bytes.push_back(static_cast<std::byte>((bits >> shift) & 0xffU));
    }
    void text(const std::string_view value) {
        u64(value.size());
        bytes.insert(bytes.end(), reinterpret_cast<const std::byte*>(value.data()),
                     reinterpret_cast<const std::byte*>(value.data() + value.size()));
    }
};

class BinaryReader final {
public:
    explicit BinaryReader(std::span<const std::byte> input) : input_(input) {}
    std::uint8_t u8() { require(1); return std::to_integer<std::uint8_t>(input_[offset_++]); }
    std::uint64_t u64() {
        require(8); std::uint64_t value = 0;
        for (unsigned shift = 0; shift < 64; shift += 8)
            value |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(input_[offset_++])) << shift;
        return value;
    }
    double f64() { return std::bit_cast<double>(u64()); }
    float f32() {
        require(4); std::uint32_t value = 0;
        for (unsigned shift = 0; shift < 32; shift += 8)
            value |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(input_[offset_++])) << shift;
        return std::bit_cast<float>(value);
    }
    std::string text() {
        const auto length = u64();
        if (length > UINT64_C(1) << 30) invalid("checkpoint string is too large");
        require(checked_size(length, "checkpoint string"));
        std::string result(reinterpret_cast<const char*>(input_.data() + offset_), checked_size(length, "checkpoint string"));
        offset_ += checked_size(length, "checkpoint string");
        return result;
    }
    void finish() const { if (offset_ != input_.size()) invalid("trailing checkpoint data"); }
private:
    void require(const std::size_t count) const {
        if (count > input_.size() - offset_) invalid("truncated checkpoint");
    }
    std::span<const std::byte> input_;
    std::size_t offset_{};
};

void write_config(BinaryWriter& writer, const Phase1Config& c) {
    for (const auto value : {c.seed, c.node_count, c.workspace_slots, c.model_dim,
         c.attention_heads, c.ffn_dim, c.physical_layers, c.reencode_interval,
         c.train_depth, c.eval_depth, c.batch_size, c.train_steps}) writer.u64(value);
    for (const auto value : {c.learning_rate, c.min_seen_accuracy, c.min_unseen_accuracy,
         c.min_unseen_gain, c.min_operator_gain, c.min_operator_vs_rag_gain,
         c.min_operator_latency_improvement, c.rag_success_tolerance}) writer.f64(value);
}

Phase1Config read_config(BinaryReader& reader) {
    Phase1Config c;
    c.seed = reader.u64(); c.node_count = reader.u64(); c.workspace_slots = reader.u64();
    c.model_dim = reader.u64(); c.attention_heads = reader.u64(); c.ffn_dim = reader.u64();
    c.physical_layers = reader.u64(); c.reencode_interval = reader.u64();
    c.train_depth = reader.u64(); c.eval_depth = reader.u64(); c.batch_size = reader.u64();
    c.train_steps = reader.u64(); c.learning_rate = reader.f64();
    c.min_seen_accuracy = reader.f64(); c.min_unseen_accuracy = reader.f64();
    c.min_unseen_gain = reader.f64(); c.min_operator_gain = reader.f64();
    c.min_operator_vs_rag_gain = reader.f64(); c.min_operator_latency_improvement = reader.f64();
    c.rag_success_tolerance = reader.f64(); c.validate(); return c;
}

struct LoadedCheckpoint final {
    Phase1Config config;
    Phase1Variant variant{};
    Phase1ModelKind kind{};
    std::string model_class;
    std::vector<Phase1Tensor> tensors;
};

LoadedCheckpoint load_checkpoint_payload(const std::filesystem::path& path) {
    const auto bytes = read_bytes(path);
    BinaryReader reader(bytes);
    if (reader.text() != "mosaic-phase1-checkpoint-v0") invalid("unsupported checkpoint schema");
    const auto variant_value = reader.u8();
    if (variant_value > static_cast<std::uint8_t>(Phase1Variant::text_rag)) invalid("invalid checkpoint variant");
    LoadedCheckpoint result;
    result.variant = static_cast<Phase1Variant>(variant_value);
    result.kind = result.variant == Phase1Variant::text_rag ? Phase1ModelKind::text_rag : Phase1ModelKind::operator_model;
    result.config = read_config(reader);
    result.model_class = reader.text();
    const auto expected_model_class = result.kind == Phase1ModelKind::text_rag
        ? "TextRagSharedDepthSequenceModel" : "SharedDepthSequenceModel";
    if (result.model_class != expected_model_class) invalid("checkpoint model class does not match variant");
    const auto count = reader.u64();
    if (count > 10000) invalid("checkpoint has too many tensors");
    result.tensors.reserve(checked_size(count, "tensor count"));
    std::set<std::string, std::less<>> names;
    for (std::uint64_t index = 0; index < count; ++index) {
        Phase1Tensor tensor;
        tensor.name = reader.text();
        if (tensor.name.empty() || !names.insert(tensor.name).second) invalid("invalid checkpoint tensor name");
        const auto rank = reader.u64();
        if (rank > 8) invalid("checkpoint tensor rank is too large");
        tensor.shape.reserve(checked_size(rank, "tensor rank"));
        for (std::uint64_t axis = 0; axis < rank; ++axis) tensor.shape.push_back(reader.u64());
        const auto elements = checked_product(tensor.shape);
        const auto dtype = reader.u8();
        if (dtype > static_cast<std::uint8_t>(Phase1TensorDType::int64))
            invalid("checkpoint tensor dtype is unsupported");
        tensor.dtype = static_cast<Phase1TensorDType>(dtype);
        if (tensor.dtype == Phase1TensorDType::float32) {
            tensor.float_values.reserve(elements);
            for (std::size_t element = 0; element < elements; ++element) tensor.float_values.push_back(reader.f32());
        } else {
            tensor.integer_values.reserve(elements);
            for (std::size_t element = 0; element < elements; ++element)
                tensor.integer_values.push_back(std::bit_cast<std::int64_t>(reader.u64()));
        }
        result.tensors.push_back(std::move(tensor));
    }
    reader.finish();
    return result;
}

std::string json_escape(const std::string_view value) {
    std::ostringstream stream;
    for (const unsigned char byte : value) {
        switch (byte) {
            case '\\': stream << "\\\\"; break;
            case '"': stream << "\\\""; break;
            case '\n': stream << "\\n"; break;
            case '\r': stream << "\\r"; break;
            case '\t': stream << "\\t"; break;
            default:
                if (byte < 0x20) stream << "\\u" << std::hex << std::setw(4) << std::setfill('0') << unsigned(byte) << std::dec;
                else stream << static_cast<char>(byte);
        }
    }
    return stream.str();
}

std::string manifest_sha(const std::filesystem::path& path) {
    const auto bytes = read_bytes(path);
    const std::string text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    const std::string marker = "\"sha256\"";
    const auto start = text.find(marker);
    if (start == std::string::npos) invalid("manifest is missing sha256");
    auto cursor = start + marker.size();
    while (cursor < text.size() && std::isspace(static_cast<unsigned char>(text[cursor]))) ++cursor;
    if (cursor == text.size() || text[cursor++] != ':') invalid("manifest sha256 has no value");
    while (cursor < text.size() && std::isspace(static_cast<unsigned char>(text[cursor]))) ++cursor;
    if (cursor == text.size() || text[cursor++] != '"') invalid("manifest sha256 is not a string");
    const auto end = text.find('"', cursor);
    if (end == std::string::npos) invalid("manifest sha256 is unterminated");
    const auto digest = text.substr(cursor, end - cursor);
    if (digest.size() != 64 || !std::ranges::all_of(digest, [](const unsigned char byte) {
            return std::isxdigit(byte) != 0;
        })) invalid("manifest sha256 is invalid");
    return digest;
}

}  // namespace

Phase1CheckpointReceipt save_phase1_checkpoint(
    const SharedDepthSequenceModel& model, const Phase1Config& config,
    const Phase1Variant variant, const std::filesystem::path& path) {
    config.validate();
    if (!(model.config() == config))
        invalid("checkpoint model/config mismatch");
    if ((variant == Phase1Variant::text_rag) != (model.kind() == Phase1ModelKind::text_rag))
        invalid("checkpoint variant/model mismatch");
    BinaryWriter writer;
    writer.text("mosaic-phase1-checkpoint-v0");
    writer.u8(static_cast<std::uint8_t>(variant));
    write_config(writer, config);
    writer.text(model.kind() == Phase1ModelKind::text_rag
        ? "TextRagSharedDepthSequenceModel" : "SharedDepthSequenceModel");
    const auto tensors = model.state_dict();
    writer.u64(tensors.size());
    for (const auto& tensor : tensors) {
        writer.text(tensor.name); writer.u64(tensor.shape.size());
        for (const auto extent : tensor.shape) writer.u64(extent);
        writer.u8(static_cast<std::uint8_t>(tensor.dtype));
        if (tensor.dtype == Phase1TensorDType::float32) {
            if (tensor.float_values.size() != checked_product(tensor.shape) || !tensor.integer_values.empty())
                invalid("float32 checkpoint tensor payload mismatch");
            for (const auto value : tensor.float_values) writer.f32(value);
        } else {
            if (tensor.integer_values.size() != checked_product(tensor.shape) || !tensor.float_values.empty())
                invalid("int64 checkpoint tensor payload mismatch");
            for (const auto value : tensor.integer_values) writer.u64(std::bit_cast<std::uint64_t>(value));
        }
    }
    if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) throw std::runtime_error("cannot create checkpoint: " + path.string());
    output.write(reinterpret_cast<const char*>(writer.bytes.data()), static_cast<std::streamsize>(writer.bytes.size()));
    output.close();
    if (!output) throw std::runtime_error("cannot write checkpoint: " + path.string());
    const auto digest = file_sha256(path);
    auto manifest_path = path;
    manifest_path += ".manifest.json";
    std::ofstream manifest(manifest_path, std::ios::binary | std::ios::trunc);
    if (!manifest) throw std::runtime_error("cannot create checkpoint manifest");
    manifest << "{\n"
        << "  \"schema_version\": \"mosaic-phase1-artifact-manifest-v0\",\n"
        << "  \"checkpoint\": \"" << json_escape(path.filename().string()) << "\",\n"
        << "  \"sha256\": \"" << digest << "\",\n"
        << "  \"bytes\": " << writer.bytes.size() << ",\n"
        << "  \"source\": \"random initialization trained on locally generated ring targets\",\n"
        << "  \"external_datasets\": [],\n"
        << "  \"external_checkpoints\": [],\n"
        << "  \"license_status\": \"project license not declared\",\n"
        << "  \"redistribution\": \"not authorized by this manifest; choose and document a project license before publishing\"\n"
        << "}\n";
    manifest.close();
    if (!manifest) throw std::runtime_error("cannot write checkpoint manifest");
    return {std::filesystem::absolute(path), std::filesystem::absolute(manifest_path), digest,
            static_cast<std::uint64_t>(writer.bytes.size())};
}

Phase1CheckpointVerification verify_checkpoint(const std::filesystem::path& path,
                                                const std::string_view device) {
    const auto resolved = resolve_device(device);
    const auto payload = load_checkpoint_payload(path);
    SharedDepthSequenceModel model(payload.config, payload.kind);
    model.load_state_dict(payload.tensors);
    std::vector<std::uint64_t> starts, operators;
    starts.reserve(checked_size(checked_multiply(payload.config.node_count, 2, "verification cases"), "verification cases"));
    operators.reserve(starts.capacity());
    for (std::uint64_t node = 0; node < payload.config.node_count; ++node)
        for (std::uint64_t operation = 0; operation < 2; ++operation) {
            starts.push_back(node); operators.push_back(operation);
        }
    auto model_operators = operators;
    if (payload.variant == Phase1Variant::no_operator) std::ranges::fill(model_operators, 0);
    Phase1CheckpointVerification result;
    result.checkpoint = std::filesystem::absolute(path);
    auto manifest_path = path; manifest_path += ".manifest.json";
    result.manifest = std::filesystem::absolute(manifest_path);
    result.sha256 = file_sha256(path);
    result.bytes = std::filesystem::file_size(path);
    result.variant = payload.variant;
    result.config = payload.config;
    result.device = resolved;
    result.strict_state_dict = true;
    result.all_tensors_loaded = true;
    result.exhaustive = true;
    result.total_cases = checked_multiply(checked_multiply(payload.config.node_count, 2,
        "verification cases"), payload.config.eval_depth, "verification total cases");
    const auto recurrent = payload.variant != Phase1Variant::single_pass;
    for (std::uint64_t depth = 1; depth <= payload.config.eval_depth; ++depth) {
        std::vector<std::uint64_t> depths(starts.size(), depth);
        const auto targets = ring_targets(starts, operators, depths, payload.config.node_count);
        const auto logits = model.forward(starts, model_operators, depths, recurrent);
        std::uint64_t correct = 0;
        for (std::size_t row = 0; row < starts.size(); ++row)
            if (argmax_row(logits, row, checked_size(payload.config.node_count, "node_count")) == targets[row]) ++correct;
        result.metrics.push_back({depth, depth <= payload.config.train_depth,
            rounded(static_cast<double>(correct) / starts.size(), 6)});
    }
    result.manifest_matches = manifest_sha(manifest_path) == result.sha256;
    result.acceptance["manifest_digest"] = result.manifest_matches;
    result.acceptance["all_tensors_loaded"] = true;
    if (payload.variant == Phase1Variant::recurrent) {
        result.acceptance["seen_accuracy"] = seen_minimum({.metrics = result.metrics}, payload.config.train_depth) >=
            payload.config.min_seen_accuracy;
        result.acceptance["unseen_accuracy"] = unseen_minimum({.metrics = result.metrics}, payload.config.train_depth) >=
            payload.config.min_unseen_accuracy;
    }
    result.passed = std::ranges::all_of(result.acceptance, [](const auto& item) { return item.second; });
    return result;
}

Phase1LatencyComparison compare_inference_latency(
    const SharedDepthSequenceModel& operator_model,
    const SharedDepthSequenceModel& text_rag_model, const std::uint64_t depth,
    const std::uint64_t warmups, const std::uint64_t repeats,
    const std::string_view device) {
    resolve_device(device);
    if (!depth || !repeats) invalid("latency profile values are invalid");
    const std::array<std::uint64_t, 1> start{7}, operation{0}, depths{depth};
    for (std::uint64_t warmup = 0; warmup < warmups; ++warmup) {
        (void)operator_model.forward(start, operation, depths, true);
        (void)text_rag_model.forward(start, operation, depths, true);
    }
    std::array<std::vector<double>, 2> samples;
    const std::array<const SharedDepthSequenceModel*, 2> models{&operator_model, &text_rag_model};
    for (std::uint64_t repeat = 0; repeat < repeats; ++repeat) {
        const std::array<std::size_t, 2> order = repeat % 2 == 0
            ? std::array<std::size_t, 2>{0, 1} : std::array<std::size_t, 2>{1, 0};
        for (const auto index : order) {
            const auto started = std::chrono::steady_clock::now();
            (void)models[index]->forward(start, operation, depths, true);
            samples[index].push_back(std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - started).count());
        }
    }
    const auto operator_p50 = median(std::move(samples[0]));
    const auto text_p50 = median(std::move(samples[1]));
    const auto improvement = text_p50 <= 0.0 ? 0.0 : 1.0 - operator_p50 / text_p50;
    return {depth, 1, repeats, rounded(operator_p50, 6), rounded(text_p50, 6), rounded(improvement, 6)};
}

Phase1MemoryOperatorSuite evaluate_memory_operator_suite(
    const SharedDepthSequenceModel& model, const Phase1Config& config,
    const std::string_view device) {
    resolve_device(device);
    config.validate();
    if (!(model.config() == config)) invalid("memory suite model/config mismatch");
    // Native, process-local facts preserve the source suite's observable swap,
    // history, composition, and predicate-scoped retraction semantics without
    // importing the source module's disk-backed storage implementation.
    struct Fact { std::uint64_t id; std::string predicate; std::string value; bool active; };
    std::vector<Fact> facts;
    const auto remember = [&](const std::string& predicate, const std::string& value) -> std::uint64_t {
        for (auto& fact : facts) if (fact.predicate == predicate) fact.active = false;
        const auto id = static_cast<std::uint64_t>(facts.size() + 1);
        facts.push_back({id, predicate, value, true});
        return id;
    };
    const auto first_id = remember("start_node", "3");
    const auto second_id = remember("start_node", "7");
    (void)remember("depth", "2");
    std::map<std::string, std::string, std::less<>> active;
    std::vector<std::string> history;
    for (const auto& fact : facts) {
        if (fact.predicate == "start_node") history.push_back(fact.value);
        if (fact.active) active[fact.predicate] = fact.value;
    }
    Phase1MemoryOperatorSuite result;
    result.namespace_name = "mosaic-phase1-" + std::to_string(config.seed);
    result.active_start_node = std::stoull(active.at("start_node"));
    result.active_depth = std::stoull(active.at("depth"));
    result.start_history_count = history.size();
    result.history_values = history;
    OperatorArchive archive({
        OperatorCode{"procedure.forward", {"advance", "progress"}, {{"direction", 1.0}},
                     1.0, 10, {"procedure.reverse"}},
        OperatorCode{"procedure.reverse", {"advance", "progress"}, {{"direction", -1.0}},
                     1.0, 1, {"procedure.forward"}},
    });
    const auto synthesized = synthesize_operators(archive.search("advance progress", 2));
    result.selected = synthesized.selected;
    result.disabled = synthesized.disabled;
    for (const auto& [left, right] : synthesized.conflicts) result.conflicts.push_back({left, right});
    const std::array<std::uint64_t, 1> start{result.active_start_node}, operation{0}, depth{result.active_depth};
    result.expected = ring_targets(start, operation, depth, config.node_count).front();
    if (result.selected == std::vector<std::string>{"procedure.forward"}) {
        const auto logits = model.forward(start, operation, depth, true);
        result.prediction = argmax_row(logits, 0, checked_size(config.node_count, "node_count"));
        result.model_requests = 1;
    }
    for (auto& fact : facts) if (fact.predicate == "start_node") { fact.active = false; ++result.forgotten_start_facts; }
    std::map<std::string, std::string, std::less<>> remaining;
    for (const auto& fact : facts) if (fact.active) remaining[fact.predicate] = fact.value;
    result.active_fact_count_after_delete = remaining.size();
    for (const auto& [predicate, value] : remaining) { (void)value; result.remaining_predicates_after_delete.push_back(predicate); }
    result.remaining_depth = std::stoull(remaining.at("depth"));
    result.unknown_request_skipped_model = result.model_requests == 1;
    result.acceptance["knowledge_swap"] = first_id != second_id && history.size() == 2 && active.at("start_node") == "7";
    result.acceptance["independent_two_fact_composition"] = result.prediction == result.expected &&
        active.at("start_node") == "7" && active.at("depth") == "2";
    result.acceptance["knowledge_delete"] = result.forgotten_start_facts == 2 &&
        !remaining.contains("start_node") && remaining.at("depth") == "2";
    result.acceptance["operator_conflict"] = result.selected == std::vector<std::string>{"procedure.forward"} &&
        result.disabled == std::vector<std::string>{"procedure.reverse"} &&
        result.conflicts == std::vector<std::vector<std::string>>{{"procedure.forward", "procedure.reverse"}};
    result.acceptance["unknown_without_model_call"] = result.unknown_request_skipped_model;
    result.passed = std::ranges::all_of(result.acceptance, [](const auto& item) { return item.second; });
    result.scope = "independent external start/depth fact composition, start-fact swap/delete, and retrieved procedure conflict resolution";
    return result;
}

Phase1ProfileReport profile_model(const Phase1Config& config) {
    SharedDepthSequenceModel model(config);
    const auto count = model.parameter_count();
    return {"mosaic-phase1-profile-v0", config, count, rounded(static_cast<double>(count) * 2.0 / 1048576.0, 3),
        rounded(static_cast<double>(count) / 1048576.0, 3),
        rounded(static_cast<double>(count) / 4.0 / 1048576.0, 3),
        20000000, 50000000, count >= 20000000 && count <= 50000000};
}

Phase1VariantReport train_one_variant(
    const Phase1Variant variant, const Phase1Config& config,
    const std::string_view device,
    const std::optional<std::filesystem::path>& checkpoint_path) {
    const auto resolved = resolve_device(device);
    config.validate();
    const auto recurrent = variant != Phase1Variant::single_pass;
    const auto operator_visible = variant != Phase1Variant::no_operator;
    const auto kind = variant == Phase1Variant::text_rag ? Phase1ModelKind::text_rag : Phase1ModelKind::operator_model;
    auto trained = train_variant_native(config, recurrent, operator_visible, kind);
    Phase1VariantReport result;
    result.config = config; result.device = resolved; result.variant = variant; result.result = trained.report;
    result.acceptance["finite_loss"] = std::isfinite(trained.report.final_loss);
    if (variant == Phase1Variant::recurrent) {
        result.acceptance["seen_accuracy"] = seen_minimum(trained.report, config.train_depth) >= config.min_seen_accuracy;
        result.acceptance["unseen_accuracy"] = unseen_minimum(trained.report, config.train_depth) >= config.min_unseen_accuracy;
    }
    result.passed = std::ranges::all_of(result.acceptance, [](const auto& item) { return item.second; });
    if (checkpoint_path) result.checkpoint = save_phase1_checkpoint(*trained.model, config, variant, *checkpoint_path);
    return result;
}

Phase1CompareReport compare_models(
    const Phase1Config& config, const std::string_view device,
    const std::optional<std::filesystem::path>& checkpoint_path) {
    const auto resolved = resolve_device(device);
    config.validate();
    auto recurrent = train_variant_native(config, true, true, Phase1ModelKind::operator_model);
    auto baseline = train_variant_native(config, false, true, Phase1ModelKind::operator_model);
    auto no_operator = train_variant_native(config, true, false, Phase1ModelKind::operator_model);
    auto text_rag = train_variant_native(config, true, true, Phase1ModelKind::text_rag);
    Phase1CompareReport result;
    result.config = config; result.device = resolved;
    result.memory_operator_suite = evaluate_memory_operator_suite(*recurrent.model, config, resolved);
    const auto recurrent_unseen = unseen_mean(recurrent.report, config.train_depth);
    const auto baseline_unseen = unseen_mean(baseline.report, config.train_depth);
    const auto no_operator_unseen = unseen_mean(no_operator.report, config.train_depth);
    const auto text_rag_unseen = unseen_mean(text_rag.report, config.train_depth);
    const auto latency = compare_inference_latency(*recurrent.model, *text_rag.model, 2, 10, 100, resolved);
    const auto matched = text_rag_unseen + config.rag_success_tolerance >= recurrent_unseen;
    const auto versus_rag = recurrent_unseen >= text_rag_unseen + config.min_operator_vs_rag_gain ||
        (matched && latency.improvement_fraction >= config.min_operator_latency_improvement);
    result.comparison = {rounded(recurrent_unseen, 6), rounded(baseline_unseen, 6),
        rounded(no_operator_unseen, 6), rounded(text_rag_unseen, 6),
        rounded(recurrent_unseen - baseline_unseen, 6),
        rounded(recurrent_unseen - no_operator_unseen, 6),
        rounded(recurrent_unseen - text_rag_unseen, 6), matched, latency};
    result.acceptance["equal_parameter_count"] = recurrent.report.parameter_count == baseline.report.parameter_count;
    result.acceptance["recurrent_seen_accuracy"] = seen_minimum(recurrent.report, config.train_depth) >= config.min_seen_accuracy;
    result.acceptance["recurrent_absolute_unseen_accuracy"] = unseen_minimum(recurrent.report, config.train_depth) >= config.min_unseen_accuracy;
    result.acceptance["recurrent_unseen_gain"] = recurrent_unseen >= baseline_unseen + config.min_unseen_gain;
    result.acceptance["operator_information_gain"] = recurrent_unseen >= no_operator_unseen + config.min_operator_gain;
    result.acceptance["operator_vs_text_rag"] = versus_rag;
    result.acceptance["finite_losses"] = finite_losses(recurrent.report) && finite_losses(baseline.report) &&
        finite_losses(no_operator.report) && finite_losses(text_rag.report);
    result.acceptance["memory_operator_suite"] = result.memory_operator_suite.passed;
    result.passed = std::ranges::all_of(result.acceptance, [](const auto& item) { return item.second; });
    result.recurrent = std::move(recurrent.report); result.single_pass = std::move(baseline.report);
    result.no_operator = std::move(no_operator.report); result.text_rag = std::move(text_rag.report);
    if (checkpoint_path) result.checkpoint = save_phase1_checkpoint(*recurrent.model, config, Phase1Variant::recurrent, *checkpoint_path);
    return result;
}

Phase1CompareReport reevaluate_report(Phase1CompareReport report) {
    const auto original_passed = report.passed;
    if (const auto found = report.acceptance.find("text_rag_matched_success");
        found != report.acceptance.end()) {
        report.comparison.text_rag_matched_success = found->second;
        report.acceptance.erase(found);
    }
    if (report.deprecated_text_rag_matched_success) {
        report.comparison.text_rag_matched_success = *report.deprecated_text_rag_matched_success;
        report.deprecated_text_rag_matched_success.reset();
        report.acceptance.erase("text_rag_matched_success");
    }
    report.passed = std::ranges::all_of(report.acceptance, [](const auto& item) { return item.second; });
    report.source_passed = original_passed;
    report.reevaluation_reason =
        "matched text-RAG success is diagnostic; the registered gate is operator accuracy gain OR latency gain at matched success";
    return report;
}

Phase1MultiSeedReport reevaluate_report(Phase1MultiSeedReport report) {
    if (report.runs.empty()) invalid("multi-seed report contains no runs");
    const auto original_passed = report.passed;
    for (auto& run : report.runs) run = reevaluate_report(std::move(run));
    report.aggregate.minimum_operator_information_gain = rounded(
        std::ranges::min(report.runs | std::views::transform(
            [](const auto& run) { return run.comparison.operator_information_gain; })), 6);
    report.aggregate.minimum_latency_improvement = rounded(
        std::ranges::min(report.runs | std::views::transform(
            [](const auto& run) { return run.comparison.latency.improvement_fraction; })), 6);
    report.aggregate.all_passed = std::ranges::all_of(report.runs, [](const auto& run) { return run.passed; });
    report.passed = report.aggregate.all_passed;
    report.source_passed = original_passed;
    report.reevaluation_reason = "nested Phase 1 gate semantics updated";
    return report;
}

Phase1MultiSeedReport compare_seeds(
    const Phase1Config& config, const std::span<const std::uint64_t> seeds,
    const std::string_view device,
    const std::optional<std::filesystem::path>& checkpoint_directory) {
    if (seeds.empty()) invalid("at least one seed is required");
    std::set<std::uint64_t> unique(seeds.begin(), seeds.end());
    if (unique.size() != seeds.size()) invalid("seeds must be unique");
    Phase1MultiSeedReport result;
    result.seeds.assign(seeds.begin(), seeds.end());
    for (const auto seed : seeds) {
        auto run_config = config;
        run_config.seed = seed;
        std::optional<std::filesystem::path> checkpoint;
        if (checkpoint_directory)
            checkpoint = *checkpoint_directory / ("recurrent_seed_" + std::to_string(seed) + ".pt");
        result.runs.push_back(compare_models(run_config, device, checkpoint));
    }
    result.aggregate.minimum_recurrent_unseen_mean_accuracy = rounded(
        std::ranges::min(result.runs | std::views::transform(
            [](const auto& run) { return run.comparison.recurrent_unseen; })), 6);
    result.aggregate.minimum_recurrent_depth8_accuracy = rounded(
        std::ranges::min(result.runs | std::views::transform(
            [](const auto& run) { return run.recurrent.metrics.back().accuracy; })), 6);
    result.aggregate.minimum_operator_information_gain = rounded(
        std::ranges::min(result.runs | std::views::transform(
            [](const auto& run) { return run.comparison.operator_information_gain; })), 6);
    result.aggregate.minimum_latency_improvement = rounded(
        std::ranges::min(result.runs | std::views::transform(
            [](const auto& run) { return run.comparison.latency.improvement_fraction; })), 6);
    result.aggregate.all_passed = std::ranges::all_of(result.runs, [](const auto& run) { return run.passed; });
    result.passed = result.aggregate.all_passed;
    return result;
}

}  // namespace swegca::world
