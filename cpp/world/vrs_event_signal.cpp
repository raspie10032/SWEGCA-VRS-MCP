#include "world/vrs_event_signal.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <limits>
#include <set>
#include <stdexcept>
#include <utility>

namespace swegca::world {

class EventSignalDependencies final {
    struct Posting final {
        std::uint32_t node{};
        std::uint32_t edge{};
    };
    struct Segment final {
        std::array<std::vector<Posting>, 2> directions;
        std::size_t size{};
    };

public:
    [[nodiscard]] static std::shared_ptr<const EventSignalDependencies> build(
        const std::size_t node_count,
        const PersistentEventVector<EventSignalEdge>& edges) {
        std::vector<std::shared_ptr<const Segment>> segments;
        segments.push_back(segment(edges, 0, edges.size()));
        return std::shared_ptr<const EventSignalDependencies>(
            new EventSignalDependencies(node_count, edges.size(), std::move(segments)));
    }

    [[nodiscard]] static std::shared_ptr<const EventSignalDependencies> advance(
        std::shared_ptr<const EventSignalDependencies> parent,
        const std::size_t parent_node_count, const std::size_t node_count,
        const std::size_t old_edge_count,
        const PersistentEventVector<EventSignalEdge>& edges) {
        if (!parent || parent->node_count_ != parent_node_count ||
            parent->edge_count_ != old_edge_count || node_count < parent_node_count ||
            old_edge_count > edges.size())
            throw std::invalid_argument("event dependency generation changed");
        auto segments = parent->segments_;
        if (old_edge_count != edges.size()) {
            segments.push_back(segment(edges, old_edge_count, edges.size()));
            while (segments.size() > 2 &&
                   2 * segments.back()->size >= segments[segments.size() - 2]->size) {
                auto combined = merge(
                    *segments[segments.size() - 2], *segments.back());
                segments.pop_back();
                segments.back() = std::move(combined);
            }
        }
        return std::shared_ptr<const EventSignalDependencies>(
            new EventSignalDependencies(node_count, edges.size(), std::move(segments)));
    }

    [[nodiscard]] std::vector<std::size_t> incoming(const std::size_t node) const {
        return edges_for(node, 1);
    }

    [[nodiscard]] std::vector<std::size_t> outgoing(const std::size_t node) const {
        return edges_for(node, 0);
    }

    [[nodiscard]] std::size_t segment_count() const noexcept { return segments_.size(); }
    [[nodiscard]] std::size_t index_bytes() const noexcept {
        std::size_t result = 0;
        for (const auto& segment : segments_)
            for (const auto& direction : segment->directions)
                result += direction.size() * sizeof(Posting);
        return result;
    }

private:
    EventSignalDependencies(
        const std::size_t node_count, const std::size_t edge_count,
        std::vector<std::shared_ptr<const Segment>> segments)
        : node_count_(node_count), edge_count_(edge_count), segments_(std::move(segments)) {}

    [[nodiscard]] static std::shared_ptr<const Segment> segment(
        const PersistentEventVector<EventSignalEdge>& edges,
        const std::size_t begin, const std::size_t end) {
        auto result = std::make_shared<Segment>();
        result->size = end - begin;
        for (std::size_t index = begin; index < end; ++index) {
            const auto edge = edges[index];
            result->directions[0].push_back(
                {edge.source, static_cast<std::uint32_t>(index)});
            result->directions[1].push_back(
                {edge.target, static_cast<std::uint32_t>(index)});
        }
        for (auto& direction : result->directions)
            std::stable_sort(direction.begin(), direction.end(), [](const Posting& a, const Posting& b) {
                return a.node < b.node;
            });
        return result;
    }

    [[nodiscard]] static std::shared_ptr<const Segment> merge(
        const Segment& left, const Segment& right) {
        auto result = std::make_shared<Segment>();
        result->size = left.size + right.size;
        for (std::size_t direction = 0; direction < 2; ++direction) {
            auto& output = result->directions[direction];
            output.reserve(result->size);
            output.insert(output.end(), left.directions[direction].begin(),
                          left.directions[direction].end());
            output.insert(output.end(), right.directions[direction].begin(),
                          right.directions[direction].end());
            std::stable_sort(output.begin(), output.end(), [](const Posting& a, const Posting& b) {
                return a.node < b.node;
            });
        }
        return result;
    }

