#include "event_vrs_kernel.hpp"

#include "python_fsum.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <set>
#include <stdexcept>
#include <utility>

namespace swegca::vrs {
namespace {

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_kernel.py@7536139:123-126
float f32(double value) {
    if (!std::isfinite(value) ||
        std::fabs(value) > static_cast<double>(std::numeric_limits<float>::max()))
        throw std::runtime_error("event arithmetic produced an unrepresentable value");
    return static_cast<float>(value);
}

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_kernel.py@7536139:129-131
bool same_f32(float left, float right) {
    return std::bit_cast<std::uint32_t>(left) == std::bit_cast<std::uint32_t>(right);
}

}  // namespace

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_kernel.py@7536139:101-110
EventVrsProposal::EventVrsProposal(
    std::shared_ptr<const ValidatedEventVrsInputs> inputs, std::map<std::uint32_t, float> scores,
    std::map<std::uint32_t, float> strengths, std::vector<std::uint32_t> pending_nodes,
    std::uint64_t rounds, std::uint64_t node_evaluations,
    std::uint64_t edge_evaluations, std::vector<std::uint32_t> seed_nodes)
    : inputs_(std::move(inputs)), scores_(std::move(scores)), strengths_(std::move(strengths)),
      pending_nodes_(std::move(pending_nodes)), rounds_(rounds), node_evaluations_(node_evaluations),
      edge_evaluations_(edge_evaluations), seed_nodes_(std::move(seed_nodes)) {}

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_kernel.py@7536139:112-120
Json EventVrsProposal::receipt() const {
    Json::Object result;
    result.emplace("version", Json(std::string("vrs-event-synchronous-f32-v1-experimental")));
    result.emplace("parent_snapshot_id", Json(inputs_->require_validated_immutable().snapshot_id()));
    result.emplace("status", Json(std::string(pending_nodes_.empty() ? "event_fixed_point" : "pending")));
    result.emplace("pending_node_count", Json(static_cast<std::int64_t>(pending_nodes_.size())));
    result.emplace("rounds", Json(static_cast<std::int64_t>(rounds_)));
    result.emplace("node_evaluations", Json(static_cast<std::int64_t>(node_evaluations_)));
    result.emplace("edge_evaluations", Json(static_cast<std::int64_t>(edge_evaluations_)));
    result.emplace("changed_scores", Json(static_cast<std::int64_t>(scores_.size())));
    result.emplace("changed_strengths", Json(static_cast<std::int64_t>(strengths_.size())));
    for (const auto* key : {"legacy_numerical_equivalence", "whole_graph_convergence_claimed",
                            "logical_implication_claimed", "cognitive_completion",
                            "persistent_state_mutated", "authority_granted"})
        result.emplace(key, Json(false));
    return Json(std::move(result));
}

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_kernel.py@7536139:134-209
EventVrsProposal advance_event_vrs(
    std::shared_ptr<const ValidatedEventVrsInputs> inputs,
    const std::vector<std::int64_t>& changed_nodes,
    const EventVrsProposal* previous, std::int64_t maximum_rounds) {
    if (!inputs) throw std::runtime_error("cold-bound event inputs required");
    const auto& source = inputs->require_validated_immutable();
    if (maximum_rounds < 0)
        throw std::runtime_error("event round budget must be a nonnegative integer");

    std::map<std::uint32_t, float> scores;
    std::map<std::uint32_t, float> strengths;
    std::set<std::uint32_t> pending;
    std::vector<std::uint32_t> seeds;
    std::uint64_t rounds = 0, node_evaluations = 0, edge_evaluations = 0;
    if (previous) {
        if (previous->inputs_.get() != inputs.get() || !changed_nodes.empty())
            throw std::runtime_error("pending event belongs to a different generation or event");
        scores = previous->scores_;
        strengths = previous->strengths_;
        pending.insert(previous->pending_nodes_.begin(), previous->pending_nodes_.end());
        seeds = previous->seed_nodes_;
        rounds = previous->rounds_;
        node_evaluations = previous->node_evaluations_;
        edge_evaluations = previous->edge_evaluations_;
    } else {
        for (const auto node : changed_nodes) {
            if (node < 0 || static_cast<std::uint64_t>(node) >= source.node_count())
                throw std::runtime_error("event seed is outside the node directory");
            pending.insert(static_cast<std::uint32_t>(node));
        }
        seeds.assign(pending.begin(), pending.end());
    }

    const auto& dependencies = source.dependencies();
    for (std::int64_t iteration = 0; iteration < maximum_rounds && !pending.empty(); ++iteration) {
        const auto score = [&](std::uint32_t node) -> double {
            const auto found = scores.find(node);
            return found == scores.end() ? static_cast<double>(source.score(node))
                                         : static_cast<double>(found->second);
        };
        const auto strength = [&](std::uint32_t edge) -> double {
            const auto found = strengths.find(edge);
            return found == strengths.end() ? static_cast<double>(source.strength(edge))
                                            : static_cast<double>(found->second);
        };
        std::map<std::uint32_t, float> next_scores;
        std::set<std::uint32_t> incident;
        std::map<std::uint32_t, std::vector<std::uint32_t>> outgoing;
        for (const auto node : pending) {
            std::vector<std::uint32_t> incoming;
            dependencies.visit_edges(node, EndpointDirection::incoming,
                [&](std::uint32_t edge) { incoming.push_back(edge); });
            dependencies.visit_edges(node, EndpointDirection::outgoing,
                [&](std::uint32_t edge) { outgoing[node].push_back(edge); });
            incident.insert(incoming.begin(), incoming.end());
            incident.insert(outgoing[node].begin(), outgoing[node].end());
            PythonFsum degree_sum, signal_sum;
            for (const auto edge : incoming) {
                const auto value = strength(edge);
                degree_sum.add(std::fabs(value));
                const auto address = source.edge(edge);
                signal_sum.add((score(address.source) * static_cast<double>(address.sign)) * value);
            }
            const double degree = std::max(1.0, degree_sum.finish());
            const double signal = signal_sum.finish();
            const double candidate = std::tanh(static_cast<double>(source.direct(node)) +
                                               0.2 * signal / degree);
            next_scores[node] = f32(0.8 * score(node) + 0.2 * candidate);
        }
        std::map<std::uint32_t, float> next_strengths;
        for (const auto edge_id : incident) {
            const auto edge = source.edge(edge_id);
            const auto score_after = [&](std::uint32_t node) {
                const auto found = next_scores.find(node);
                return found == next_scores.end() ? score(node) : static_cast<double>(found->second);
            };
            const auto left = score_after(edge.source);
            const auto right = score_after(edge.target);
            const bool stable =
                1.0 - 0.5 * std::fabs((left * static_cast<double>(edge.sign)) - right) >= 0.75 &&
                std::fabs(left) + std::fabs(right) >= 0.1 &&
                !(source.unresolved(edge.source) || source.unresolved(edge.target));
            const double factor = stable ? 1.01 : 0.995;
            const double base = static_cast<double>(edge.vrs_strength);
            next_strengths[edge_id] = f32(std::max(base * 0.25,
                                            std::min(strength(edge_id) * factor, base * 4.0)));
        }
        std::set<std::uint32_t> next_pending;
        for (const auto& [node, value] : next_scores) {
            if (!same_f32(value, static_cast<float>(score(node)))) {
                next_pending.insert(node);
                for (const auto edge : outgoing[node])
                    next_pending.insert(source.edge(edge).target);
            }
            if (same_f32(value, source.score(node))) scores.erase(node);
            else scores[node] = value;
        }
        for (const auto& [edge, value] : next_strengths) {
            if (!same_f32(value, static_cast<float>(strength(edge))))
                next_pending.insert(source.edge(edge).target);
            if (same_f32(value, source.strength(edge))) strengths.erase(edge);
            else strengths[edge] = value;
        }
        node_evaluations += pending.size();
        edge_evaluations += incident.size();
        pending = std::move(next_pending);
        ++rounds;
    }
    return EventVrsProposal(std::move(inputs), std::move(scores), std::move(strengths),
                            std::vector<std::uint32_t>(pending.begin(), pending.end()), rounds,
                            node_evaluations, edge_evaluations, std::move(seeds));
}

}  // namespace swegca::vrs
