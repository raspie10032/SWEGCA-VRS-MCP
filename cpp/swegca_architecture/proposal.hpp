#pragma once

#include "swegca_architecture/cognitive_state.hpp"
#include "swegca_architecture/allocation.hpp"
#include "swegca_architecture/native_tensor.hpp"
#include "swegca_architecture/role_registry.hpp"

#include <cstddef>
#include <cstdint>
#include <array>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// A producer's transient proposal over a detached Main snapshot. This type
// carries no decision, binding receipt, or authority. Its source and cited
// addresses are observations until Main binds them to admitted evidence.
// Target indices refer to named roles; unregistered spare tensor slots are
// capacity only and cannot be targeted by a producer proposal.
// Rule: ARCHITECTURE_SPEC.md@5901a5a:64-69,117-123,155-174;
// reconstruction board §3D, §4, §10.5.
namespace swegca::architecture {

struct TensorDeltaInput final {
    ScalarType scalar_type;
    TensorShape3 shape;
    std::span<const std::byte> canonical_bytes;
};

// One element of an original [batch] score tensor. Its tensor dtype is
// independent of both the other scores and the candidate delta dtype.
struct ScoreScalarInput final {
    ScalarType scalar_type;
    std::span<const std::byte> canonical_bytes;
};

struct ScoreScalar final {
    ScalarType scalar_type;
    std::array<std::byte, 8> bytes{};
    // Numeric -0 is normalized for arithmetic; bytes retain its input bit.
    double value = 0;  // exact widening of the stored binary16/32/64 value
};

struct SynapseProposalInput final {
    std::string_view source;
    std::string_view claim;
    std::uint64_t claim_revision;
    StateGeneration based_on;
    // Untrusted citation text. Main's Bind verifies address form, publication,
    // experience kind (original or derived), and equality with the decision's
    // admitted set.
    std::span<const std::string_view> evidence_addresses;
    std::span<const std::size_t> target_role_indices;  // indices in snapshot.state().roles()
    TensorDeltaInput semantic_delta;
    TensorDeltaInput executive_delta;
    TensorDeltaInput scratch_delta;
    ScoreScalarInput confidence;
    ScoreScalarInput contradiction;
    ScoreScalarInput uncertainty;
};

class SynapseProposal final {
public:
    // Copies every borrowed input into Main's request account. The proposal
    // remains transient and its data grants no write authority. Empty evidence
    // or targets can be recorded as a producer output, but Main's Bind rejects
    // either before any authority-bearing path.
    // SWEGCA: src/swegca/mosaic_synapse_arbiter.py@5901a5a:54-93
    SynapseProposal(const AllocationContext& memory,
                    const StateSnapshot& snapshot,
                    const SynapseProposalInput& input);

    SynapseProposal(SynapseProposal&&) noexcept = default;
    SynapseProposal& operator=(SynapseProposal&&) = delete;
    SynapseProposal(const SynapseProposal&) = delete;
    SynapseProposal& operator=(const SynapseProposal&) = delete;
    ~SynapseProposal() = default;

    // SWEGCA: src/swegca/mosaic_synapse_arbiter.py@5901a5a:54-93
    [[nodiscard]] const ProducerId& source() const noexcept { return source_; }
    // SWEGCA: src/swegca/mosaic_synapse_arbiter.py@5901a5a:54-93
    [[nodiscard]] const ClaimRevision& claim() const noexcept { return claim_; }
    // SWEGCA: src/swegca/mosaic_synapse_arbiter.py@5901a5a:54-93
    [[nodiscard]] const StateGeneration& based_on() const noexcept { return based_on_; }
    // SWEGCA: src/swegca/mosaic_synapse_arbiter.py@5901a5a:54-93
    [[nodiscard]] std::span<const std::basic_string<char, std::char_traits<char>,
                                                     AllocationAdapter<char>>>
    evidence_addresses() const noexcept { return evidence_addresses_; }
    // SWEGCA: src/swegca/mosaic_synapse_arbiter.py@5901a5a:54-93
    [[nodiscard]] const RoleMask& targets() const noexcept { return targets_; }
    // SWEGCA: src/swegca/mosaic_synapse_arbiter.py@5901a5a:54-93
    [[nodiscard]] const CognitiveTensor& semantic_delta() const noexcept { return semantic_delta_; }
    // SWEGCA: src/swegca/mosaic_synapse_arbiter.py@5901a5a:54-93
    [[nodiscard]] const CognitiveTensor& executive_delta() const noexcept { return executive_delta_; }
    // SWEGCA: src/swegca/mosaic_synapse_arbiter.py@5901a5a:54-93
    [[nodiscard]] const CognitiveTensor& scratch_delta() const noexcept { return scratch_delta_; }
    // SWEGCA: src/swegca/mosaic_synapse_arbiter.py@5901a5a:73-93
    [[nodiscard]] const ScoreScalar& confidence() const noexcept { return confidence_; }
    // SWEGCA: src/swegca/mosaic_synapse_arbiter.py@5901a5a:73-93
    [[nodiscard]] const ScoreScalar& contradiction() const noexcept { return contradiction_; }
    // SWEGCA: src/swegca/mosaic_synapse_arbiter.py@5901a5a:73-93
    [[nodiscard]] const ScoreScalar& uncertainty() const noexcept { return uncertainty_; }

private:
    using Text = std::basic_string<char, std::char_traits<char>, AllocationAdapter<char>>;
    using Addresses = std::vector<Text, AllocationAdapter<Text>>;

