#include "swegca_vrs/proposal_arbiter.hpp"

#include "swegca_vrs/scalar_codec.hpp"
#include "swegca_vrs/core_sha256.hpp"

#include <array>
#include <bit>
#include <limits>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace swegca::vrs {
namespace {

// SWEGCA: src/swegca/mosaic_synapse_arbiter.py@5901a5a:96-122
void hash_u64(Sha256& hash, std::uint64_t value) {
    std::array<std::byte, 8> bytes{};
    for (std::size_t at = 0; at < bytes.size(); ++at)
        bytes[at] = static_cast<std::byte>((value >> (8 * at)) & 0xff);
    hash.update(bytes);
}

// SWEGCA: src/swegca/mosaic_synapse_arbiter.py@5901a5a:222-236
void hash_policy(Sha256& hash, const ArbiterPolicy& policy) {
    hash_u64(hash, std::bit_cast<std::uint64_t>(policy.maximum_slot_delta));
    hash_u64(hash, std::bit_cast<std::uint64_t>(policy.maximum_world_delta));
    hash_u64(hash, std::bit_cast<std::uint64_t>(policy.minimum_weight + 0.0));
}

// SWEGCA: src/swegca/mosaic_synapse_arbiter.py@5901a5a:238-262
std::size_t checked_product(std::size_t left, std::size_t right) {
    if (right != 0 && left > std::numeric_limits<std::size_t>::max() / right)
        throw std::overflow_error("arbiter_shape_overflow");
    return left * right;
}

// SWEGCA: src/swegca/mosaic_synapse_arbiter.py@5901a5a:238-262
const CognitiveTensor& partition_delta(const SynapseProposal& proposal,
                                       TensorPartition partition) {
    switch (partition) {
        case TensorPartition::semantic: return proposal.semantic_delta();
        case TensorPartition::executive: return proposal.executive_delta();
        case TensorPartition::scratch: return proposal.scratch_delta();
    }
    throw std::invalid_argument("arbiter_partition_invalid");
}

}  // namespace

// SWEGCA: src/swegca/mosaic_synapse_arbiter.py@5901a5a:96-122
ArbitrationResult::ArbitrationResult(StateGeneration based_on, std::uint64_t step,
                                     RoleMask changed, ScalarType type, std::uint64_t width,
                                     Bytes delta, Flags accepted, Flags conflict,
                                     Digest256 receipt,
                                     std::optional<Digest256> single_binding_receipt)
    : based_on_(std::move(based_on)), step_(step), changed_(std::move(changed)),
      type_(type), width_(width), delta_(std::move(delta)),
      accepted_(std::move(accepted)), conflict_(std::move(conflict)), receipt_(receipt),
      single_binding_receipt_(single_binding_receipt) {}

// SWEGCA: src/swegca/mosaic_synapse_arbiter.py@5901a5a:96-122
ArbitrationResult::ArbitrationResult(ArbitrationResult&& other) noexcept
    : based_on_(std::move(other.based_on_)), step_(other.step_),
      changed_(std::move(other.changed_)), type_(other.type_), width_(other.width_),
      delta_(std::move(other.delta_)), accepted_(std::move(other.accepted_)),
      conflict_(std::move(other.conflict_)), receipt_(other.receipt_),
      single_binding_receipt_(other.single_binding_receipt_),
      live_(std::exchange(other.live_, false)) {}

// SWEGCA: src/swegca/mosaic_synapse_arbiter.py@5901a5a:96-122
void ArbitrationResult::require_live() const {
    if (!live_) throw std::logic_error("arbitration_result_moved_from");
}

// SWEGCA: src/swegca/mosaic_synapse_arbiter.py@5901a5a:96-122
const StateGeneration& ArbitrationResult::based_on() const {
    require_live();
    return based_on_;
}

// SWEGCA: src/swegca/mosaic_synapse_arbiter.py@5901a5a:96-122
std::uint64_t ArbitrationResult::validated_at_step() const {
    require_live();
    return step_;
}

// SWEGCA: src/swegca/mosaic_synapse_arbiter.py@5901a5a:96-122
const RoleMask& ArbitrationResult::changed_roles() const {
    require_live();
    return changed_;
}

// SWEGCA: src/swegca/mosaic_synapse_arbiter.py@5901a5a:96-122
std::span<const std::byte> ArbitrationResult::role_delta() const {
    require_live();
    return delta_;
}

