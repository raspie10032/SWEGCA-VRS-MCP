#include "world/vrs_event_kernel.hpp"

#include "world/detached_vrs_state_update.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <set>
#include <stdexcept>
#include <utility>

namespace swegca::world {
namespace {

[[nodiscard]] float f32(const double value) {
    if (!std::isfinite(value) || std::abs(value) > std::numeric_limits<float>::max())
        throw std::invalid_argument("event arithmetic produced an unrepresentable value");
    return static_cast<float>(value);
}

[[nodiscard]] bool same(const float left, const float right) noexcept {
    return std::bit_cast<std::uint32_t>(left) == std::bit_cast<std::uint32_t>(right);
}

// Expansion summation follows the Python math.fsum boundary before the
// explicitly persisted float32 rounding.
[[nodiscard]] double accurate_sum(const std::vector<double>& values) {
    std::vector<double> partials;
    partials.reserve(values.size());
    for (double x : values) {
        std::size_t next = 0;
        for (double y : partials) {
            if (std::abs(x) < std::abs(y)) std::swap(x, y);
            const auto high = x + y;
            const auto low = y - (high - x);
            if (low != 0.0) partials[next++] = low;
            x = high;
        }
        partials.resize(next);
        partials.push_back(x);
    }
    double result = 0.0;
    for (auto at = partials.rbegin(); at != partials.rend(); ++at) result += *at;
    return result;
}

template<class Map, class Base>
[[nodiscard]] float current(const Map& changed, const Base& base,
                            const std::size_t index) {
    const auto found = changed.find(index);
    return found == changed.end() ? base[index] : found->second;
}

}  // namespace

EventVrsProposal::EventVrsProposal(
    const EventSignalInputs* inputs,
    std::map<std::size_t, float> score_values,
    std::map<std::size_t, float> strength_values,
    std::vector<std::size_t> pending,
    const std::uint64_t round_count,
    const std::uint64_t node_count,
    const std::uint64_t edge_count,
    std::vector<std::size_t> seeds)
    : scores(std::move(score_values)), strengths(std::move(strength_values)),
      pending_nodes(std::move(pending)), rounds(round_count),
      node_evaluations(node_count), edge_evaluations(edge_count),
      seed_nodes(std::move(seeds)), inputs_(inputs) {}

EventVrsReceipt EventVrsProposal::receipt() const noexcept {
    return {event_vrs_version, inputs_->snapshot_id,
            pending_nodes.empty() ? std::string_view("event_fixed_point")
                                  : std::string_view("pending"),
            pending_nodes.size(), rounds, node_evaluations, edge_evaluations,
            scores.size(), strengths.size(), false, false, false, false, false, false};
}

EventVrsProposal advance_event_vrs(
    const EventSignalInputs& inputs,
    const std::span<const std::size_t> changed_nodes,
    const EventVrsProposal* previous,
    const std::uint64_t maximum_rounds) {
    std::map<std::size_t, float> scores;
    std::map<std::size_t, float> strengths;
    std::set<std::size_t> pending;
    std::vector<std::size_t> seeds;
    std::uint64_t rounds = 0;
    std::uint64_t node_count = 0;
    std::uint64_t edge_count = 0;

    if (previous) {
        if (!previous->belongs_to(inputs) || !changed_nodes.empty())
            throw std::invalid_argument(
                "pending event belongs to a different generation or event");
        scores = previous->scores;
        strengths = previous->strengths;
        pending.insert(previous->pending_nodes.begin(), previous->pending_nodes.end());
        seeds = previous->seed_nodes;
        rounds = previous->rounds;
        node_count = previous->node_evaluations;
        edge_count = previous->edge_evaluations;
    } else {
        seeds.assign(changed_nodes.begin(), changed_nodes.end());
        std::ranges::sort(seeds);
        seeds.erase(std::unique(seeds.begin(), seeds.end()), seeds.end());
        for (const auto node : seeds)
            if (node >= inputs.score.size())
                throw std::invalid_argument("event seed is outside the node directory");
        pending.insert(seeds.begin(), seeds.end());
    }

    for (std::uint64_t pass = 0; pass != maximum_rounds && !pending.empty(); ++pass) {
        std::map<std::size_t, float> next_scores;
        std::set<std::size_t> incident;
        std::map<std::size_t, std::vector<std::size_t>> outgoing;
        for (const auto node : pending) {
            const auto incoming = inputs.incoming(node);
            auto& node_outgoing = outgoing[node];
            node_outgoing = inputs.outgoing(node);
            incident.insert(incoming.begin(), incoming.end());
            incident.insert(node_outgoing.begin(), node_outgoing.end());

            std::vector<double> degree_terms;
            std::vector<double> signal_terms;
            degree_terms.reserve(incoming.size());
            signal_terms.reserve(incoming.size());
            for (const auto edge_index : incoming) {
                const auto edge = inputs.edges[edge_index];
                const auto edge_strength = current(strengths, inputs.strength, edge_index);
                degree_terms.push_back(std::abs(static_cast<double>(edge_strength)));
                signal_terms.push_back(
                    static_cast<double>(current(scores, inputs.score, edge.source)) *
                    static_cast<double>(edge.sign) * edge_strength);
            }
            const auto degree = std::max(1.0, accurate_sum(degree_terms));
            const auto signal = accurate_sum(signal_terms);
            const auto candidate = std::tanh(
                static_cast<double>(inputs.direct[node]) + 0.2 * signal / degree);
            next_scores[node] = f32(
                0.8 * current(scores, inputs.score, node) + 0.2 * candidate);
        }

        std::map<std::size_t, float> next_strengths;
        for (const auto edge_index : incident) {
            const auto edge = inputs.edges[edge_index];
            const auto next_or_current = [&](const std::size_t node) {
                const auto found = next_scores.find(node);
                return found == next_scores.end()
                    ? current(scores, inputs.score, node) : found->second;
            };
            const auto left = next_or_current(edge.source);
            const auto right = next_or_current(edge.target);
            const bool stable =
                1.0 - 0.5 * std::abs(
                    static_cast<double>(left) * edge.sign - right) >= 0.75 &&
                std::abs(left) + std::abs(right) >= 0.1 &&
                !inputs.unresolved[edge.source] && !inputs.unresolved[edge.target];
            const auto factor = stable ? legacy_vrs_stable_reinforcement_factor
                                       : legacy_vrs_unstable_weakening_factor;
            const auto base = static_cast<double>(edge.vrs_strength);
            const auto value = std::clamp(
                static_cast<double>(current(strengths, inputs.strength, edge_index)) * factor,
                base * 0.25, base * 4.0);
            next_strengths[edge_index] = f32(value);
        }

        std::set<std::size_t> next_pending;
        for (const auto& [node, value] : next_scores) {
            if (!same(value, current(scores, inputs.score, node))) {
                next_pending.insert(node);
                for (const auto edge_index : outgoing[node])
                    next_pending.insert(inputs.edges[edge_index].target);
            }
            if (same(value, inputs.score[node])) scores.erase(node);
            else scores[node] = value;
        }
        for (const auto& [edge_index, value] : next_strengths) {
            if (!same(value, current(strengths, inputs.strength, edge_index)))
                next_pending.insert(inputs.edges[edge_index].target);
            if (same(value, inputs.strength[edge_index])) strengths.erase(edge_index);
            else strengths[edge_index] = value;
        }
        node_count += pending.size();
        edge_count += incident.size();
        pending = std::move(next_pending);
        ++rounds;
    }

    return EventVrsProposal(
        &inputs, std::move(scores), std::move(strengths),
        std::vector<std::size_t>(pending.begin(), pending.end()), rounds,
        node_count, edge_count, std::move(seeds));
}

}  // namespace swegca::world
