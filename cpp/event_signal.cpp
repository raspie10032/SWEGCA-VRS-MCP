#include "event_signal.hpp"

#include "event_signal_strength.hpp"
#include "python_fsum.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <utility>

namespace swegca::vrs {
namespace {

struct IncomingTerm {
    std::uint32_t source;
    std::int8_t sign;
    double weight;
    double baseline;
};

struct NodeTopology {
    std::vector<IncomingTerm> incoming;
    double denominator;
    std::vector<std::uint32_t> targets;
    double direct;
    double original;
};

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

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_signal.py@7536139:181-185
EventSignalProposal::EventSignalProposal(
    std::shared_ptr<const ValidatedEventVrsInputs> inputs,
    std::map<std::uint32_t, float> scores,
    std::map<std::uint32_t, float> strengths,
    std::vector<std::uint32_t> pending_nodes,
    std::uint64_t rounds, std::uint64_t node_evaluations,
    std::uint64_t edge_evaluations,
    std::vector<std::uint32_t> seed_nodes,
    std::shared_ptr<const VRSStateUpdateReceipt> strength_receipt,
    std::string strength_storage_dtype)
    // SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_signal.py@7536139:181-185
    : EventVrsProposal(std::move(inputs), std::move(scores), std::move(strengths),
                       std::move(pending_nodes), rounds, node_evaluations,
                       edge_evaluations, std::move(seed_nodes)),
      strength_receipt_(std::move(strength_receipt)),
      strength_storage_dtype_(std::move(strength_storage_dtype)) {}

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_signal.py@7536139:26-35
Json EventSignalProposal::receipt() const {
    auto result = EventVrsProposal::receipt().object();
    result.insert_or_assign("version", Json(std::string("vrs-re-evidence-event-signal-f32-v2-experimental")));
    result.insert_or_assign("status", Json(std::string(pending_nodes_.empty() ? "signal_fixed_point" : "pending")));
    result.insert_or_assign("strength_updates_reapplied_during_iterations", Json(std::int64_t{0}));
    result.insert_or_assign("numerical_compatibility_controls_strength", Json(false));
    result.insert_or_assign("strength_storage_dtype", Json(strength_storage_dtype_));
    result.insert_or_assign("storage_binding_verified", Json(false));
    result.insert_or_assign("input_strength_proposal_count",
                            Json(static_cast<std::int64_t>(strength_receipt_ ?
                                strength_receipt_->updates.size() : 0)));
    return Json(std::move(result));
}

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_signal.py@7536139:100-185
EventSignalProposal settle_event_signal(
    std::shared_ptr<const ValidatedEventVrsInputs> inputs,
    const std::vector<std::int64_t>& changed_nodes,
    std::shared_ptr<const VRSStateUpdateReceipt> strength_updates,
    std::string_view connection_namespace,
    const EventSignalProposal* previous,
    std::int64_t maximum_rounds,
    std::optional<std::string> strength_storage_dtype) {
    if (!inputs) throw std::runtime_error("cold-bound event inputs required");
    const auto& source = inputs->require_validated_immutable();
    if (maximum_rounds < 0)
        throw std::runtime_error("signal round budget must be a nonnegative integer");
    const std::string dtype = strength_storage_dtype ? *strength_storage_dtype :
        previous ? previous->strength_storage_dtype_ : "<f4";
    if (dtype != "<f4" && dtype != "<f2")
        throw std::runtime_error("unsupported event strength storage dtype");

    std::map<std::uint32_t, float> scores, strengths;
    std::set<std::uint32_t> pending;
    std::vector<std::uint32_t> seeds;
    std::uint64_t rounds = 0, nodes = 0, edges = 0;
    std::shared_ptr<const VRSStateUpdateReceipt> receipt;
    if (previous) {
        if (previous->inputs_.get() != inputs.get() || !changed_nodes.empty() ||
            strength_updates || dtype != previous->strength_storage_dtype_)
            throw std::runtime_error("signal resume generation or event changed");
        scores = previous->scores_;
        strengths = previous->strengths_;
        pending.insert(previous->pending_nodes_.begin(), previous->pending_nodes_.end());
        seeds = previous->seed_nodes_;
        rounds = previous->rounds_;
        nodes = previous->node_evaluations_;
        edges = previous->edge_evaluations_;
        receipt = previous->strength_receipt_;
    } else {
        for (const auto node : changed_nodes) {
            if (node < 0 || static_cast<std::uint64_t>(node) >= source.node_count())
                throw std::runtime_error("signal seed outside node directory");
            pending.insert(static_cast<std::uint32_t>(node));
        }
        auto bound = bind_event_strength_updates(*inputs, strength_updates.get(),
                                                 connection_namespace, dtype);
        strengths = std::move(bound.strengths);
        pending.insert(bound.seed_nodes.begin(), bound.seed_nodes.end());
        seeds.assign(pending.begin(), pending.end());
        receipt = std::move(strength_updates);
    }

    // Source cache lasts only for this invocation. Its terms and denominator
    // use fixed proposed strengths; changed scores remain round-dependent.
    // SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_signal.py@7536139:141-149
    std::map<std::uint32_t, NodeTopology> topology;
    std::map<std::uint32_t, double> initial_scores;
    const auto initial = [&](std::uint32_t node) -> double {
        const auto found = initial_scores.find(node);
        if (found != initial_scores.end()) return found->second;
        const auto value = static_cast<double>(source.score(node));
        initial_scores[node] = value;
        return value;
    };
    const auto& index = source.dependencies();
    for (std::int64_t iteration = 0; iteration < maximum_rounds && !pending.empty(); ++iteration) {
        std::map<std::uint32_t, float> next_scores;
        std::map<std::uint32_t, std::vector<std::uint32_t>> outgoing;
        std::uint64_t edge_reads = 0;
        for (const auto node : pending) {
            auto found = topology.find(node);
            if (found == topology.end()) {
                NodeTopology candidate;
                PythonFsum degree;
                index.visit_edges(node, EndpointDirection::incoming,
                    [&](std::uint32_t edge) {
                        const auto address = source.edge(edge);
                        const auto proposed = strengths.find(edge);
                        const double weight = proposed == strengths.end() ?
                            static_cast<double>(source.strength(edge)) :
                            static_cast<double>(proposed->second);
                        candidate.incoming.push_back(
                            IncomingTerm{address.source, address.sign, weight, initial(address.source)});
                        degree.add(std::fabs(weight));
                    });
                index.visit_edges(node, EndpointDirection::outgoing,
                    [&](std::uint32_t edge) { candidate.targets.push_back(source.edge(edge).target); });
                candidate.denominator = std::max(1.0, degree.finish());
                candidate.direct = static_cast<double>(source.direct(node));
                candidate.original = initial(node);
                found = topology.emplace(node, std::move(candidate)).first;
            }
            const auto& term = found->second;
            outgoing[node] = term.targets;
            PythonFsum signal;
            for (const auto& incoming : term.incoming) {
                const auto current = scores.find(incoming.source);
                const double score = current == scores.end() ? incoming.baseline :
                    static_cast<double>(current->second);
                signal.add((score * static_cast<double>(incoming.sign)) * incoming.weight);
            }
            const auto current = scores.find(node);
            const double old = current == scores.end() ? term.original :
                static_cast<double>(current->second);
            next_scores[node] = f32(0.8 * old + 0.2 *
                std::tanh(term.direct + 0.2 * signal.finish() / term.denominator));
            edge_reads += term.incoming.size();
        }
        std::set<std::uint32_t> following;
        for (const auto& [node, value] : next_scores) {
            const auto existing = scores.find(node);
            const auto baseline = initial_scores.at(node);
            const float previous_score = existing == scores.end() ?
                static_cast<float>(baseline) : existing->second;
            if (!same_f32(value, previous_score)) {
                following.insert(node);
                following.insert(outgoing.at(node).begin(), outgoing.at(node).end());
            }
            if (same_f32(value, static_cast<float>(baseline))) scores.erase(node);
            else scores[node] = value;
        }
        ++rounds;
        nodes += pending.size();
        edges += edge_reads;
        pending = std::move(following);
    }
    return EventSignalProposal(
        std::move(inputs), std::move(scores), std::move(strengths),
        std::vector<std::uint32_t>(pending.begin(), pending.end()), rounds,
        nodes, edges, std::move(seeds), std::move(receipt), dtype);
}

}  // namespace swegca::vrs
