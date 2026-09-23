#include "swegca_vrs/cognition_runner.hpp"

#include "swegca_vrs/arbiter_kernel.hpp"

#include <exception>
#include <optional>
#include <stdexcept>
#include <utility>

namespace swegca::vrs {
namespace {

// Source Mapping keys are unique. A native registry must retain the same
// property before any route lookup or producer execution.
// SWEGCA: src/swegca/mosaic_synapse_arbiter.py@5901a5a:378-396
const ProducerDefinition* find_producer(
    std::span<const ProducerDefinition> registry, std::string_view name) {
    for (const auto& producer : registry) {
        if (producer.name == name) return &producer;
    }
    return nullptr;
}

// The constructor validates proposal shape, scores, finite deltas, and
// generation; this check additionally binds the returned source to the
// immutable registered route member as in the author's execute closure.
// SWEGCA: src/swegca/mosaic_synapse_arbiter.py@5901a5a:400-407
SynapseProposal execute_producer(const ProducerDefinition& producer,
                                 const StateSnapshot& snapshot,
                                 const void* request,
                                 const AllocationContext& memory) {
    auto proposal = producer.run(StateSnapshot(snapshot), request,
                                 producer.immutable_definition, memory);
    if (proposal.based_on() != snapshot.state().generation())
        throw std::invalid_argument("proposal_snapshot_generation_mismatch");
    if (proposal.source().value() != producer.name)
        throw std::invalid_argument("cognition_core_source_identity_changed");
    return proposal;
}

// Reuse the existing SWEGCA score operations, including storage-type
// rounding, rather than introduce another arithmetic rule for fanout.
// SWEGCA: src/swegca/mosaic_synapse_arbiter.py@5901a5a:410-417
kernel::ScoreValue primary_weight(const SynapseProposal& proposal) noexcept {
    const auto& confidence = proposal.confidence();
    const auto& contradiction = proposal.contradiction();
    const auto& uncertainty = proposal.uncertainty();
    const auto first = kernel::arbiter_score_multiply(
        {confidence.scalar_type, confidence.value},
        kernel::arbiter_score_complement(
            {contradiction.scalar_type, contradiction.value}));
    return kernel::arbiter_score_multiply(
        first, kernel::arbiter_score_complement(
                   {uncertainty.scalar_type, uncertainty.value}));
}

struct WorkerOutcome final {
    std::optional<SynapseProposal> proposal;
    std::exception_ptr failure;
};

struct WorkerContext final {
    const StateSnapshot& snapshot;
    const void* request;
    const AllocationContext& memory;
    std::span<const std::string_view> names;
    std::span<const ProducerDefinition> registry;
    std::span<WorkerOutcome> outcomes;
};

// Each slot has exactly one writer and is read only after run_joined returns.
// No exception crosses a worker boundary; every scheduled task can complete.
// SWEGCA: src/swegca/mosaic_synapse_arbiter.py@5901a5a:421-429
void run_worker(void* opaque, std::size_t index) noexcept {
    auto& context = *static_cast<WorkerContext*>(opaque);
    try {
        const auto* producer = find_producer(context.registry, context.names[index]);
        context.outcomes[index].proposal.emplace(execute_producer(
            *producer, context.snapshot, context.request, context.memory));
    } catch (...) {
        context.outcomes[index].failure = std::current_exception();
    }
}

}  // namespace

// SWEGCA: src/swegca/mosaic_synapse_arbiter.py@5901a5a:365-455
ProducerRunIntermediate run_request_producers(
    const MainOwner& main, const AllocationContext& request_memory,
    const void* request, std::string_view operation_type,
    std::span<const ProducerDefinition> registry,
    std::span<const CognitionRoute> routes, double decisive_weight,
    JoinedProducerExecutor& executor) {
    if (operation_type.empty())
        throw std::invalid_argument("operation_type_must_be_nonempty");
    if (registry.empty())
        throw std::invalid_argument("at_least_one_resident_cognition_core_required");
    if (!(decisive_weight >= 0.0 && decisive_weight <= 1.0))
        throw std::invalid_argument("decisive_weight_out_of_range");

    // A Python Mapping cannot contain duplicate names. A native span can;
    // fail closed before resolving the route so lookup remains unambiguous.
    for (std::size_t at = 0; at < registry.size(); ++at) {
        if (registry[at].name.empty() || registry[at].run == nullptr)
            throw std::invalid_argument("cognition_core_invalid");
        for (std::size_t prior = 0; prior < at; ++prior) {
            if (registry[prior].name == registry[at].name)
                throw std::invalid_argument("cognition_core_duplicate");
        }
    }

    const CognitionRoute* route = nullptr;
    for (const auto& candidate : routes) {
        if (candidate.operation_type == operation_type) {
            if (route != nullptr)
                throw std::invalid_argument("cognition_route_duplicate_operation");
            route = &candidate;
        }
    }
    if (route == nullptr) throw std::out_of_range(std::string(operation_type));
    if (route->producers.empty())
        throw std::invalid_argument("cognition_route_empty");
    for (std::size_t at = 0; at < route->producers.size(); ++at) {
        for (std::size_t prior = 0; prior < at; ++prior) {
            if (route->producers[prior] == route->producers[at])
                throw std::invalid_argument("cognition_route_duplicate_core");
        }
    }
    std::optional<std::string_view> unknown;
    for (const auto name : route->producers) {
        if (find_producer(registry, name) == nullptr &&
            (!unknown || name < *unknown))
            unknown = name;
    }
    if (unknown) throw std::out_of_range(std::string(*unknown));

    const auto started = std::chrono::steady_clock::now();
    const auto snapshot = main.snapshot();
    const auto before = snapshot.state().generation();
    const auto content_before = snapshot.content_digest();
    ProducerRunIntermediate::Proposals proposals{
        request_memory.allocator<SynapseProposal>()};
    proposals.reserve(route->producers.size());

    const auto* primary_definition = find_producer(registry, route->producers.front());
    proposals.push_back(execute_producer(*primary_definition, snapshot,
                                         request, request_memory));
    const auto weight = primary_weight(proposals.front());
    const auto threshold = kernel::arbiter_round_score(
        decisive_weight, weight.scalar_type);
    const auto additional = route->producers.subspan(1);
    const bool fanout_used = !additional.empty() && weight.value < threshold;

    if (fanout_used) {
        std::vector<WorkerOutcome, AllocationAdapter<WorkerOutcome>> outcomes{
            request_memory.allocator<WorkerOutcome>()};
        outcomes.resize(additional.size());
        WorkerContext context{snapshot, request, request_memory, additional,
                              registry, outcomes};
        std::exception_ptr executor_failure;
        try {
            executor.run_joined(additional.size(), &run_worker, &context);
        } catch (...) {
            // The executor contract guarantees all earlier submissions are
            // joined even when submission or execution itself failed.
            executor_failure = std::current_exception();
        }
        // A submission/executor error exits the author's with block after
        // joining already submitted tasks, before reading future results.
        if (executor_failure) std::rethrow_exception(executor_failure);
        // Once submission succeeds, the author reads futures in route order.
        // Preserve the first producer failure in that order after all join.
        for (auto& outcome : outcomes) {
            if (outcome.failure) std::rethrow_exception(outcome.failure);
        }
        for (auto& outcome : outcomes) {
            if (!outcome.proposal)
                throw std::logic_error("cognition_worker_not_executed");
            proposals.push_back(std::move(*outcome.proposal));
        }
    }

    // An immutable lease cannot be mutated by a producer. Also refuse a
    // changed Main head before this request's outputs proceed to Bind.
    // SWEGCA: src/swegca/mosaic_synapse_arbiter.py@5901a5a:431-435
    const auto after = main.snapshot();
    if (after.state().generation() != before ||
        after.content_digest() != content_before)
        throw std::runtime_error("dynamic_cognition_mutated_main_persistent_state");

    using Text = std::basic_string<char, std::char_traits<char>, AllocationAdapter<char>>;
    Text operation{request_memory.allocator<char>()};
    operation.assign(operation_type);
    return ProducerRunIntermediate(std::move(operation), std::move(proposals),
                                   fanout_used, weight.value, weight.scalar_type,
                                   started);
}

}  // namespace swegca::vrs
