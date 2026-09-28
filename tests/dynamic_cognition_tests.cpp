#include "world/dynamic_cognition.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <future>
#include <iostream>
#include <limits>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace swegca::world;

void check(const bool condition, const char* expression, const int line) {
    if (!condition) {
        std::cerr << "CHECK failed at line " << line << ": " << expression
                  << '\n';
        std::exit(1);
    }
}

#define CHECK(expression) check(static_cast<bool>(expression), #expression, __LINE__)

template <typename Operation>
void rejects(Operation&& operation, const std::string_view message) {
    try {
        std::forward<Operation>(operation)();
    } catch (const std::exception& error) {
        CHECK(std::string_view(error.what()).find(message) !=
              std::string_view::npos);
        return;
    }
    CHECK(false);
}

Tensor zeros(const std::vector<std::uint64_t>& shape) {
    std::size_t count = 1;
    for (const auto dimension : shape) {
        count *= static_cast<std::size_t>(dimension);
    }
    return Tensor(TensorDType::float32, shape,
                  std::vector<double>(count, 0.0));
}

std::shared_ptr<WorldState> world() {
    return std::make_shared<WorldState>(
        zeros({2, 32, 4}),
        BooleanMask({2, 32}, std::vector<std::uint8_t>(64, 0)),
        BooleanMask({2, 32}, std::vector<std::uint8_t>(64, 0)), "test");
}

std::shared_ptr<CognitiveState> cognitive() {
    return std::make_shared<CognitiveState>(
        zeros({2, 20, 4}), zeros({2, 6, 4}), zeros({2, 6, 4}),
        StructuredWorldGraph(
            {WorldEntity("door", "object",
                         {{"nested", JsonValue::Object{{"state", "closed"}}}},
                         {}, {"frame:before"})},
            {}));
}

template <typename State>
SynapseProposal proposal(const State& state, std::string source,
                         const bool decisive,
                         std::vector<std::vector<std::string>> evidence = {}) {
    std::uint64_t batch = 0;
    std::uint64_t slots = 0;
    std::uint64_t dimension = 0;
    TensorDType dtype = TensorDType::float32;
    std::string device;
    if constexpr (std::is_same_v<State, WorldState>) {
        const auto shape = state.semantic_slots().shape();
        batch = shape[0];
        slots = shape[1];
        dimension = shape[2];
        dtype = state.semantic_slots().dtype();
        device = state.semantic_slots().device();
    } else {
        const auto shape = state.semantic_slots().shape();
        batch = shape[0];
        slots = shape[1] + state.executive_slots().shape()[1] +
                state.scratch_slots().shape()[1];
        dimension = shape[2];
        dtype = state.semantic_slots().dtype();
        device = state.semantic_slots().device();
    }
    std::vector<double> delta(
        static_cast<std::size_t>(batch * slots * dimension), 0.0);
    std::vector<std::uint8_t> mask(
        static_cast<std::size_t>(batch * slots), 0);
    for (std::uint64_t row = 0; row < batch; ++row) {
        mask[static_cast<std::size_t>(row * slots + 30)] = 1;
        delta[static_cast<std::size_t>((row * slots + 30) * dimension)] =
            1.0;
    }
    const double confidence = decisive ? 1.0 : 0.5;
    const double uncertainty = decisive ? 0.0 : 0.5;
    return {std::move(source),
            Tensor(dtype, {batch, slots, dimension}, std::move(delta), device),
            std::vector<double>(static_cast<std::size_t>(batch), confidence),
            std::vector<double>(static_cast<std::size_t>(batch), 0.0),
            std::vector<double>(static_cast<std::size_t>(batch), uncertainty),
            BooleanMask({batch, slots}, std::move(mask)), std::move(evidence),
            "hypothesis"};
}