// SWEGCA: src/swegca/mosaic_synapse_arbiter.py@5901a5a:96-122
ScalarType ArbitrationResult::scalar_type() const {
    require_live();
    return type_;
}

// SWEGCA: src/swegca/mosaic_synapse_arbiter.py@5901a5a:96-122
std::uint64_t ArbitrationResult::width() const {
    require_live();
    return width_;
}

// SWEGCA: src/swegca/mosaic_synapse_arbiter.py@5901a5a:96-122
std::span<const std::uint8_t> ArbitrationResult::accepted() const {
    require_live();
    return accepted_;
}

// SWEGCA: src/swegca/mosaic_synapse_arbiter.py@5901a5a:96-122
std::span<const std::uint8_t> ArbitrationResult::conflicted_roles() const {
    require_live();
    return conflict_;
}

// SWEGCA: src/swegca/mosaic_synapse_arbiter.py@5901a5a:96-122
const Digest256& ArbitrationResult::receipt() const {
    require_live();
    return receipt_;
}

// SWEGCA: src/swegca/mosaic_synapse_arbiter.py@5901a5a:96-122
std::optional<Digest256> ArbitrationResult::single_binding_receipt() const {
    require_live();
    return single_binding_receipt_;
}

// SWEGCA: src/swegca/mosaic_synapse_arbiter.py@5901a5a:219-236
ProposalArbiter::ProposalArbiter(const AllocationContext& memory, ArbiterPolicy policy)
    : memory_(memory), policy_(policy), rules_(make_arbiter_rules(policy)) {}

// SWEGCA: src/swegca/mosaic_synapse_arbiter.py@5901a5a:238-321
ArbitrationOutcome ProposalArbiter::arbitrate(
    const CognitiveState& state, std::uint64_t current_step,
    std::span<const BoundProposal> proposals) const {
    // torch.stack promotes the proposal weight tensors before the weighted
    // delta multiplication; that result may be wider than the state tensor.
    ScalarType weight_type = ScalarType::float16;
    if (!proposals.empty()) weight_type = proposals.front().proposal().confidence().scalar_type;
    ScalarType delta_type = state.semantic().scalar_type();
    if (!proposals.empty())
        delta_type = proposals.front().proposal().semantic_delta().scalar_type();
    for (const auto& bound : proposals) {
        const auto& p = bound.proposal();
        weight_type = kernel::arbiter_promote(weight_type, p.confidence().scalar_type);
        weight_type = kernel::arbiter_promote(weight_type, p.contradiction().scalar_type);
        weight_type = kernel::arbiter_promote(weight_type, p.uncertainty().scalar_type);
        delta_type = kernel::arbiter_promote(delta_type,
                                              p.semantic_delta().scalar_type());
    }
    if (delta_type == ScalarType::float64)
        return arbitrate_typed<double, double>(state, current_step, proposals);
    if (weight_type == ScalarType::float64)
        return arbitrate_typed<float, double>(state, current_step, proposals);
    return arbitrate_typed<float, float>(state, current_step, proposals);
}

