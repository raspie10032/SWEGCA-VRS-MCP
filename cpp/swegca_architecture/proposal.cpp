#include "swegca_architecture/proposal.hpp"

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

}  // namespace swegca::architecture