void test_primary_fast_path_and_preview_only() {
    const auto state = world();
    std::vector<std::string> calls;
    WorldCognitionCores cores;
    for (const auto& name : {"fast", "shape", "motion"}) {
        cores.emplace(name, [&, name](WorldState& snapshot,
                                      const OpaqueCognitionRequest&) {
            calls.emplace_back(name);
            return proposal(snapshot, name, true);
        });
    }
    const auto result = run_dynamic_cognition(
        state, OpaqueCognitionRequest{}, "identity", cores,
        {{"identity", {"fast", "shape", "motion"}}});
    CHECK(calls == std::vector<std::string>{"fast"});
    CHECK(result.proposals.size() == 1);
    CHECK(result.trace.executed_cores == std::vector<std::string>{"fast"});
    CHECK(!result.trace.fanout_used);
    CHECK(!result.trace.manager_retained);
    CHECK(!result.trace.worker_state_retained);
    CHECK(result.trace.primary_weights == std::vector<double>({1.0, 1.0}));
    CHECK(!result.arbitration.committed());
    CHECK(result.arbitration.world_state().get() == state.get());
}

void test_parallel_fanout_route_order_and_join() {
    const auto state = world();
    std::atomic<int> arrivals{0};
    std::promise<void> both_arrived;
    const auto release = both_arrived.get_future().share();
    std::mutex names_mutex;
    std::vector<std::string> called;
    WorldCognitionCores cores{
        {"fast", [&](WorldState& snapshot, const OpaqueCognitionRequest&) {
             return proposal(snapshot, "fast", false);
         }}};
    for (const auto& name : {"shape", "motion"}) {
        cores.emplace(name, [&, name](WorldState& snapshot,
                                      const OpaqueCognitionRequest&) {
            if (arrivals.fetch_add(1) + 1 == 2) {
                both_arrived.set_value();
            }
            if (release.wait_for(std::chrono::seconds(2)) !=
                std::future_status::ready) {
                throw std::runtime_error("workers did not run in parallel");
            }
            {
                std::lock_guard lock(names_mutex);
                called.emplace_back(name);
            }
            return proposal(snapshot, name, true);
        });
    }
    cores.emplace("audio", [](WorldState& snapshot,
                               const OpaqueCognitionRequest&) {
        return proposal(snapshot, "audio", true);
    });
    const auto result = run_dynamic_cognition(
        state, OpaqueCognitionRequest{}, "identity", cores,
        {{"identity", {"fast", "shape", "motion"}},
         {"speech", {"fast", "audio"}}});
    CHECK(arrivals == 2);
    CHECK(called.size() == 2);
    CHECK(result.trace.fanout_used);
    CHECK(result.trace.executed_cores ==
          std::vector<std::string>({"fast", "shape", "motion"}));
    CHECK(result.proposals[0].source == "fast");
    CHECK(result.proposals[1].source == "shape");
    CHECK(result.proposals[2].source == "motion");
    CHECK(!result.arbitration.committed());
}

void test_snapshot_isolation_and_deep_cognitive_copy() {
    const auto state = world();
    bool worker_saw_clean = false;
    WorldCognitionCores cores{
        {"primary", [](WorldState& snapshot, const OpaqueCognitionRequest&) {
             const_cast<double*>(snapshot.semantic_slots().values().data())[0] = 7.0;
             return proposal(snapshot, "primary", false);
         }},
        {"worker", [&](WorldState& snapshot, const OpaqueCognitionRequest&) {
             worker_saw_clean = snapshot.semantic_slots().values()[0] == 0.0;
             return proposal(snapshot, "worker", true);
         }}};
    static_cast<void>(run_dynamic_cognition(
        state, OpaqueCognitionRequest{}, "identity", cores,
        {{"identity", {"primary", "worker"}}}));
    CHECK(worker_saw_clean);
    CHECK(state->semantic_slots().values()[0] == 0.0);

    const auto cognition = cognitive();
    CognitiveStateCognitionCores cognitive_cores{
        {"metadata-worker",
         [](CognitiveState& snapshot, const OpaqueCognitionRequest&) {
             auto& entity = const_cast<WorldEntity&>(
                 snapshot.structured_world_graph().entities()[0]);
             auto& nested = const_cast<JsonValue::Object&>(
                 entity.properties.at("nested").as_object());
             nested["state"] = "open";
             return proposal(snapshot, "metadata-worker", true);
         }}};
    static_cast<void>(run_dynamic_cognition(
        cognition, OpaqueCognitionRequest{}, "metadata", cognitive_cores,
        {{"metadata", {"metadata-worker"}}}));
    CHECK(cognition->structured_world_graph()
              .entities()[0]
              .properties.at("nested")
              .at("state")
              .as_string() == "closed");
}