    ProducerId source_;
    ClaimRevision claim_;
    StateGeneration based_on_;
    Addresses evidence_addresses_;
    RoleMask targets_;
    CognitiveTensor semantic_delta_;
    CognitiveTensor executive_delta_;
    CognitiveTensor scratch_delta_;
    ScoreScalar confidence_;
    ScoreScalar contradiction_;
    ScoreScalar uncertainty_;
};

// Canonical identities of the actual delta tensors and target mask held by a
// proposal. Main supplies these exact digests when it asks the accumulator
// for a decision; Bind recomputes them from the proposal it will arbitrate.
// SWEGCA: paper/swegca/ARCHITECTURE_SPEC.md@5901a5a:154-162
[[nodiscard]] Digest256 proposal_delta_digest(const SynapseProposal& proposal);
// SWEGCA: paper/swegca/ARCHITECTURE_SPEC.md@5901a5a:154-162
[[nodiscard]] Digest256 proposal_mask_digest(const SynapseProposal& proposal);
// Every producer field that can affect binding or arbitration, including the
// exact citation order and score bits. A receipt can audit the complete input.
// SWEGCA: paper/swegca/ARCHITECTURE_SPEC.md@5901a5a:154-162
[[nodiscard]] Digest256 proposal_content_digest(const SynapseProposal& proposal);

// Main's successful Bind result. Only EvidenceGate can construct one, and it
// carries no state-write capability. Arbitration receives this type, never an
// unbound producer proposal. A moved-from shell cannot be read as a bound one.
// SWEGCA: paper/swegca/ARCHITECTURE_SPEC.md@5901a5a:154-174
class BoundProposal final {
public:
    BoundProposal(BoundProposal&& other) noexcept;
    BoundProposal& operator=(BoundProposal&&) = delete;
    BoundProposal(const BoundProposal&) = delete;
    BoundProposal& operator=(const BoundProposal&) = delete;
    ~BoundProposal() = default;

    // SWEGCA: paper/swegca/ARCHITECTURE_SPEC.md@5901a5a:154-174
    [[nodiscard]] const SynapseProposal& proposal() const;
    // SWEGCA: paper/swegca/ARCHITECTURE_SPEC.md@5901a5a:154-174
    [[nodiscard]] const Digest256& decision_digest() const;
    // SWEGCA: paper/swegca/ARCHITECTURE_SPEC.md@5901a5a:154-174
    [[nodiscard]] const Digest256& binding_digest() const;
    // SWEGCA: paper/swegca/ARCHITECTURE_SPEC.md@5901a5a:154-174
    [[nodiscard]] const Digest256& binding_receipt() const;
    // The writer must reject an expired binding or recheck evidence at its
    // own current step before authority is issued.
    // SWEGCA: paper/swegca/ARCHITECTURE_SPEC.md@5901a5a:141-150
    [[nodiscard]] std::uint64_t validated_at_step() const;

private:
    friend class EvidenceGate;
    BoundProposal(SynapseProposal proposal, Digest256 decision_digest,
                  Digest256 binding_digest, Digest256 binding_receipt,
                  std::uint64_t validated_at_step);
    void require_live() const;

    SynapseProposal proposal_;
    Digest256 decision_digest_;
    Digest256 binding_digest_;
    Digest256 binding_receipt_;
    std::uint64_t validated_at_step_;
    bool live_ = true;
};

}  // namespace swegca::architecture
