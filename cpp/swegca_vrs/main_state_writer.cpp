#include "swegca_vrs/main_state_writer.hpp"

#include "swegca_vrs/arbiter_kernel.hpp"
#include "swegca_vrs/core_sha256.hpp"
#include "swegca_vrs/evidence_gate.hpp"
#include "swegca_vrs/experience.hpp"
#include "swegca_vrs/scalar_codec.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <limits>
#include <new>
#include <span>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>

namespace swegca::vrs {

// Main's writer keeps references to Main's journal and ledger (Main owns all
// three) and its own arbiter under the dedicated bounded-write policy.
struct detail::MainStateWriterState final {
    // SWEGCA: src/tinylm_slicer/mosaic_bounded_world_write.py@3bddcb7:399-405
    MainStateWriterState(const ExperienceJournal& journal, MainAuthorityLedger& ledger,
                         AllocationContext memory, BoundedWriteConfig config,
                         std::shared_ptr<const ProposalArbiter> arbiter)
        : journal(&journal), ledger(&ledger), memory(std::move(memory)),
          config(config), gate_policy(gate_policy_digest(config.gate)),
          arbiter(std::move(arbiter)) {}
    const ExperienceJournal* journal;
    MainAuthorityLedger* ledger;
    AllocationContext memory;
    BoundedWriteConfig config;
    // The gate thresholds this writer's configuration names; the capability
    // must have been issued under the same ones.
    Digest256 gate_policy;
    std::shared_ptr<const ProposalArbiter> arbiter;
};

namespace {

using Bytes = BoundedWriteReceipt::Bytes;

// The target is fixed by the architecture, not configurable.
constexpr std::string_view verification_role = "verification";
constexpr std::string_view write_policy_version = "bounded-verification-v1";

// Lineage: direct — BoundedWorldWriteConfig.__post_init__: the bounds are
// finite within [0, 1], the slot delta is positive, the diversity minima are
// positive (make_gate_rules checks the gate's part).
// SWEGCA: src/tinylm_slicer/mosaic_bounded_world_write.py@3bddcb7:42-54
void validate_config(const BoundedWriteConfig& config) {
    (void)make_gate_rules(config.gate);
    const auto unit = [](double value) {
        return std::isfinite(value) && value >= 0 && value <= 1;
    };
    if (!unit(config.maximum_slot_delta) || config.maximum_slot_delta <= 0)
        throw std::invalid_argument("bounded_write_config_invalid:maximum_slot_delta");
    if (!unit(config.minimum_proposal_weight))
        throw std::invalid_argument("bounded_write_config_invalid:minimum_proposal_weight");
}

// Lineage: direct — the writer's own arbiter: maximum_world_delta equals
// maximum_slot_delta, minimum_weight is minimum_proposal_weight.
// SWEGCA: src/tinylm_slicer/mosaic_bounded_world_write.py@3bddcb7:400-404
ArbiterPolicy bounded_preview_policy(const BoundedWriteConfig& config) {
    return ArbiterPolicy{config.maximum_slot_delta, config.maximum_slot_delta,
                         config.minimum_proposal_weight};
}

// Lineage: direct — str(tensor.dtype) as slot_tensor_hash spells it.
// SWEGCA: src/tinylm_slicer/mosaic_cognitive_slot_memory.py@3bddcb7:22-28
std::string_view torch_dtype_name(ScalarType type) {
    switch (type) {
        case ScalarType::bfloat16: return "torch.bfloat16";
        case ScalarType::float16: return "torch.float16";
        case ScalarType::float32: return "torch.float32";
        case ScalarType::float64: return "torch.float64";
    }
    throw std::invalid_argument("bounded_write_scalar_type_invalid");
}

// The author's slot_tensor_hash of a [1, width] slot: SHA-256 over the dtype
// name, the JSON shape "[1,<width>]" and the little-endian element bytes.
// Lineage: direct — the same preimage bytes. For bfloat16 the author's
// tensor.numpy() raises (numpy has no bfloat16) although its receipt codec
// lists bfloat16; this native form hashes the stored bits instead.
// SWEGCA: src/tinylm_slicer/mosaic_cognitive_slot_memory.py@3bddcb7:22-28
Digest256 slot_digest(ScalarType type, std::uint64_t width,
                      std::span<const std::byte> bytes) {
    Sha256 hash;
    hash.update(torch_dtype_name(type));
    std::array<char, 32> shape{};
    shape[0] = '[';
    shape[1] = '1';
    shape[2] = ',';
    const auto written = std::to_chars(shape.data() + 3, shape.data() + shape.size() - 1, width);
    if (written.ec != std::errc{})
        throw std::invalid_argument("bounded_write_slot_width_invalid");
    *written.ptr = ']';
    hash.update(std::string_view(shape.data(),
                                 static_cast<std::size_t>(written.ptr + 1 - shape.data())));
    hash.update(bytes);
    return Digest256(hash.finish());
}

// Length-prefixed text so no two field sequences hash the same bytes.
// SWEGCA: src/tinylm_slicer/mosaic_bounded_world_write.py@3bddcb7:433-445
void hash_text(Sha256& hash, std::string_view text) {
    const auto length = static_cast<std::uint64_t>(text.size());
    std::array<std::byte, 8> prefix{};
    for (std::size_t at = 0; at < prefix.size(); ++at)
        prefix[at] = static_cast<std::byte>((length >> (8 * at)) & 0xff);
    hash.update(prefix);
    hash.update(text);
}

// SWEGCA: src/tinylm_slicer/mosaic_bounded_world_write.py@3bddcb7:433-445
void hash_u64(Sha256& hash, std::uint64_t value) {
    std::array<std::byte, 8> bytes{};
    for (std::size_t at = 0; at < bytes.size(); ++at)
        bytes[at] = static_cast<std::byte>((value >> (8 * at)) & 0xff);
    hash.update(bytes);
}

// The receipt id over the author's seed: before-state hash, delta hash,
// revision, evidence references in proposal order, the claim (the author's
// hypothesis_id) and the proposal digest (its proposal_binding_digest).
// Lineage: weak analogy — the author hashes a sorted-key JSON of the seed;
// this is a native length-prefixed preimage over the same fields.
// SWEGCA: src/tinylm_slicer/mosaic_bounded_world_write.py@3bddcb7:433-445
Digest256 receipt_id(const Digest256& before_state, const Digest256& delta,
                     std::uint64_t revision, const EvidenceReferences& evidence,
                     const ClaimRevision& claim, const Digest256& proposal_digest) {
    Sha256 hash;
    hash_text(hash, "swegca.bounded_write_receipt_id.v1");
    hash.update(before_state.bytes());
    hash.update(delta.bytes());
    hash_u64(hash, revision);
    hash_u64(hash, evidence.size());
    for (const auto& address : evidence) hash_text(hash, address.value());
    hash_text(hash, claim.claim().value());
    hash_u64(hash, claim.revision());
    hash.update(proposal_digest.bytes());
    return Digest256(hash.finish());
}

struct VerificationRole final {
    const RoleDefinition* role;
    std::size_t index;  // registry index, the arbiter's row
};

// The registered verification role, which must lie in the scratch partition.
// Lineage: direct — topology.fixed_roles["verification"] and the partition
// check of _replace_verification_slot.
// SWEGCA: src/tinylm_slicer/mosaic_bounded_world_write.py@3bddcb7:364-376
VerificationRole find_verification(const CognitiveState& state) {
    const auto* role = state.roles().find(verification_role);
    if (role == nullptr)
        throw std::invalid_argument("bounded_write_verification_role_missing");
    if (role->partition != TensorPartition::scratch)
        throw std::invalid_argument("bounded_write_verification_not_in_scratch");
    const auto definitions = state.roles().definitions();
    return {role, static_cast<std::size_t>(role - definitions.data())};
}

// Lineage: direct — cognitive_slot_value: a detached copy of one slot of a
// batch-one state.
// SWEGCA: src/tinylm_slicer/mosaic_bounded_world_write.py@3bddcb7:424-425
Bytes slot_bytes(const AllocationContext& memory, const CognitiveTensor& tensor,
                 std::uint64_t slot) {
    const auto& shape = tensor.shape();
    if (shape.batches != 1 || slot >= shape.slots)
        throw std::invalid_argument("bounded_write_slot_invalid");
    const auto size = shape.width * scalar_width(tensor.scalar_type());
    Bytes bytes(static_cast<std::size_t>(size), std::byte{}, memory.allocator<std::byte>());
    tensor.copy_bytes(slot * size, bytes);
    return bytes;
}

// SWEGCA: src/tinylm_slicer/mosaic_bounded_world_write.py@3bddcb7:424-428
double widen(ScalarType type, std::span<const std::byte> element) {
    return type == ScalarType::float64 ? read_scalar64(type, element)
                                       : static_cast<double>(read_scalar32(type, element));
}

// torch's elementwise conversion between the four stored types: a widening
// is exact, float64 narrows through binary32 (c10::Half and c10::BFloat16
// convert from float), and binary32 narrows once, ties to even.
// Lineage: native mechanism — torch's copy_ into a tensor of another dtype,
// which the author's `.to(dtype=...)` and slot assignment perform.
// SWEGCA: src/tinylm_slicer/mosaic_bounded_world_write.py@3bddcb7:364-376
Bytes cast_slot(const AllocationContext& memory, ScalarType from,
                std::span<const std::byte> bytes, ScalarType to) {
    const auto from_width = scalar_width(from);
    const auto to_width = scalar_width(to);
    if (bytes.size() % from_width != 0)
        throw std::invalid_argument("bounded_write_slot_byte_count_invalid");
    const auto count = bytes.size() / from_width;
    Bytes result(count * to_width, std::byte{}, memory.allocator<std::byte>());
    for (std::size_t at = 0; at < count; ++at) {
        const auto source = bytes.subspan(at * from_width, from_width);
        const auto target = std::span<std::byte>(result).subspan(at * to_width, to_width);
        if (from == to) {
            std::copy(source.begin(), source.end(), target.begin());
        } else if (to == ScalarType::float64) {
            write_scalar64(to, widen(from, source), target);
        } else if (from == ScalarType::float64) {
            write_scalar32(to, static_cast<float>(read_scalar64(from, source)), target);
        } else {
            write_scalar32(to, read_scalar32(from, source), target);
        }
    }
    return result;
}

struct SlotSum final {
    ScalarType type;
    Bytes bytes;
};

// after_slot = before_slot + local_delta as torch computes it: the result
// type is the promotion of the two, float64 adds in binary64, every other
// result adds in binary32 (the CPU opmath of half and bfloat16) and rounds
// once to the result type.
// Lineage: direct — the author's tensor addition.
// SWEGCA: src/tinylm_slicer/mosaic_bounded_world_write.py@3bddcb7:426-427
SlotSum add_slots(const AllocationContext& memory, ScalarType before_type,
                  std::span<const std::byte> before, ScalarType delta_type,
                  std::span<const std::byte> delta, std::uint64_t width) {
    const auto type = kernel::arbiter_promote(before_type, delta_type);
    const auto before_width = scalar_width(before_type);
    const auto delta_width = scalar_width(delta_type);
    const auto result_width = scalar_width(type);
    if (before.size() != width * before_width || delta.size() != width * delta_width)
        throw std::invalid_argument("bounded_write_slot_byte_count_invalid");
    Bytes result(static_cast<std::size_t>(width * result_width), std::byte{},
                 memory.allocator<std::byte>());
    for (std::uint64_t at = 0; at < width; ++at) {
        const auto left = before.subspan(at * before_width, before_width);
        const auto right = delta.subspan(at * delta_width, delta_width);
        const auto target = std::span<std::byte>(result).subspan(at * result_width, result_width);
        if (type == ScalarType::float64) {
            write_scalar64(type, widen(before_type, left) + widen(delta_type, right), target);
        } else {
            const float sum = read_scalar32(before_type, left) + read_scalar32(delta_type, right);
            write_scalar32(type, sum, target);
        }
    }
    return {type, std::move(result)};
}

// The author's `replace(state, scratch_slots=..., self_state=...)`: every
// other field of the prior state is kept as it is.
// Lineage: direct — dataclasses.replace over the prior state.
// SWEGCA: src/tinylm_slicer/mosaic_bounded_world_write.py@3bddcb7:446-456
std::shared_ptr<const CognitiveState> successor_state(
    SuccessorStateKey key, const AllocationContext& memory,
    const CognitiveState& prior, CognitiveTensor scratch, SelfState self) {
    const auto evidence = prior.evidence_references();
    return std::allocate_shared<CognitiveState>(
        memory.allocator<CognitiveState>(), std::move(key), memory, prior,
        RoleRegistry(prior.roles()), CognitiveTensor(prior.semantic()),
        CognitiveTensor(prior.executive()), std::move(scratch),
        StructuredWorldGraph(prior.world_graph()),
        EvidenceReferences(evidence.begin(), evidence.end(),
                           memory.allocator<ExperienceAddress>()),
        GoalState(prior.goals()), ValueState(prior.values()), std::move(self));
}

// The result keeps the preview's candidate (the author's proposed_delta).
// ArbitrationResult moves but does not assign, so it is emplaced.
// SWEGCA: src/tinylm_slicer/mosaic_bounded_world_write.py@3bddcb7:405-412
void keep_preview(BoundedWriteResult& result, ArbitrationOutcome& outcome) {
    if (outcome.candidate) result.preview.emplace(std::move(*outcome.candidate));
}

// Lineage: direct — self_state with the bounded-write key set to the prior
// head, or removed when there was none.
// SWEGCA: src/tinylm_slicer/mosaic_bounded_world_write.py@3bddcb7:488-492
SelfState self_with_head(const CognitiveState& state,
                         const std::optional<BoundedWriteHead>& head) {
    CanonicalPayload payload(state.self().payload());
    if (head) return SelfState(std::move(payload), *head);
    return SelfState(std::move(payload));
}

}  // namespace

// Only Main constructs its writer; the arbiter's private construction right
// is exercised here, as MainOwner does for its ledger.
// SWEGCA: src/tinylm_slicer/mosaic_bounded_world_write.py@3bddcb7:399-405
MainStateWriter::MainStateWriter(const ExperienceJournal& journal, MainAuthorityLedger& ledger,
                                 const AllocationContext& memory,
                                 const BoundedWriteConfig& config) {
    validate_config(config);
    auto arbiter_allocator = memory.allocator<ProposalArbiter>();
    auto* raw_arbiter = arbiter_allocator.allocate(1);
    try {
        ::new (static_cast<void*>(raw_arbiter))
            ProposalArbiter(memory, bounded_preview_policy(config));
    } catch (...) {
        arbiter_allocator.deallocate(raw_arbiter, 1);
        throw;
    }
    auto arbiter = std::shared_ptr<const ProposalArbiter>(
        raw_arbiter,
        [arbiter_allocator](ProposalArbiter* value) mutable noexcept {
            value->~ProposalArbiter();
            arbiter_allocator.deallocate(value, 1);
        }, memory.allocator<ProposalArbiter>());
    state_ = std::allocate_shared<detail::MainStateWriterState>(
        memory.allocator<detail::MainStateWriterState>(), journal, ledger, memory, config,
        std::move(arbiter));
}

// SWEGCA: src/tinylm_slicer/mosaic_bounded_world_write.py@3bddcb7:379-387
MainStateWriter::~MainStateWriter() = default;

// The dedicated preview the gate must judge: the one proposal under the
// writer's bounded policy, no commit.
// Lineage: direct — the writer's own SingleWorldArbiter(..., commit=False).
// SWEGCA: src/tinylm_slicer/mosaic_bounded_world_write.py@3bddcb7:400-406
ArbitrationOutcome MainStateWriter::preview(const StateSnapshot& snapshot,
                                            std::uint64_t current_step,
                                            const BoundProposal& bound) const {
    return state_->arbiter->arbitrate(snapshot, current_step,
                                      std::span<const BoundProposal>(&bound, 1));
}

// One guarded verification write, in the order of the user's 2026-08-25
// writer: shape and target checks raise; a failed gate or an unaccepted
// preview returns a refusal; authority for exactly this proposal and preview
// is checked before a dry run; only a commit consumes it. The state and its
// publication come from one Main snapshot, so they cannot be torn apart: an
// exact rollback restores equal content under a new publication, and content
// alone cannot tell the two heads apart.
// SWEGCA: src/tinylm_slicer/mosaic_bounded_world_write.py@3bddcb7:379-475
BoundedWriteResult MainStateWriter::write(const StateSnapshot& snapshot,
                                          const BoundProposal& bound, GateOutcome& gate,
                                          std::uint64_t current_step, bool commit) {
    const auto state = snapshot.share_state();
    if (!state) throw std::invalid_argument("bounded_write_state_missing");
    const auto& current = *state;
    const auto& published = snapshot.head();
    const auto& memory = state_->memory;
    // :388-389 one batch-one state.
    if (current.semantic().shape().batches != 1)
        throw std::invalid_argument("bounded_write_requires_batch_one_state");
    // :390 proposal.validate(state): made against this published state and
    // its roles.
    const auto& proposal = bound.proposal();
    if (proposal.based_on() != published ||
        !proposal.targets().matches(current.roles()))
        throw std::invalid_argument("bounded_write_proposal_not_for_state");
    // :391-398 the proposal targets the verification role alone.
    const auto verification = find_verification(current);
    if (proposal.targets().selected_count() != 1 ||
        !proposal.targets().test(verification.index))
        throw std::invalid_argument("bounded_write_target_not_verification");

    // :399-412 the gate's reason, then the dedicated preview.
    auto outcome = preview(snapshot, current_step, bound);
    BoundedWriteResult result;
    result.state = state;
    result.preview_receipt = outcome.receipt;
    std::uint32_t failures = gate.failures;
    const bool accepted = outcome.failures == 0 && outcome.candidate &&
                          outcome.candidate->accepted().size() == 1 &&
                          outcome.candidate->accepted()[0] != 0;
    if (failures == 0 && outcome.failures != 0) failures |= bounded_write_preview_refused;
    if (failures == 0 && !accepted) failures |= bounded_write_proposal_weight;
    if (failures != 0) {
        result.failures = failures;
        keep_preview(result, outcome);
        return result;
    }
    const auto& candidate = *outcome.candidate;

    // :413-419 authentic authority for exactly this proposal, preview and
    // published state, issued by this Main's ledger, still live; checked, not
    // spent.
    // The operation also names the gate policy, recomputed from this
    // writer's configuration: a gate built with other thresholds (the
    // author's _authorization_reason(gates, config) uses the one config)
    // issued an operation that cannot match, and the write is refused.
    if (!gate.authority) throw std::invalid_argument("bounded_write_authority_missing");
    const auto operation = verification_commit_operation(
        bound.decision_digest(), bound.binding_digest(), bound.binding_receipt(),
        candidate.receipt(), verification.role->id.value(), current.roles().digest(),
        state_->gate_policy, published);
    state_->ledger->verify(ConsumeKey<AuthorityDomain::cognitive_state_commit>{},
                           *gate.authority, published, operation);
    if (gate.authority->descriptor().owner != current.owner())
        throw std::invalid_argument("bounded_write_authority_owner_mismatch");
    // Main's journal HEAD must still name the published state the gate
    // judged: its content and its publication both.
    if (!published.matches(state_->journal->state_head())) {
        result.failures = bounded_write_journal_stale;
        keep_preview(result, outcome);
        return result;
    }

    // :420-423 dry run.
    if (!commit) {
        result.authorized = true;
        result.status = BoundedWriteStatus::authorized_dry_run;
        keep_preview(result, outcome);
        return result;
    }

    // :424-428 the new verification slot, in the state's own scalar type.
    const auto& scratch = current.scratch();
    const auto width = scratch.shape().width;
    const auto state_type = scratch.scalar_type();
    const auto before = slot_bytes(memory, scratch, verification.role->slot);
    const auto delta_type = candidate.scalar_type();
    const auto delta_row = width * scalar_width(delta_type);
    const auto rows = candidate.role_delta();
    if (candidate.width() != width || rows.size() != current.roles().size() * delta_row)
        throw std::invalid_argument("bounded_write_preview_shape_mismatch");
    const auto delta = rows.subspan(verification.index * delta_row, delta_row);
    const auto after = add_slots(memory, state_type, before, delta_type, delta, width);
    const auto stored = cast_slot(memory, after.type, after.bytes, state_type);
    auto next_scratch = scratch.with_replaced_slot(memory, verification.role->slot, stored);

    // :429-432 the revision follows the prior head.
    const auto& prior = current.self().write_head();
    std::uint64_t revision = 1;
    if (prior) {
        if (prior->revision == std::numeric_limits<std::uint64_t>::max())
            throw std::overflow_error("bounded_write_revision_exhausted");
        revision = prior->revision + 1;
    }

    // :433-445 the receipt seed and id.
    const auto before_hash = current.content_digest();
    const auto delta_hash = slot_digest(delta_type, width, delta);
    EvidenceReferences evidence(memory.allocator<ExperienceAddress>());
    evidence.reserve(proposal.evidence_addresses().size());
    for (const auto& address : proposal.evidence_addresses())
        evidence.emplace_back(memory, std::string_view(address));
    const ClaimRevision claim(proposal.claim());
    const auto proposal_digest = proposal_content_digest(proposal);
    const auto id = receipt_id(before_hash, delta_hash, revision, evidence, claim,
                               proposal_digest);

    // :446-457 the successor carries the new head.
    BoundedWriteHead head{PolicyVersion(memory, write_policy_version), id, revision,
                          RoleId(memory, verification_role), evidence, claim,
                          proposal_digest};
    CanonicalPayload payload(current.self().payload());
    auto successor = successor_state(SuccessorStateKey{}, memory, current,
                                     std::move(next_scratch),
                                     SelfState(std::move(payload), std::move(head)));

    // :458-472 the receipt.
    BoundedWriteReceipt receipt{
        id, revision, RoleId(memory, verification_role), before_hash,
        successor->content_digest(), state_type, width, before,
        slot_digest(state_type, width, before), slot_digest(after.type, width, after.bytes),
        slot_digest(state_type, width, stored),
        delta_hash, evidence, prior, claim, proposal_digest,
        published, bound.decision_digest(),
        bound.binding_digest(), bound.binding_receipt(), candidate.receipt(),
        AuthorityDomain::cognitive_state_commit};

    // :473-475 the committed result, complete before authority is spent.
    result.state = std::move(successor);
    result.authorized = true;
    result.committed = true;
    result.status = BoundedWriteStatus::committed;
    keep_preview(result, outcome);
    result.receipt.emplace(std::move(receipt));

    // Spend the capability last: nothing above has changed Main, and a
    // failure before this line leaves the capability unspent. Nothing after
    // it can throw: the reset and the return move are noexcept.
    state_->ledger->consume(ConsumeKey<AuthorityDomain::cognitive_state_commit>{},
                            std::move(*gate.authority), published, operation);
    gate.authority.reset();
    return result;
}

static_assert(std::is_nothrow_move_constructible_v<BoundedWriteResult>,
              "a committed result must leave write() without a throwing move");

// Exact rollback of the current state to the receipt's before-state.
// Lineage: direct — rollback_bounded_verification_write.
// SWEGCA: src/tinylm_slicer/mosaic_bounded_world_write.py@3bddcb7:478-496
std::shared_ptr<const CognitiveState> MainStateWriter::rollback(
    const CognitiveState& state, const BoundedWriteReceipt& receipt) const {
    const auto& memory = state_->memory;
    if (receipt.target_role.value() != verification_role)
        throw std::invalid_argument("bounded_write_receipt_target_invalid");
    if (state.content_digest() != receipt.after_state_hash)
        throw std::invalid_argument("bounded_write_rollback_stale");
    const auto verification = find_verification(state);
    const auto& scratch = state.scratch();
    const auto width = scratch.shape().width;
    const auto current = slot_bytes(memory, scratch, verification.role->slot);
    // The stored slot, not the promoted sum (BoundedWriteReceipt::stored_slot_hash).
    if (slot_digest(scratch.scalar_type(), width, current) != receipt.stored_slot_hash)
        throw std::invalid_argument("bounded_write_slot_differs_from_receipt");
    if (receipt.slot_width != width)
        throw std::invalid_argument("bounded_write_receipt_slot_shape_invalid");
    const auto restored = cast_slot(memory, receipt.before_slot_type, receipt.before_slot,
                                    scratch.scalar_type());
    auto successor = successor_state(
        SuccessorStateKey{}, memory, state,
        scratch.with_replaced_slot(memory, verification.role->slot, restored),
        self_with_head(state, receipt.prior_write_head));
    if (successor->content_digest() != receipt.before_state_hash)
        throw std::invalid_argument("bounded_write_rollback_not_bit_exact");
    return successor;
}

// The receipt still owns the current verification-slot head.
// Lineage: direct — validate_bounded_verification_retraction.
// SWEGCA: src/tinylm_slicer/mosaic_bounded_world_write.py@3bddcb7:499-518
void MainStateWriter::validate_retraction(const CognitiveState& state,
                                          const BoundedWriteReceipt& receipt) const {
    if (receipt.target_role.value() != verification_role)
        throw std::invalid_argument("bounded_write_receipt_target_invalid");
    const auto& head = state.self().write_head();
    if (!head) throw std::invalid_argument("bounded_write_receipt_not_active");
    if (head->receipt_id != receipt.receipt_id || head->revision != receipt.revision ||
        head->target_role != receipt.target_role)
        throw std::invalid_argument("bounded_write_receipt_not_lifo_head");
    const auto verification = find_verification(state);
    const auto& scratch = state.scratch();
    const auto current = slot_bytes(state_->memory, scratch, verification.role->slot);
    if (slot_digest(scratch.scalar_type(), scratch.shape().width, current) !=
        receipt.stored_slot_hash)
        throw std::invalid_argument("bounded_write_slot_newer_or_unrelated");
}

// Retracts the current write and keeps every later change to the rest of
// the state (goals, values, graph and the other slots).
// Lineage: direct — retract_bounded_verification_write.
// SWEGCA: src/tinylm_slicer/mosaic_bounded_world_write.py@3bddcb7:521-541
std::shared_ptr<const CognitiveState> MainStateWriter::retract(
    const CognitiveState& state, const BoundedWriteReceipt& receipt) const {
    validate_retraction(state, receipt);
    const auto& memory = state_->memory;
    const auto verification = find_verification(state);
    const auto& scratch = state.scratch();
    const auto before = cast_slot(memory, receipt.before_slot_type, receipt.before_slot,
                                  scratch.scalar_type());
    if (slot_digest(scratch.scalar_type(), scratch.shape().width, before) !=
        receipt.before_slot_hash)
        throw std::invalid_argument("bounded_write_receipt_before_slot_mismatch");
    return successor_state(
        SuccessorStateKey{}, memory, state,
        scratch.with_replaced_slot(memory, verification.role->slot, before),
        self_with_head(state, receipt.prior_write_head));
}

}  // namespace swegca::vrs
