#include "swegca_architecture/proposal.hpp"
#include "swegca_architecture/scalar_codec.hpp"
#include "swegca_architecture/sha256.hpp"

#include <array>
#include <bit>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace swegca::architecture {
namespace {

// The candidate delta has exactly the detached state's partition shapes.
// Its scalar dtype is independent of the state, as in SynapseProposal.validate.
// The tensor constructor separately checks byte count, finite
// values, and canonical signed zero for every supported scalar format.
// SWEGCA: src/swegca/mosaic_synapse_arbiter.py@5901a5a:65-93
void require_matching_delta_shape(const CognitiveTensor& delta,
                                  const CognitiveTensor& state) {
    if (delta.shape() != state.shape())
        throw std::invalid_argument("proposal_delta_state_shape_mismatch");
}

// SWEGCA: src/swegca/mosaic_synapse_arbiter.py@5901a5a:73-93
ScoreScalar keep_unit_score(const ScoreScalarInput& input) {
    const auto width = scalar_width(input.scalar_type);
    if (input.canonical_bytes.size() != width)
        throw std::invalid_argument("proposal_score_width_mismatch");
    const double value = input.scalar_type == ScalarType::float64
                             ? read_scalar64(input.scalar_type, input.canonical_bytes)
                             : static_cast<double>(read_scalar32(input.scalar_type,
                                                                  input.canonical_bytes));
    if (!std::isfinite(value) || value < 0.0 || value > 1.0)
        throw std::invalid_argument("proposal_score_out_of_range");
    ScoreScalar score{input.scalar_type, {}, value};
    for (std::size_t at = 0; at < width; ++at)
        score.bytes[at] = input.canonical_bytes[at];
    return score;
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
void hash_text(Sha256& hash, std::string_view value) {
    hash_u64(hash, value.size());
    hash.update(value);
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
    tensor.for_each_chunk([&hash](std::span<const std::byte> chunk) {
        hash.update(chunk);
    });
}

}  // namespace

// SWEGCA: src/swegca/mosaic_synapse_arbiter.py@5901a5a:54-93
SynapseProposal::SynapseProposal(const AllocationContext& memory,
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
      confidence_(keep_unit_score(input.confidence)),
      contradiction_(keep_unit_score(input.contradiction)),
      uncertainty_(keep_unit_score(input.uncertainty)) {
    const auto& state = snapshot.state();
    if (based_on_ != state.generation())
        throw std::invalid_argument("proposal_snapshot_generation_mismatch");
    require_matching_delta_shape(semantic_delta_, state.semantic());
    require_matching_delta_shape(executive_delta_, state.executive());
    require_matching_delta_shape(scratch_delta_, state.scratch());
    // One original delta_candidate is split across these three partitions.
    if (semantic_delta_.scalar_type() != executive_delta_.scalar_type() ||
        semantic_delta_.scalar_type() != scratch_delta_.scalar_type())
        throw std::invalid_argument("proposal_delta_partition_dtype_mismatch");
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

// Re-created (user@2026-09-23): one canonical identity for every immutable
// field the Bind gate and conflict arbiter may inspect. Every score's own
// tensor dtype and stored bits are part of its identity.
// SWEGCA: paper/swegca/ARCHITECTURE_SPEC.md@5901a5a:154-162
Digest256 proposal_content_digest(const SynapseProposal& proposal) {
    Sha256 hash;
    hash_text(hash, "swegca.proposal_content.v1");
    hash_text(hash, proposal.source().value());
    hash_text(hash, proposal.claim().claim().value());
    hash_u64(hash, proposal.claim().revision());
    hash_u64(hash, proposal.based_on().ordinal());
    hash.update(proposal.based_on().digest().bytes());
    hash_u64(hash, proposal.evidence_addresses().size());
    for (const auto& address : proposal.evidence_addresses()) hash_text(hash, address);
    hash.update(proposal_delta_digest(proposal).bytes());
    hash.update(proposal_mask_digest(proposal).bytes());
    const auto score = [&hash](const ScoreScalar& value) {
        const auto width = scalar_width(value.scalar_type);
        hash_u64(hash, static_cast<std::uint8_t>(value.scalar_type));
        hash_u64(hash, width);
        hash.update(std::span<const std::byte>(value.bytes.data(), width));
    };
    score(proposal.confidence());
    score(proposal.contradiction());
    score(proposal.uncertainty());
    return Digest256(hash.finish());
}

// SWEGCA: paper/swegca/ARCHITECTURE_SPEC.md@5901a5a:154-174
BoundProposal::BoundProposal(SynapseProposal proposal, Digest256 decision_digest,
                             Digest256 binding_digest, Digest256 binding_receipt,
                             std::uint64_t validated_at_step)
    : proposal_(std::move(proposal)), decision_digest_(decision_digest),
      binding_digest_(binding_digest), binding_receipt_(binding_receipt),
      validated_at_step_(validated_at_step) {}

// SWEGCA: paper/swegca/ARCHITECTURE_SPEC.md@5901a5a:154-174
BoundProposal::BoundProposal(BoundProposal&& other) noexcept
    : proposal_(std::move(other.proposal_)), decision_digest_(other.decision_digest_),
      binding_digest_(other.binding_digest_), binding_receipt_(other.binding_receipt_),
      validated_at_step_(other.validated_at_step_),
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

// SWEGCA: paper/swegca/ARCHITECTURE_SPEC.md@5901a5a:141-150
std::uint64_t BoundProposal::validated_at_step() const {
    require_live();
    return validated_at_step_;
}

}  // namespace swegca::architecture
