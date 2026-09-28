#include "world/dynamic_cognition.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <future>
#include <set>
#include <stdexcept>
#include <utility>

namespace swegca::world {
namespace {

[[nodiscard]] WorldState deep_snapshot(const WorldState& world) {
    return WorldState(
        world.semantic_slots().clone(), world.active_mask().clone(),
        world.dirty_mask().clone(), std::string(world.source()),
        std::vector<SurfaceResidualRef>(world.surface_refs().begin(),
                                        world.surface_refs().end()));
}

[[nodiscard]] CognitiveState deep_snapshot(const CognitiveState& world) {
    return world.clone();
}

[[nodiscard]] bool state_equal(const WorldState& left,
                               const WorldState& right) noexcept {
    return left.semantic_slots().exact_equal(right.semantic_slots()) &&
           left.active_mask() == right.active_mask() &&
           left.dirty_mask() == right.dirty_mask();
}

[[nodiscard]] bool state_equal(const CognitiveState& left,
                               const CognitiveState& right) noexcept {
    return left.semantic_slots().exact_equal(right.semantic_slots()) &&
           left.executive_slots().exact_equal(right.executive_slots()) &&
           left.scratch_slots().exact_equal(right.scratch_slots());
}

template <typename State, typename Cores>
void validate_inputs(const std::shared_ptr<const State>& world,
                     const std::string& operation_type,
                     const Cores& resident_cores,
                     const CognitionRoutes& routes,
                     const double decisive_weight) {
    if (!world) {
        throw std::invalid_argument("World State must not be null");
    }
    if (operation_type.empty()) {
        throw std::invalid_argument("operation_type must be nonempty");
    }
    if (resident_cores.empty()) {
        throw std::invalid_argument(
            "at least one resident cognition core is required");
    }
    if (!std::isfinite(decisive_weight) || decisive_weight < 0.0 ||
        decisive_weight > 1.0) {
        throw std::invalid_argument("decisive_weight must be within [0, 1]");
    }
    const auto route_entry = routes.find(operation_type);
    if (route_entry == routes.end()) {
        throw std::out_of_range(operation_type);
    }
    const auto& route = route_entry->second;
    const std::set<std::string, std::less<>> unique(route.begin(), route.end());
    if (route.empty() || unique.size() != route.size()) {
        throw std::invalid_argument(
            "a cognition route must contain unique core names");
    }
    for (const auto& core_name : unique) {
        if (!resident_cores.contains(core_name)) {
            throw std::out_of_range(core_name);
        }
    }
}

template <typename State, typename Cores>
[[nodiscard]] DynamicCognitionResult<State> run(
    std::shared_ptr<const State> world,
    const OpaqueCognitionRequest& request,
    std::string operation_type,
    const Cores& resident_cores,
    const CognitionRoutes& routes,
    const double decisive_weight,
    std::shared_ptr<const SingleWorldArbiter> arbiter) {
    validate_inputs(world, operation_type, resident_cores, routes,
                    decisive_weight);
    const auto started = std::chrono::steady_clock::now();
    const State main_before = deep_snapshot(*world);
    const auto& route = routes.at(operation_type);

    const auto execute = [&](const std::string& core_name) {
        State snapshot = deep_snapshot(*world);
        SynapseProposal proposal = resident_cores.at(core_name)(snapshot, request);
        proposal.validate(*world);
        if (proposal.source != core_name) {
            throw std::invalid_argument(
                "cognition core source identity changed");
        }
        return proposal;
    };

    SynapseProposal primary = execute(route.front());
    std::vector<double> primary_weights(primary.confidence.size(), 0.0);
    for (std::size_t batch = 0; batch < primary_weights.size(); ++batch) {
        primary_weights[batch] =
            primary.confidence[batch] *
            (1.0 - primary.contradiction[batch]) *
            (1.0 - primary.uncertainty[batch]);
    }
    const bool primary_decisive = std::all_of(
        primary_weights.begin(), primary_weights.end(),
        [decisive_weight](const double value) {
            return value >= decisive_weight;
        });
    const bool fanout_used = route.size() > 1 && !primary_decisive;

    std::vector<SynapseProposal> proposals;
    proposals.reserve(fanout_used ? route.size() : 1);
    proposals.push_back(std::move(primary));
    if (fanout_used) {
        std::vector<std::future<SynapseProposal>> workers;
        workers.reserve(route.size() - 1);
        for (auto core = std::next(route.begin()); core != route.end(); ++core) {
            workers.push_back(std::async(std::launch::async, execute, *core));
        }
        for (auto& worker : workers) {
            proposals.push_back(worker.get());
        }
    }

    if (!state_equal(main_before, *world)) {
        throw std::runtime_error(
            "dynamic cognition mutated main persistent state");
    }

    const auto selected_arbiter =
        arbiter ? std::move(arbiter)
                : std::make_shared<const SingleWorldArbiter>();
    auto arbitration = (*selected_arbiter)(world, proposals, false);

    std::vector<std::string> executed_cores;
    executed_cores.reserve(proposals.size());
    for (const auto& proposal : proposals) {
        executed_cores.push_back(proposal.source);
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - started);
    return DynamicCognitionResult<State>{
        std::move(arbitration), std::move(proposals),
        DynamicCognitionTrace{std::move(operation_type),
                              std::move(executed_cores), fanout_used,
                              static_cast<std::uint64_t>(elapsed.count()),
                              std::move(primary_weights), false, false}};
}

}  // namespace

DynamicCognitionResult<WorldState> run_dynamic_cognition(
    std::shared_ptr<const WorldState> world,
    const OpaqueCognitionRequest& request,
    std::string operation_type,
    const WorldCognitionCores& resident_cores,
    const CognitionRoutes& routes,
    const double decisive_weight,
    std::shared_ptr<const SingleWorldArbiter> arbiter) {
    return run(std::move(world), request, std::move(operation_type),
               resident_cores, routes, decisive_weight, std::move(arbiter));
}

DynamicCognitionResult<CognitiveState> run_dynamic_cognition(
    std::shared_ptr<const CognitiveState> world,
    const OpaqueCognitionRequest& request,
    std::string operation_type,
    const CognitiveStateCognitionCores& resident_cores,
    const CognitionRoutes& routes,
    const double decisive_weight,
    std::shared_ptr<const SingleWorldArbiter> arbiter) {
    return run(std::move(world), request, std::move(operation_type),
               resident_cores, routes, decisive_weight, std::move(arbiter));
}

}  // namespace swegca::world
