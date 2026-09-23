#pragma once

#include "swegca_vrs/allocation.hpp"
#include "swegca_vrs/main_owner.hpp"
#include "swegca_vrs/proposal.hpp"

#include <chrono>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Request-local producer stage of the author's dynamic cognition path. A
// producer output is still unbound: Main must Bind every proposal and run the
// complete no-commit preview before constructing DynamicCognitionTrace. In
// particular, elapsed_ns is measured after that preview, not here.
// Rule: mosaic_synapse_arbiter.py@5901a5a:365-455; board §3H.
namespace swegca::vrs {

// An immutable registered definition may outlive requests; its callback must
// create and destroy any producer instance and identity state in the call.
// Neither the callback nor definition may retain the request, snapshot, or a
// mutable Main pointer. The state argument is a distinct immutable snapshot
// lease for this call, detached from later Main head changes.
using ProducerCallback = SynapseProposal (*)(
    StateSnapshot state, const void* request,
    const void* immutable_definition, const AllocationContext& request_memory);

struct ProducerDefinition final {
    std::string_view name;
    ProducerCallback run = nullptr;
    const void* immutable_definition = nullptr;
};

struct CognitionRoute final {
    std::string_view operation_type;
    std::span<const std::string_view> producers;
};

// VRS supplies an executor with its own CPU limit. On success, `run_joined`
// invokes each index exactly once. On failure, including submission failure,
// it joins every submitted worker before throwing. It never retains task/context
// or their borrowed request/snapshot beyond this call. The runner catches task
// exceptions inside the noexcept callback, then reports the first one in
// route order. `fanout_used` means extra route members ran, independent of
// physical simultaneous worker count.
class JoinedProducerExecutor {
public:
    JoinedProducerExecutor() = default;
    JoinedProducerExecutor(const JoinedProducerExecutor&) = delete;
    JoinedProducerExecutor& operator=(const JoinedProducerExecutor&) = delete;
    virtual ~JoinedProducerExecutor() = default;

    virtual void run_joined(std::size_t count,
                            void (*task)(void*, std::size_t) noexcept,
                            void* context) = 0;
};

// Intermediate output only. The absence of a completed trace and preview is
// deliberate: Main has not yet bound the proposals to admitted evidence.
class ProducerRunIntermediate final {
public:
    using Proposals = std::vector<SynapseProposal, AllocationAdapter<SynapseProposal>>;

    ProducerRunIntermediate(ProducerRunIntermediate&&) noexcept = default;
    ProducerRunIntermediate& operator=(ProducerRunIntermediate&&) = delete;
    ProducerRunIntermediate(const ProducerRunIntermediate&) = delete;
    ProducerRunIntermediate& operator=(const ProducerRunIntermediate&) = delete;
    ~ProducerRunIntermediate() = default;

    // SWEGCA: src/swegca/mosaic_synapse_arbiter.py@5901a5a:108-115
    [[nodiscard]] std::string_view operation_type() const noexcept { return operation_type_; }
    // SWEGCA: src/swegca/mosaic_synapse_arbiter.py@5901a5a:118-122
    [[nodiscard]] const Proposals& proposals() const noexcept { return proposals_; }
    // SWEGCA: src/swegca/mosaic_synapse_arbiter.py@5901a5a:108-115
    [[nodiscard]] bool fanout_used() const noexcept { return fanout_used_; }
    // SWEGCA: src/swegca/mosaic_synapse_arbiter.py@5901a5a:113-121
    [[nodiscard]] double primary_weight() const noexcept { return primary_weight_; }
    // The author's primary_weights is a typed tensor; batch one stores one
    // scalar, and the type must survive until Main creates the final trace.
    // SWEGCA: src/swegca/mosaic_synapse_arbiter.py@5901a5a:113-121
    [[nodiscard]] ScalarType primary_weight_type() const noexcept {
        return primary_weight_type_;
    }
    // The eventual full cognition path finalizes elapsed time only after
    // Bind and multi-proposal no-commit preview.
    // SWEGCA: src/swegca/mosaic_synapse_arbiter.py@5901a5a:398-399
    [[nodiscard]] std::chrono::steady_clock::time_point started_at() const noexcept {
        return started_at_;
    }

private:
    friend ProducerRunIntermediate run_request_producers(
        const MainOwner&, const AllocationContext&, const void*,
        std::string_view, std::span<const ProducerDefinition>,
        std::span<const CognitionRoute>, double, JoinedProducerExecutor&);
    using Text = std::basic_string<char, std::char_traits<char>, AllocationAdapter<char>>;

    // SWEGCA: src/swegca/mosaic_synapse_arbiter.py@5901a5a:442-455
    ProducerRunIntermediate(Text operation_type, Proposals proposals,
                            bool fanout_used, double primary_weight,
                            ScalarType primary_weight_type,
                            std::chrono::steady_clock::time_point started_at)
        : operation_type_(std::move(operation_type)), proposals_(std::move(proposals)),
          fanout_used_(fanout_used), primary_weight_(primary_weight),
          primary_weight_type_(primary_weight_type), started_at_(started_at) {}

    Text operation_type_;
    Proposals proposals_;
    bool fanout_used_ = false;
    double primary_weight_ = 0;
    ScalarType primary_weight_type_ = ScalarType::float32;
    std::chrono::steady_clock::time_point started_at_;
};

// The operation and immutable registry/route definitions are borrowed only
// for this call. The opaque request is read-only for the same lifetime. Each
// producer gets its own copy of the captured immutable snapshot lease and no
// Main reference. The result has no decision or write capability.
// SWEGCA: src/swegca/mosaic_synapse_arbiter.py@5901a5a:365-455
[[nodiscard]] ProducerRunIntermediate run_request_producers(
    const MainOwner& main, const AllocationContext& request_memory,
    const void* request, std::string_view operation_type,
    std::span<const ProducerDefinition> registry,
    std::span<const CognitionRoute> routes, double decisive_weight,
    JoinedProducerExecutor& executor);

}  // namespace swegca::vrs