    [[nodiscard]] std::vector<std::size_t> edges_for(
        const std::size_t node, const std::size_t direction) const {
        if (node >= node_count_) throw std::out_of_range("event node outside directory");
        std::vector<std::size_t> result;
        const auto probe = static_cast<std::uint32_t>(node);
        for (const auto& segment : segments_) {
            const auto& postings = segment->directions[direction];
            const auto lower = std::lower_bound(
                postings.begin(), postings.end(), probe,
                [](const Posting& row, const std::uint32_t value) { return row.node < value; });
            const auto upper = std::upper_bound(
                lower, postings.end(), probe,
                [](const std::uint32_t value, const Posting& row) { return value < row.node; });
            for (auto at = lower; at != upper; ++at) result.push_back(at->edge);
        }
        return result;
    }

    const std::size_t node_count_;
    const std::size_t edge_count_;
    const std::vector<std::shared_ptr<const Segment>> segments_;
};

namespace {

[[nodiscard]] bool digest(const std::string_view value) noexcept {
    return value.size() == 64 && std::all_of(value.begin(), value.end(), [](const char byte) {
        return (byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f');
    });
}

[[nodiscard]] float f32(const double value) {
    if (!std::isfinite(value) || std::abs(value) > std::numeric_limits<float>::max()) {
        throw std::invalid_argument("event arithmetic produced an unrepresentable value");
    }
    return static_cast<float>(value);
}

[[nodiscard]] bool same(const float left, const float right) noexcept {
    return std::bit_cast<std::uint32_t>(left) == std::bit_cast<std::uint32_t>(right);
}

// Expansion summation used by Python math.fsum. The final float32 cast is the
// persisted numerical boundary; all inputs here are finite.
[[nodiscard]] double accurate_sum(const std::vector<double>& values) {
    std::vector<double> partials;
    partials.reserve(values.size());
    for (double x : values) {
        std::size_t next = 0;
        for (double y : partials) {
            if (std::abs(x) < std::abs(y)) std::swap(x, y);
            const double high = x + y;
            const double low = y - (high - x);
            if (low != 0.0) partials[next++] = low;
            x = high;
        }
        partials.resize(next);
        partials.push_back(x);
    }
    double result = 0.0;
    for (auto iterator = partials.rbegin(); iterator != partials.rend(); ++iterator) {
        result += *iterator;
    }
    return result;
}

[[nodiscard]] std::uint16_t float_to_half(const float value) {
    const auto bits = std::bit_cast<std::uint32_t>(value);
    const auto sign = static_cast<std::uint16_t>((bits >> 16U) & 0x8000U);
    const auto exponent = static_cast<int>((bits >> 23U) & 0xffU);
    const auto fraction = bits & 0x7fffffU;
    if (exponent == 0xff) {
        return static_cast<std::uint16_t>(sign | 0x7c00U | (fraction != 0 ? 0x0200U : 0U));
    }
    int half_exponent = exponent - 127 + 15;
    if (half_exponent >= 31) return static_cast<std::uint16_t>(sign | 0x7c00U);
    if (half_exponent <= 0) {
        if (half_exponent < -10) return sign;
        std::uint32_t mantissa = fraction | 0x800000U;
        const unsigned shift = static_cast<unsigned>(14 - half_exponent);
        const std::uint32_t quotient = mantissa >> shift;
        const std::uint32_t remainder = mantissa & ((std::uint32_t{1} << shift) - 1U);
        const std::uint32_t halfway = std::uint32_t{1} << (shift - 1U);
        return static_cast<std::uint16_t>(
            sign | (quotient + static_cast<std::uint32_t>(
                remainder > halfway || (remainder == halfway && (quotient & 1U)))));
    }
    std::uint32_t quotient = fraction >> 13U;
    const std::uint32_t remainder = fraction & 0x1fffU;
    quotient += static_cast<std::uint32_t>(
        remainder > 0x1000U || (remainder == 0x1000U && (quotient & 1U)));
    if (quotient == 0x400U) {
        quotient = 0;
        ++half_exponent;
        if (half_exponent >= 31) return static_cast<std::uint16_t>(sign | 0x7c00U);
    }
    return static_cast<std::uint16_t>(
        sign | static_cast<std::uint16_t>(half_exponent << 10U) |
        static_cast<std::uint16_t>(quotient));
}

[[nodiscard]] float half_to_float(const std::uint16_t half) {
    const auto sign = static_cast<std::uint32_t>(half & 0x8000U) << 16U;
    const auto exponent = static_cast<std::uint32_t>((half >> 10U) & 0x1fU);
    const auto fraction = static_cast<std::uint32_t>(half & 0x03ffU);
    std::uint32_t bits = 0;
    if (exponent == 0) {
        if (fraction == 0) bits = sign;
        else {
            auto normalized = fraction;
            int shift = 0;
            while ((normalized & 0x0400U) == 0) { normalized <<= 1U; ++shift; }
            normalized &= 0x03ffU;
            bits = sign | static_cast<std::uint32_t>((127 - 14 - shift) << 23U) |
                (normalized << 13U);
        }
    } else if (exponent == 0x1fU) {
        bits = sign | 0x7f800000U | (fraction << 13U);
    } else {
        bits = sign | ((exponent + 127U - 15U) << 23U) | (fraction << 13U);
    }
    return std::bit_cast<float>(bits);
}

[[nodiscard]] float stored_strength(const double value, const EventStrengthStorage storage) {
    const float rounded = f32(value);
    if (storage == EventStrengthStorage::float32) return rounded;
    const float half = half_to_float(float_to_half(rounded));
    if (!std::isfinite(half)) throw std::invalid_argument("event strength exceeds durable f16 capacity");
    if ((value >= 1.0) != (half >= 1.0F)) {
        throw std::invalid_argument("durable f16 rounding changes experience promotion");
    }
    return half;
}

[[nodiscard]] std::size_t connection_index(
    const std::string_view connection, const std::string_view name_space) {
    if (!connection.starts_with(name_space)) {
        throw std::invalid_argument("re-evidence connection namespace changed");
    }
    const auto suffix = connection.substr(name_space.size());
    if (suffix.empty() || !std::all_of(suffix.begin(), suffix.end(), [](const char byte) {
            return byte >= '0' && byte <= '9';
        })) {
        throw std::invalid_argument("invalid connection address");
    }
    std::uint64_t parsed = 0;
    const auto result = std::from_chars(suffix.data(), suffix.data() + suffix.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != suffix.data() + suffix.size() ||
        parsed > std::numeric_limits<std::size_t>::max()) {
        throw std::invalid_argument("invalid connection address");
    }
    return static_cast<std::size_t>(parsed);
}

}  // namespace

std::uint16_t event_strength_float16_bits(const float value) {
    return float_to_half(value);
}

float event_strength_from_float16_bits(const std::uint16_t value) {
    return half_to_float(value);
}

EventSignalInputs::EventSignalInputs(
    std::string snapshot_id_value, std::vector<float> direct_value,
    std::vector<float> score_value, std::vector<EventSignalEdge> edges_value,
    std::vector<float> strength_value, std::vector<std::uint8_t> unresolved_value)
    : snapshot_id(std::move(snapshot_id_value)), direct(std::move(direct_value)),
      score(std::move(score_value)), edges(std::move(edges_value)),
      strength(std::move(strength_value)), unresolved(std::move(unresolved_value)) {
    if (!digest(snapshot_id)) throw std::invalid_argument("event inputs need a generation digest");
    if (direct.size() != score.size() || unresolved.size() != score.size() ||
        strength.size() != edges.size()) {
        throw std::invalid_argument("event input shape or dtype changed");
    }
    for (std::size_t index = 0; index < score.size(); ++index)
        if (!std::isfinite(direct[index]) || !std::isfinite(score[index]) || unresolved[index] > 1)
            throw std::invalid_argument("event input contains invalid values");
    for (std::size_t index = 0; index < strength.size(); ++index)
        if (!std::isfinite(strength[index]) || strength[index] < 0)
            throw std::invalid_argument("event input contains invalid values");
    for (std::size_t index = 0; index != edges.size(); ++index) {
        const auto& edge = edges[index];
        if (edge.source >= score.size() || edge.target >= score.size() ||
            edge.sign < -1 || edge.sign > 1 || !std::isfinite(edge.vrs_strength) ||
            edge.vrs_strength < 0) {
            throw std::invalid_argument("event edges or strength changed");
        }
    }
    dependencies_ = EventSignalDependencies::build(score.size(), edges);
}

EventSignalInputs::EventSignalInputs(
    std::string snapshot_id_value, PersistentEventVector<float> direct_value,
    PersistentEventVector<float> score_value,
    PersistentEventVector<EventSignalEdge> edges_value,
    PersistentEventVector<float> strength_value,
    PersistentEventVector<std::uint8_t> unresolved_value,
    std::shared_ptr<const EventSignalInputs> delta_parent_value,
    std::vector<std::size_t> score_indices_value,
    std::vector<std::size_t> strength_indices_value,
    std::vector<std::size_t> direct_indices_value)
    : snapshot_id(std::move(snapshot_id_value)), direct(std::move(direct_value)),
      score(std::move(score_value)), edges(std::move(edges_value)),
      strength(std::move(strength_value)), unresolved(std::move(unresolved_value)),
      delta_parent_(std::move(delta_parent_value)),
      score_indices_(std::move(score_indices_value)),
      strength_indices_(std::move(strength_indices_value)),
      direct_indices_(std::move(direct_indices_value)) {
    if (!digest(snapshot_id) || !delta_parent_ || direct.size() != score.size() ||
        unresolved.size() != score.size() || strength.size() != edges.size())
        throw std::invalid_argument("event delta node/edge counts disagree");
    const auto old_edges = delta_parent_->edges.size();
    for (std::size_t index = old_edges; index < edges.size(); ++index) {
        const auto edge = edges[index];
        if (edge.source >= score.size() || edge.target >= score.size())
            throw std::invalid_argument("event endpoint is outside the node directory");
    }
    dependencies_ = EventSignalDependencies::advance(
        delta_parent_->dependencies_, delta_parent_->score.size(), score.size(),
        old_edges, edges);
}

EventSignalInputs::EventSignalInputs(
    std::string snapshot_id_value, PersistentEventVector<float> direct_value,
    PersistentEventVector<float> score_value,
    PersistentEventVector<EventSignalEdge> edges_value,
    PersistentEventVector<float> strength_value,
    PersistentEventVector<std::uint8_t> unresolved_value,
    std::shared_ptr<const EventSignalDependencies> dependencies_value)
    : snapshot_id(std::move(snapshot_id_value)), direct(std::move(direct_value)),
      score(std::move(score_value)), edges(std::move(edges_value)),
      strength(std::move(strength_value)), unresolved(std::move(unresolved_value)),
      dependencies_(std::move(dependencies_value)) {
    if (!digest(snapshot_id) || !dependencies_ || direct.size() != score.size() ||
        unresolved.size() != score.size() || strength.size() != edges.size())
        throw std::invalid_argument("event successor generation changed");
}

std::vector<std::size_t> EventSignalInputs::incoming(const std::size_t node) const {
    return dependencies_->incoming(node);
}

std::vector<std::size_t> EventSignalInputs::outgoing(const std::size_t node) const {
    return dependencies_->outgoing(node);
}

std::size_t EventSignalInputs::dependency_segment_count() const noexcept {
    return dependencies_->segment_count();
}

std::size_t EventSignalInputs::dependency_index_bytes() const noexcept {
    return dependencies_->index_bytes();
}

EventSignalProposal::EventSignalProposal(
    const EventSignalInputs* inputs, std::map<std::size_t, float> scores_value,
    std::map<std::size_t, float> strengths_value,
    std::vector<std::size_t> pending_nodes_value, const std::uint64_t rounds_value,
    const std::uint64_t node_evaluations_value, const std::uint64_t edge_evaluations_value,
    std::vector<std::size_t> seed_nodes_value,
    std::shared_ptr<const DetachedVrsStateUpdateReceipt> strength_receipt_value,
    const EventStrengthStorage strength_storage_value)
    : scores(std::move(scores_value)), strengths(std::move(strengths_value)),
      pending_nodes(std::move(pending_nodes_value)), rounds(rounds_value),
      node_evaluations(node_evaluations_value), edge_evaluations(edge_evaluations_value),
      seed_nodes(std::move(seed_nodes_value)), strength_receipt(std::move(strength_receipt_value)),
      strength_storage(strength_storage_value), inputs_(inputs) {}

EventSignalProposal settle_event_signal(
    const EventSignalInputs& inputs, const std::span<const std::size_t> changed_nodes,
    std::shared_ptr<const DetachedVrsStateUpdateReceipt> strength_updates,
    const std::string_view connection_namespace, const EventSignalProposal* previous,
    const std::uint64_t maximum_rounds,
    const std::optional<EventStrengthStorage> requested_storage) {
    const auto strength_storage = requested_storage.value_or(
        previous == nullptr ? EventStrengthStorage::float32 : previous->strength_storage);
    std::map<std::size_t, float> scores;
    std::map<std::size_t, float> strengths;
    std::set<std::size_t> pending;
    std::vector<std::size_t> seeds;
    std::uint64_t rounds = 0;
    std::uint64_t node_evaluations = 0;
    std::uint64_t edge_evaluations = 0;
    std::shared_ptr<const DetachedVrsStateUpdateReceipt> receipt;
    if (previous != nullptr) {
        if (previous->inputs_ != &inputs || !changed_nodes.empty() || strength_updates ||
            (requested_storage && previous->strength_storage != *requested_storage)) {
            throw std::invalid_argument("signal resume generation or event changed");
        }
        scores = previous->scores;
        strengths = previous->strengths;
        pending.insert(previous->pending_nodes.begin(), previous->pending_nodes.end());
        seeds = previous->seed_nodes;
        rounds = previous->rounds;
        node_evaluations = previous->node_evaluations;
        edge_evaluations = previous->edge_evaluations;
        receipt = previous->strength_receipt;
    } else {
        pending.insert(changed_nodes.begin(), changed_nodes.end());
        if (!pending.empty() && *pending.rbegin() >= inputs.score.size()) {
            throw std::invalid_argument("signal seed outside node directory");
        }
        if (connection_namespace != "vrs-edge:" && connection_namespace != "vrs-edge-group:") {
            throw std::invalid_argument("explicit connection namespace required");
        }
        if (strength_updates) {
            if (strength_updates->snapshot_id != inputs.snapshot_id) {
                throw std::invalid_argument("re-evidence proposal generation changed");
            }
            std::set<std::size_t> seen;
            for (const auto& row : strength_updates->updates) {
                const auto edge = connection_index(row.connection_id, connection_namespace);
                if (edge >= inputs.edges.size() || !seen.insert(edge).second) {
                    throw std::invalid_argument("duplicate or unknown connection address");
                }
                if (row.previous_strength != static_cast<double>(inputs.strength[edge])) {
                    throw std::invalid_argument("re-evidence strength does not match immutable input");
                }
                double expected = row.previous_strength;
                if (row.update_action == "reinforce" && row.verdict == CurrentEvidenceVerdict::support) {
                    expected *= legacy_vrs_stable_reinforcement_factor;
                } else if (row.update_action == "weaken" && row.verdict == CurrentEvidenceVerdict::refute) {
                    expected *= legacy_vrs_unstable_weakening_factor;
                } else if (row.update_action == "abstain_conflict") {
                } else if (row.update_action == "preserve_unresolved" &&
                           row.verdict != CurrentEvidenceVerdict::support &&
                           row.verdict != CurrentEvidenceVerdict::refute) {
                } else {
                    throw std::invalid_argument("re-evidence proposal action changed");
                }
                if (row.current_strength != expected ||
                    row.promotion != assess_vrs_experience_promotion(
                        strength_updates->snapshot_id, row.connection_id,
                        row.previous_strength, expected)) {
                    throw std::invalid_argument("re-evidence strength or promotion binding changed");
                }
                const float stored = stored_strength(expected, strength_storage);
                if (!same(stored, inputs.strength[edge])) {
                    strengths.emplace(edge, stored);
                    pending.insert(inputs.edges[edge].target);
                }
            }
        }
        seeds.assign(pending.begin(), pending.end());
        receipt = std::move(strength_updates);
    }

    struct IncomingTerm final { std::size_t source; int sign; float weight; float baseline; };
    struct Topology final {
        std::vector<IncomingTerm> incoming;
        double denominator{};
        std::vector<std::size_t> outgoing_targets;
        float direct{};
        float original{};
    };
    std::map<std::size_t, Topology> topology;
    std::map<std::size_t, float> initial_scores;
    const auto initial = [&](const std::size_t node) -> float {
        const auto [at, inserted] = initial_scores.emplace(node, inputs.score[node]);
        (void)inserted;
        return at->second;
    };
    for (std::uint64_t iteration = 0; iteration != maximum_rounds && !pending.empty(); ++iteration) {
        std::map<std::size_t, float> next_scores;
        std::map<std::size_t, std::vector<std::size_t>> outgoing;
        std::uint64_t reads = 0;
        for (const auto node : pending) {
            auto found = topology.find(node);
            if (found == topology.end()) {
                Topology value;
                std::vector<double> absolute_weights;
                for (const auto edge_index : inputs.incoming(node)) {
                    const auto edge = inputs.edges[edge_index];
                    const auto changed = strengths.find(edge_index);
                    const float weight = changed == strengths.end() ? inputs.strength[edge_index] : changed->second;
                    value.incoming.push_back({edge.source, edge.sign, weight, initial(edge.source)});
                    absolute_weights.push_back(std::abs(static_cast<double>(weight)));
                }
                value.denominator = std::max(1.0, accurate_sum(absolute_weights));
                for (const auto edge_index : inputs.outgoing(node)) {
                    value.outgoing_targets.push_back(inputs.edges[edge_index].target);
                }
                value.direct = inputs.direct[node];
                value.original = initial(node);
                found = topology.emplace(node, std::move(value)).first;
            }
            const auto& value = found->second;
            std::vector<double> signal_terms;
            signal_terms.reserve(value.incoming.size());
            for (const auto& term : value.incoming) {
                const auto changed = scores.find(term.source);
                const float score = changed == scores.end() ? term.baseline : changed->second;
                signal_terms.push_back(static_cast<double>(score) * term.sign * term.weight);
            }
            const double signal = accurate_sum(signal_terms);
            const auto changed = scores.find(node);
            const float old = changed == scores.end() ? value.original : changed->second;
            next_scores.emplace(node, f32(
                0.8 * static_cast<double>(old) +
                0.2 * std::tanh(static_cast<double>(value.direct) + 0.2 * signal / value.denominator)));
            outgoing.emplace(node, value.outgoing_targets);
            reads += value.incoming.size();
        }
        std::set<std::size_t> following;
        for (const auto& [node, value] : next_scores) {
            const auto current_at = scores.find(node);
            const float current = current_at == scores.end() ? initial_scores.at(node) : current_at->second;
            if (!same(value, current)) {
                following.insert(node);
                const auto& targets = outgoing.at(node);
                following.insert(targets.begin(), targets.end());
            }
            if (same(value, initial_scores.at(node))) scores.erase(node);
            else scores[node] = value;
        }
        ++rounds;
        node_evaluations += pending.size();
        edge_evaluations += reads;
        pending = std::move(following);
    }
    return EventSignalProposal(
        &inputs, std::move(scores), std::move(strengths),
        std::vector<std::size_t>(pending.begin(), pending.end()), rounds,
        node_evaluations, edge_evaluations, std::move(seeds), std::move(receipt),
        strength_storage);
}

}  // namespace swegca::world
