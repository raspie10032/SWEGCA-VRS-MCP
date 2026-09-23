#pragma once

#include "swegca_architecture/cognitive_state.hpp"
#include "swegca_architecture/memory_ledger.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// A producer's transient proposal over a detached Main snapshot. This type
// carries no decision, binding receipt, or authority. Its source and cited
// addresses are observations until Main binds them to admitted evidence.
// Rule: ARCHITECTURE_SPEC.md@5901a5a:64-69,117-123,155-174;
// reconstruction board §3D, §4, §10.5.
namespace swegca::architecture {

struct TensorDeltaInput final {
    ScalarType scalar_type;
    TensorShape3 shape;
    std::span<const std::byte> canonical_bytes;
};

struct SynapseProposalInput final {
    std::string_view source;
    std::string_view claim;
    std::uint64_t claim_revision;
    StateGeneration based_on;
    std::span<const std::string_view> evidence_addresses;
    std::span<const std::size_t> target_role_indices;
    TensorDeltaInput semantic_delta;
    TensorDeltaInput executive_delta;
    TensorDeltaInput scratch_delta;
    double confidence;
    double contradiction;
    double uncertainty;
};

class SynapseProposal final {
public:
    // Copies every borrowed input into Main's request account. The proposal
    // remains transient and its data grants no write authority. Empty evidence
    // or targets can be recorded as a producer output, but Main's Bind rejects
    // either before any authority-bearing path.
    // SWEGCA: src/swegca/mosaic_synapse_arbiter.py@5901a5a:54-93
    SynapseProposal(const MemoryLedger::Account& memory,
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
                                                     MemoryLedger::Allocator<char>>>
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
    [[nodiscard]] double confidence() const noexcept { return confidence_; }
    // SWEGCA: src/swegca/mosaic_synapse_arbiter.py@5901a5a:73-93
    [[nodiscard]] double contradiction() const noexcept { return contradiction_; }
    // SWEGCA: src/swegca/mosaic_synapse_arbiter.py@5901a5a:73-93
    [[nodiscard]] double uncertainty() const noexcept { return uncertainty_; }

private:
    using Text = std::basic_string<char, std::char_traits<char>, MemoryLedger::Allocator<char>>;
    using Addresses = std::vector<Text, MemoryLedger::Allocator<Text>>;

    ProducerId source_;
    ClaimRevision claim_;
    StateGeneration based_on_;
    Addresses evidence_addresses_;
    RoleMask targets_;
    CognitiveTensor semantic_delta_;
    CognitiveTensor executive_delta_;
    CognitiveTensor scratch_delta_;
    double confidence_;
    double contradiction_;
    double uncertainty_;
};

}  // namespace swegca::architecture
