#pragma once

#include "swegca_vrs/core_allocation.hpp"

#include "swegca_vrs/cognitive_state.hpp"
#include "swegca_vrs/arbiter_rules.hpp"
#include "swegca_architecture/allocation.hpp"
#include "swegca_vrs/proposal.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

// The Main-owned arbiter consumes only successful BoundProposal values. It
// computes one bounded, conflict-suppressed candidate over registered roles
// and an audit receipt. It has no state pointer, write token, or commit API.
// Rule: mosaic_synapse_arbiter.py@5901a5a:219-362; board §3D, §5, §10.5.
namespace swegca::vrs {

enum ArbitrationFailure : std::uint32_t {
    arbitration_stale_generation = 1u << 0,
    arbitration_stale_step = 1u << 1,
    arbitration_registry_mismatch = 1u << 2,
    arbitration_invalid_shape = 1u << 3,
    arbitration_kernel_refused = 1u << 4,
};

class ArbitrationResult final {
public:
    ArbitrationResult(ArbitrationResult&& other) noexcept;
    ArbitrationResult& operator=(ArbitrationResult&&) = delete;
    ArbitrationResult(const ArbitrationResult&) = delete;
    ArbitrationResult& operator=(const ArbitrationResult&) = delete;
    ~ArbitrationResult() = default;

    // SWEGCA: src/swegca/mosaic_synapse_arbiter.py@5901a5a:96-122
    [[nodiscard]] const StateGeneration& based_on() const;
    // SWEGCA: src/swegca/mosaic_synapse_arbiter.py@5901a5a:96-122
    [[nodiscard]] std::uint64_t validated_at_step() const;
    // SWEGCA: src/swegca/mosaic_synapse_arbiter.py@5901a5a:96-122
    [[nodiscard]] const RoleMask& changed_roles() const;
    // Role-order [role][feature] canonical bytes, one registered role per row.
    // Writer maps each row through that same registry and only writes roles
    // in changed_roles. Spare numeric slots are never targets.
    // SWEGCA: src/swegca/mosaic_synapse_arbiter.py@5901a5a:303-321
    [[nodiscard]] std::span<const std::byte> role_delta() const;
    // SWEGCA: src/swegca/mosaic_synapse_arbiter.py@5901a5a:96-122
    [[nodiscard]] ScalarType scalar_type() const;
    // SWEGCA: src/swegca/mosaic_synapse_arbiter.py@5901a5a:96-122
    [[nodiscard]] std::uint64_t width() const;
    // One bit per input proposal and one bit per registered role.
    // SWEGCA: src/swegca/mosaic_synapse_arbiter.py@5901a5a:96-122
    [[nodiscard]] std::span<const std::uint8_t> accepted() const;
    // SWEGCA: src/swegca/mosaic_synapse_arbiter.py@5901a5a:96-122
    [[nodiscard]] std::span<const std::uint8_t> conflicted_roles() const;
    // SWEGCA: src/swegca/mosaic_synapse_arbiter.py@5901a5a:96-122
    [[nodiscard]] const Digest256& receipt() const;
    // A guarded verification write uses exactly one bound proposal. This
    // identity is absent for multi-proposal previews, which grant no write.
    [[nodiscard]] std::optional<Digest256> single_binding_receipt() const;

private:
    friend class ProposalArbiter;
    using Bytes = std::vector<std::byte, AllocationAdapter<std::byte>>;
    using Flags = std::vector<std::uint8_t, AllocationAdapter<std::uint8_t>>;
    ArbitrationResult(StateGeneration based_on, std::uint64_t step, RoleMask changed,
                      ScalarType type, std::uint64_t width, Bytes delta, Flags accepted,
                      Flags conflict, Digest256 receipt,
                      std::optional<Digest256> single_binding_receipt);
    void require_live() const;

    StateGeneration based_on_;
    std::uint64_t step_;
    RoleMask changed_;
    ScalarType type_;
    std::uint64_t width_;
    Bytes delta_;
    Flags accepted_;
    Flags conflict_;
    Digest256 receipt_;
    std::optional<Digest256> single_binding_receipt_;
    bool live_ = true;
};

struct ArbitrationOutcome final {
    std::uint32_t failures = 0;
    Digest256 receipt{Digest256::Bytes{}};  // includes no-commit decisions
    std::optional<ArbitrationResult> candidate;
};

class ProposalArbiter final {
public:
    ProposalArbiter(const ProposalArbiter&) = delete;
    ProposalArbiter& operator=(const ProposalArbiter&) = delete;
    ProposalArbiter(ProposalArbiter&&) = delete;
    ProposalArbiter& operator=(ProposalArbiter&&) = delete;
    ~ProposalArbiter() = default;

    // The caller supplies Main's current state and step. Failures produce a
    // no-commit receipt; capacity exhaustion throws before any state change.
    // SWEGCA: src/swegca/mosaic_synapse_arbiter.py@5901a5a:238-321
    [[nodiscard]] ArbitrationOutcome arbitrate(
        const CognitiveState& state, std::uint64_t current_step,
        std::span<const BoundProposal> proposals) const;

private:
    friend class MainOwner;
    ProposalArbiter(const AllocationContext& memory, ArbiterPolicy policy);
    template <class T, class R>
    [[nodiscard]] ArbitrationOutcome arbitrate_typed(
        const CognitiveState& state, std::uint64_t current_step,
        std::span<const BoundProposal> proposals) const;

    AllocationContext memory_;
    ArbiterPolicy policy_;
    kernel::ArbiterRules rules_;
};

}  // namespace swegca::vrs