void test_source_identity_request_and_provenance() {
    const auto state = world();
    const std::string address = "experience://failed-identity-view";
    const OpaqueCognitionRequest request =
        std::map<std::string, std::string>{{"actor_identity", address}};
    WorldCognitionCores cores{
        {"experience-fast",
         [&](WorldState& snapshot, const OpaqueCognitionRequest& opaque) {
             const auto& index =
                 std::any_cast<const std::map<std::string, std::string>&>(opaque);
             CHECK(index.at("actor_identity") == address);
             return proposal(snapshot, "experience-fast", true,
                             {{address}, {address}});
         }}};
    const auto result = run_dynamic_cognition(
        state, request, "identity", cores,
        {{"identity", {"experience-fast"}}});
    CHECK(result.proposals[0].evidence_addresses ==
          std::vector<std::vector<std::string>>({{address}, {address}}));

    WorldCognitionCores wrong{
        {"fast", [](WorldState& snapshot, const OpaqueCognitionRequest&) {
             return proposal(snapshot, "other", true);
         }}};
    rejects(
        [&] {
            static_cast<void>(run_dynamic_cognition(
                state, OpaqueCognitionRequest{}, "identity", wrong,
                {{"identity", {"fast"}}}));
        },
        "source identity");
}

void test_main_mutation_detection() {
    const auto mutable_state = world();
    std::shared_ptr<const WorldState> state = mutable_state;
    WorldCognitionCores cores{
        {"malicious", [mutable_state](WorldState& snapshot,
                                       const OpaqueCognitionRequest&) {
             const_cast<double*>(
                 mutable_state->semantic_slots().values().data())[0] = 99.0;
             return proposal(snapshot, "malicious", true);
         }}};
    rejects(
        [&] {
            static_cast<void>(run_dynamic_cognition(
                state, OpaqueCognitionRequest{}, "identity", cores,
                {{"identity", {"malicious"}}}));
        },
        "mutated main persistent state");
}

void test_input_validation() {
    const auto state = world();
    const WorldCognitionCores core{
        {"fast", [](WorldState& snapshot, const OpaqueCognitionRequest&) {
             return proposal(snapshot, "fast", true);
         }}};
    const CognitionRoutes valid{{"identity", {"fast"}}};
    rejects(
        [&] {
            static_cast<void>(run_dynamic_cognition(
                state, OpaqueCognitionRequest{}, "", core, valid));
        },
        "operation_type");
    rejects(
        [&] {
            static_cast<void>(run_dynamic_cognition(
                state, OpaqueCognitionRequest{}, "identity", {}, valid));
        },
        "resident cognition core");
    for (const auto weight :
         {-0.01, 1.01, std::numeric_limits<double>::quiet_NaN()}) {
        rejects(
            [&] {
                static_cast<void>(run_dynamic_cognition(
                    state, OpaqueCognitionRequest{}, "identity", core, valid,
                    weight));
            },
            "decisive_weight");
    }
    rejects(
        [&] {
            static_cast<void>(run_dynamic_cognition(
                state, OpaqueCognitionRequest{}, "missing", core, valid));
        },
        "missing");
    rejects(
        [&] {
            static_cast<void>(run_dynamic_cognition(
                state, OpaqueCognitionRequest{}, "identity", core,
                {{"identity", {}}}));
        },
        "unique core names");
    rejects(
        [&] {
            static_cast<void>(run_dynamic_cognition(
                state, OpaqueCognitionRequest{}, "identity", core,
                {{"identity", {"fast", "fast"}}}));
        },
        "unique core names");
    rejects(
        [&] {
            static_cast<void>(run_dynamic_cognition(
                state, OpaqueCognitionRequest{}, "identity", core,
                {{"identity", {"unknown"}}}));
        },
        "unknown");
}

}  // namespace

int main() {
    test_primary_fast_path_and_preview_only();
    test_parallel_fanout_route_order_and_join();
    test_snapshot_isolation_and_deep_cognitive_copy();
    test_source_identity_request_and_provenance();
    test_main_mutation_detection();
    test_input_validation();
    std::cout << "dynamic cognition tests passed\n";
}
