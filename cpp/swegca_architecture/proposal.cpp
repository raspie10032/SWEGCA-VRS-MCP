#include "swegca_architecture/proposal.hpp"
#include "swegca_architecture/sha256.hpp"

#include <array>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace swegca::architecture {
namespace {

// The candidate delta has exactly the detached state's partition shapes and
// scalar types. The tensor constructor separately checks byte count, finite
// values, and canonical signed zero for every supported scalar format.
// SWEGCA: src/swegca/mosaic_synapse_arbiter.py@5901a5a:65-93
void require_matching_delta(const CognitiveTensor& delta, const CognitiveTensor& state) {
    if (delta.scalar_type() != state.scalar_type() || delta.shape() != state.shape())
        throw std::invalid_argument("proposal_delta_state_shape_mismatch");
}

// SWEGCA: src/swegca/mosaic_synapse_arbiter.py@5901a5a:73-93
void require_unit_score(double value) {
    if (!std::isfinite(value) || value < 0.0 || value > 1.0)
        throw std::invalid_argument("proposal_score_out_of_range");
}

// Fixed-width little-endian encoding keeps the digest independent of host
// word order and of the compiler's object layout.
// SWEGCA: paper/swegca/ARCHITECTURE_SPEC.md@5901a5a:154-162
void hash_u64(Sha256& hash, std::uint64_t value) {
    std::array<std::byte, 8> bytes{};
    for (std::size_t index = 0; index < bytes.size(); ++index)
        bytes[index] = static_cast<std::byte>((value >> (8 * index)) & 0xff);
    hash.update(bytes);
}

// SWEGCA: paper/swegca/ARCHITECTURE_SPEC.md@5901a5a:154-162
void hash_tensor(Sha256& hash, std::uint8_t partition, const CognitiveTensor& tensor) {
    hash_u64(hash, partition);
    hash_u64(hash, static_cast<std::uint8_t>(tensor.scalar_type()));
    hash_u64(hash, static_cast<std::uint8_t>(tensor.byte_order()));
    hash_u64(hash, tensor.shape().batches);
    hash_u64(hash, tensor.shape().slots);
    hash_u64(hash, tensor.shape().width);
    hash_u64(hash, tensor.byte_count());
    hash.update(tensor.bytes());
}

}  // namespace

// SWEGCA: src/swegca/mosaic_synapse_arbiter.py@5901a5a:54-93
SynapseProposal::SynapseProposal(const MemoryLedger::Account& memory,
                                 const StateSnapshot& snapshot,
                                 const SynapseProposalInput& input)
    : source_(memory, input.source),
      claim_(ClaimId(memory, input.claim), input.claim_revision),
      based_on_(input.based_on),
      evidence_addresses_(memory.allocator<Text>()),
      targets_(RoleMask::from_indices(snapshot.state().roles(), input.target_role_indices)),
      semantic_delta_(memory, input.semantic_delta.scalar_type,
                      input.semantic_delta.shape, input.semantic_delta.canonical_bytes),
      executive_delta_(memory, input.executive_delta.scalar_type,
                       input.executive_delta.shape, input.executive_delta.canonical_bytes),
      scratch_delta_(memory, input.scratch_delta.scalar_type,
                     input.scratch_delta.shape, input.scratch_delta.canonical_bytes),
      // SWEGCA: src/swegca/mosaic_synapse_arbiter.py@5901a5a:54-93
      confidence_(input.confidence),
      contradiction_(input.contradiction),
      uncertainty_(input.uncertainty) {
    const auto& state = snapshot.state();
    if (based_on_ != state.generation())
        throw std::invalid_argument("proposal_snapshot_generation_mismatch");
    require_matching_delta(semantic_delta_, state.semantic());
    require_matching_delta(executive_delta_, state.executive());
    require_matching_delta(scratch_delta_, state.scratch());
    require_unit_score(confidence_);
    require_unit_score(contradiction_);
    require_unit_score(uncertainty_);
    evidence_addresses_.reserve(input.evidence_addresses.size());
    for (const auto address : input.evidence_addresses) {
        detail::require_identity_text(address, ExperienceAddressTag::name);
        Text kept{memory.allocator<char>()};
        kept.assign(address);
        evidence_addresses_.push_back(std::move(kept));
    }
}

// Re-created (user@2026-09-23): native canonical Bind identity for the
// proposal's actual three-part delta, under SWEGCA §4.5.
// SWEGCA: paper/swegca/ARCHITECTURE_SPEC.md@5901a5a:154-162
Digest256 proposal_delta_digest(const SynapseProposal& proposal) {
    Sha256 hash;
    hash.update("swegca.proposal_delta.v1");
    hash_tensor(hash, 1, proposal.semantic_delta());
    hash_tensor(hash, 2, proposal.executive_delta());
    hash_tensor(hash, 3, proposal.scratch_delta());
    return Digest256(hash.finish());
}

// Re-created (user@2026-09-23): native canonical Bind identity for the
// complete role mask. Registry identity prevents a mask from another state
// layout acquiring the same meaning.
// SWEGCA: paper/swegca/ARCHITECTURE_SPEC.md@5901a5a:154-162
Digest256 proposal_mask_digest(const SynapseProposal& proposal) {
    const auto& mask = proposal.targets();
    Sha256 hash;
    hash.update("swegca.proposal_mask.v1");
    hash.update(mask.registry_digest().bytes());
    hash_u64(hash, mask.role_count());
    hash_u64(hash, mask.words().size());
    for (const auto word : mask.words()) hash_u64(hash, word);
    return Digest256(hash.finish());
}

// SWEGCA: paper/swegca/ARCHITECTURE_SPEC.md@5901a5a:154-174
BoundProposal::BoundProposal(SynapseProposal proposal, Digest256 decision_digest,
                             Digest256 binding_digest, Digest256 binding_receipt)
    : proposal_(std::move(proposal)), decision_digest_(decision_digest),
      binding_digest_(binding_digest), binding_receipt_(binding_receipt) {}

// SWEGCA: paper/swegca/ARCHITECTURE_SPEC.md@5901a5a:154-174
BoundProposal::BoundProposal(BoundProposal&& other)
    : proposal_(std::move(other.proposal_)), decision_digest_(other.decision_digest_),
      binding_digest_(other.binding_digest_), binding_receipt_(other.binding_receipt_),
      live_(std::exchange(other.live_, false)) {}

// SWEGCA: paper/swegca/ARCHITECTURE_SPEC.md@5901a5a:154-174
void BoundProposal::require_live() const {
    if (!live_) throw std::logic_error("bound_proposal_moved_from");
}

// SWEGCA: paper/swegca/ARCHITECTURE_SPEC.md@5901a5a:154-174
const SynapseProposal& BoundProposal::proposal() const {
    require_live();
    return proposal_;
}

// SWEGCA: paper/swegca/ARCHITECTURE_SPEC.md@5901a5a:154-174
const Digest256& BoundProposal::decision_digest() const {
    require_live();
    return decision_digest_;
}

// SWEGCA: paper/swegca/ARCHITECTURE_SPEC.md@5901a5a:154-174
const Digest256& BoundProposal::binding_digest() const {
    require_live();
    return binding_digest_;
}

// SWEGCA: paper/swegca/ARCHITECTURE_SPEC.md@5901a5a:154-174
const Digest256& BoundProposal::binding_receipt() const {
    require_live();
    return binding_receipt_;
}

}  // namespace swegca::architecture