// Mask first, then slot clipping, directional conflict, weighted reduction,
// and whole-state clipping are performed by the pure typed SWEGCA kernel.
// The shell owns every buffer on Main's Account and maps only registered
// roles. The output uses the promoted delta/stacked-weight scalar type.
// SWEGCA: src/swegca/mosaic_synapse_arbiter.py@5901a5a:238-321
template <class T, class R>
ArbitrationOutcome ProposalArbiter::arbitrate_typed(
    const CognitiveState& state, std::uint64_t current_step,
    std::span<const BoundProposal> proposals) const {
    using Scores = kernel::ProposalScoresOf<T>;
    using ScoreList = std::vector<Scores, AllocationAdapter<Scores>>;
    using Values = std::vector<T, AllocationAdapter<T>>;
    using ResultValues = std::vector<R, AllocationAdapter<R>>;
    using Weights = std::vector<double, AllocationAdapter<double>>;
    using Flags = ArbitrationResult::Flags;
    using Bytes = ArbitrationResult::Bytes;
    using Indices = std::vector<std::size_t, AllocationAdapter<std::size_t>>;
    const auto& registry = state.roles();
    const auto P = proposals.size();
    const auto S = registry.size();
    const auto W = static_cast<std::size_t>(state.semantic().shape().width);
    const auto state_type = state.semantic().scalar_type();
    ScalarType weight_type = ScalarType::float16;
    if (!proposals.empty()) weight_type = proposals.front().proposal().confidence().scalar_type;
    ScalarType delta_type = state_type;
    if (!proposals.empty())
        delta_type = proposals.front().proposal().semantic_delta().scalar_type();
    for (const auto& bound : proposals) {
        const auto& p = bound.proposal();
        weight_type = kernel::arbiter_promote(weight_type, p.confidence().scalar_type);
        weight_type = kernel::arbiter_promote(weight_type, p.contradiction().scalar_type);
        weight_type = kernel::arbiter_promote(weight_type, p.uncertainty().scalar_type);
        delta_type = kernel::arbiter_promote(delta_type,
                                              p.semantic_delta().scalar_type());
    }
    const auto result_type = proposals.empty()
                                 ? state_type
                                 : kernel::arbiter_promote(delta_type, weight_type);
    const auto scalar_bytes = scalar_width(result_type);
    std::uint32_t failures = 0;
    // This native arbiter has a scalar score and one role delta per proposal.
    // Until those buffers carry a batch axis, reject every nonempty proposal
    // set whose state batch is not one; otherwise delta_at would read only
    // batch zero. The empty proposal set remains the source's no-op path.
    if ((P != 0 && state.semantic().shape().batches != 1) ||
        P > std::numeric_limits<std::uint32_t>::max() || S == 0 || W == 0 ||
        state.semantic().shape().width > std::numeric_limits<std::size_t>::max())
        failures |= arbitration_invalid_shape;
    for (const auto& bound : proposals) {
        if (bound.proposal().based_on() != state.generation())
            failures |= arbitration_stale_generation;
        if (bound.validated_at_step() != current_step)
            failures |= arbitration_stale_step;
        if (!bound.proposal().targets().matches(registry))
            failures |= arbitration_registry_mismatch;
    }

    Sha256 receipt;
    receipt.update("swegca.arbitration_receipt.v1");
    hash_u64(receipt, state.generation().ordinal());
    receipt.update(state.generation().digest().bytes());
    receipt.update(registry.digest().bytes());
    hash_u64(receipt, current_step);
    hash_u64(receipt, static_cast<std::uint8_t>(state_type));
    hash_u64(receipt, static_cast<std::uint8_t>(delta_type));
    hash_u64(receipt, static_cast<std::uint8_t>(result_type));
    hash_u64(receipt, W);
    hash_policy(receipt, policy_);
    hash_u64(receipt, P);
    for (const auto& bound : proposals) receipt.update(bound.binding_receipt().bytes());
    if (P == 0 || failures != 0) {
        hash_u64(receipt, failures);
        ArbitrationOutcome out;
        out.failures = failures;
        out.receipt = Digest256(receipt.finish());
        return out;
    }

    const auto SW = checked_product(S, W);
    (void)checked_product(checked_product(P, S), W);
    const auto byte_count = checked_product(SW, scalar_bytes);
    ScoreList scores(P, Scores{}, memory_.allocator<Scores>());
    Weights weights(P, 0.0, memory_.allocator<double>());
    Flags accepted(P, 0, memory_.allocator<std::uint8_t>());
    Flags conflict(S, 0, memory_.allocator<std::uint8_t>());
    ResultValues output(SW, R{0}, memory_.allocator<R>());
    Weights work_weights(P, 0.0, memory_.allocator<double>());
    Flags work_accepted(P, 0, memory_.allocator<std::uint8_t>());
    Flags work_conflict(S, 0, memory_.allocator<std::uint8_t>());
    ResultValues work_output(SW, R{0}, memory_.allocator<R>());
    Flags slot_mask(P, 0, memory_.allocator<std::uint8_t>());
    Values slot_denominator(P, T{0}, memory_.allocator<T>());
    Values slot_scale(P, T{0}, memory_.allocator<T>());

    for (std::size_t p = 0; p < P; ++p) {
        const auto& proposal = proposals[p].proposal();
        std::uint32_t source_id = static_cast<std::uint32_t>(p + 1);
        for (std::size_t earlier = 0; earlier < p; ++earlier)
            if (proposals[earlier].proposal().source() == proposal.source()) {
                source_id = scores[earlier].source;
                break;
            }
        scores[p] = Scores{{proposal.confidence().scalar_type,
                            proposal.confidence().value},
                           {proposal.contradiction().scalar_type,
                            proposal.contradiction().value},
                           {proposal.uncertainty().scalar_type,
                            proposal.uncertainty().value},
                           proposal.semantic_delta().scalar_type(), source_id};
    }

    const auto mask_at = [&](std::size_t p, std::size_t r,
                             std::uint8_t& mask) noexcept -> bool {
        try {
            if (p >= P || r >= S) return false;
            mask = proposals[p].proposal().targets().test(r) ? 1 : 0;
            return true;
        } catch (...) {
            return false;
        }
    };
    const auto delta_at = [&](std::size_t p, std::size_t r, std::size_t w,
                              T& delta) noexcept -> bool {
        try {
            if (p >= P || r >= S || w >= W) return false;
            const auto& role = registry.at(r);
            if (role.slot > (std::numeric_limits<std::size_t>::max() - w) / W)
                return false;
            const auto element = static_cast<std::size_t>(role.slot) * W + w;
            const auto& tensor = partition_delta(proposals[p].proposal(), role.partition);
            if (tensor.scalar_type() == ScalarType::float64) {
                if constexpr (std::is_same_v<T, double>)
                    return try_read_scalar64(tensor, element, delta);
                else
                    return false;
            }
            float narrow = 0;
            if (!try_read_scalar32(tensor, element, narrow)) return false;
            delta = static_cast<T>(narrow);
            return true;
        } catch (...) {
            return false;
        }
    };
    const kernel::ArbiterShape shape{P, S, W};
    const kernel::ArbiterViewBuffersOf<T, R> buffers{
        std::span<double>(weights), std::span<std::uint8_t>(accepted),
        std::span<std::uint8_t>(conflict), std::span<R>(output),
        std::span<double>(work_weights), std::span<std::uint8_t>(work_accepted),
        std::span<std::uint8_t>(work_conflict), std::span<R>(work_output),
        std::span<std::uint8_t>(slot_mask), std::span<T>(slot_denominator),
        std::span<T>(slot_scale), delta_type, result_type};
    if (!kernel::arbitrate_view(rules_, shape, std::span<const Scores>(scores),
                                mask_at, delta_at, buffers)) {
        failures |= arbitration_kernel_refused;
        hash_u64(receipt, failures);
        ArbitrationOutcome out;
        out.failures = failures;
        out.receipt = Digest256(receipt.finish());
        return out;
    }

    Bytes encoded(byte_count, std::byte{0}, memory_.allocator<std::byte>());
    Indices changed(memory_.allocator<std::size_t>());
    changed.reserve(S);
    for (std::size_t r = 0; r < S; ++r) {
        bool nonzero = false;
        for (std::size_t w = 0; w < W; ++w) {
            const auto offset = (r * W + w) * scalar_bytes;
            auto scalar = std::span<std::byte>(encoded).subspan(offset, scalar_bytes);
            if constexpr (std::is_same_v<R, double>)
                write_scalar64(result_type, output[r * W + w], scalar);
            else
                write_scalar32(result_type, output[r * W + w], scalar);
            // The source checks proposed_delta.abs().any(), so -0 is not a
            // changed slot even though its stored sign bit must survive.
            for (std::size_t at = 0; at < scalar.size(); ++at) {
                auto bits = std::to_integer<std::uint8_t>(scalar[at]);
                if (at + 1 == scalar.size()) bits &= 0x7fu;
                nonzero |= bits != 0;
            }
        }
        if (nonzero) changed.push_back(r);
    }
    auto changed_mask = RoleMask::from_indices(registry, changed);
    hash_u64(receipt, failures);
    receipt.update(std::as_bytes(std::span<const std::uint8_t>(accepted)));
    receipt.update(std::as_bytes(std::span<const std::uint8_t>(conflict)));
    // This C++ receipt binds the exact encoded proposal bytes. Its digest can
    // distinguish +0 from -0 even when both leave changed_mask clear; the
    // author's numeric dirty test supplies the mask, not this hash identity.
    receipt.update(std::span<const std::byte>(encoded));
    hash_u64(receipt, changed_mask.role_count());
    for (const auto word : changed_mask.words()) hash_u64(receipt, word);
    const Digest256 digest(receipt.finish());
    if (changed.empty()) {
        ArbitrationOutcome out;
        out.receipt = digest;  // explicit no-commit, no candidate
        return out;
    }
    ArbitrationResult result(state.generation(), current_step, std::move(changed_mask),
                             result_type, W, std::move(encoded), std::move(accepted),
                             std::move(conflict), digest,
                             P == 1 ? std::optional<Digest256>(proposals.front().binding_receipt())
                                    : std::nullopt);
    ArbitrationOutcome out;
    out.receipt = digest;
    out.candidate.emplace(std::move(result));
    return out;
}

}  // namespace swegca::vrs
