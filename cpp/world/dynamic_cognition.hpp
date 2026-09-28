#pragma once

#include "world/synapse_arbiter.hpp"

#include <any>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace swegca::world {

using OpaqueCognitionRequest = std::any;

template <typename State>
using CognitionCore =
    std::function<SynapseProposal(State&, const OpaqueCognitionRequest&)>;

using CognitionRoutes = std::map<std::string, std::vector<std::string>, std::less<>>;

struct DynamicCognitionTrace final {
    std::string operation_type;
    std::vector<std::string> executed_cores;
    bool fanout_used{false};
    std::uint64_t elapsed_ns{0};
    std::vector<double> primary_weights;
    bool manager_retained{false};
    bool worker_state_retained{false};

    friend bool operator==(const DynamicCognitionTrace&,
                           const DynamicCognitionTrace&) = default;
};

template <typename State>
struct DynamicCognitionResult final {
    ArbitrationResult<State> arbitration;
    std::vector<SynapseProposal> proposals;
    DynamicCognitionTrace trace;
};

using WorldCognitionCores =
    std::map<std::string, CognitionCore<WorldState>, std::less<>>;
using CognitiveStateCognitionCores =
    std::map<std::string, CognitionCore<CognitiveState>, std::less<>>;

[[nodiscard]] DynamicCognitionResult<WorldState> run_dynamic_cognition(
    std::shared_ptr<const WorldState> world,
    const OpaqueCognitionRequest& request,
    std::string operation_type,
    const WorldCognitionCores& resident_cores,
    const CognitionRoutes& routes,
    double decisive_weight = 0.75,
    std::shared_ptr<const SingleWorldArbiter> arbiter = {});

[[nodiscard]] DynamicCognitionResult<CognitiveState> run_dynamic_cognition(
    std::shared_ptr<const CognitiveState> world,
    const OpaqueCognitionRequest& request,
    std::string operation_type,
    const CognitiveStateCognitionCores& resident_cores,
    const CognitionRoutes& routes,
    double decisive_weight = 0.75,
    std::shared_ptr<const SingleWorldArbiter> arbiter = {});

}  // namespace swegca::world
