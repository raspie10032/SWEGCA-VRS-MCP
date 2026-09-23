#pragma once

#include "swegca_vrs/allocation.hpp"
#include "swegca_vrs/authority.hpp"
#include "swegca_vrs/cognitive_state.hpp"
#include "swegca_vrs/gate_rules.hpp"
#include "swegca_vrs/identity_types.hpp"
#include "swegca_vrs/native_tensor.hpp"
#include "swegca_vrs/proposal_arbiter.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

// Main's guarded verification-slot writer (Stage 6). The class itself is
// defined in authority_roles.hpp, beside MainOwner, so every translation unit
// that can name SuccessorStateKey or ConsumeKey<cognitive_state_commit> sees
// its one definition. This header holds its configuration and results.
// Writer source: the user's implementation of 2026-08-25
// (tinylm-slicer-sanabi-bazzite mosaic_bounded_world_write.py, last changed
// in 7e4ae52 and unchanged at the pinned 3bddcb7), which binds the gate
// capability to the exact proposal and checks that authority before a dry
// run. The 2026-08-20 text (SWEGCA-Architecture src/swegca@5901a5a) is the
// older one. The writer returns an immutable successor and a receipt; it never
// replaces Main's current state. Publishing a successor is Main's step.
namespace swegca::vrs {

// The author's one configuration: the gate's thresholds and the dedicated
// preview's bounds. Main builds the EvidenceGate from `gate` and this writer
// from the rest, so a guarded write is judged and previewed under the same
// configuration.
// Lineage: direct — BoundedWorldWriteConfig and its validation.
// SWEGCA: src/tinylm_slicer/mosaic_bounded_world_write.py@3bddcb7:34-55
struct BoundedWriteConfig final {
    GatePolicy gate;
    double maximum_slot_delta = 0.02;
    double minimum_proposal_weight = 0.5;
};

// What a refused guarded write reports beyond the gate's own failure bits
// (the gate uses bits up to 1 << 27).
enum BoundedWriteFailure : std::uint32_t {
    // Lineage: direct — "proposal_weight": the dedicated preview did not accept it.
    // SWEGCA: src/tinylm_slicer/mosaic_bounded_world_write.py@3bddcb7:407-408
    bounded_write_proposal_weight = 1u << 28,
    // Lineage: native mechanism — Main's journal HEAD moved after the gate
    // judged this state (evidence_gate.cpp: "The guarded writer must recheck at
    // commit because HEAD may advance later"); the author has no journal.
    // SWEGCA: paper/swegca/ARCHITECTURE_SPEC.md@5901a5a:141-150
    bounded_write_journal_stale = 1u << 29,
    // Lineage: native mechanism — the C++ arbiter reports refusals as failure
    // bits and a no-commit receipt instead of raising.
    // SWEGCA: src/tinylm_slicer/mosaic_bounded_world_write.py@3bddcb7:405-412
    bounded_write_preview_refused = 1u << 30,
};

enum class BoundedWriteStatus : std::uint8_t {
    refused = 1,             // the author's `reason` names a failed gate
    authorized_dry_run = 2,  // "authorized_dry_run"
    committed = 3,           // "committed"
};

// Every field of the author's receipt, the two it added on 2026-08-25, and
// the C++ audit fields the architecture inventory asks of a state-write
// receipt (decision, binding, previews, generations, authority domain).
// A receipt is data, as the author's is: rollback and retraction trust it
// only as far as the state's hashes agree with it.
// Lineage: direct — BoundedWorldWriteReceipt (08-25).
// SWEGCA: src/tinylm_slicer/mosaic_bounded_world_write.py@3bddcb7:216-230
struct BoundedWriteReceipt final {
    using Bytes = std::vector<std::byte, AllocationAdapter<std::byte>>;

    Digest256 receipt_id;
    std::uint64_t revision;
    RoleId target_role;
    Digest256 before_state_hash;
    Digest256 after_state_hash;
    // The verification slot before the write, [1, slot_width] in its stored
    // scalar type, little-endian.
    ScalarType before_slot_type;
    std::uint64_t slot_width;
    Bytes before_slot;
    Digest256 before_slot_hash;
    // The author's after_slot_hash: the sum before_slot + local_delta in its
    // promoted scalar type (:427, :466).
    Digest256 after_slot_hash;
    // The slot as stored, cast back to the state's scalar type. It equals
    // after_slot_hash when no promotion occurred. When one did (a float16
    // state with a float32 delta or weight), the author's rollback and
    // retraction compare the stored slot with after_slot_hash and always
    // refuse; here they compare with this hash, so a promoted write can be
    // undone. A documented divergence from the Python behaviour.
    Digest256 stored_slot_hash;
    Digest256 applied_delta_hash;
    EvidenceReferences evidence_references;
    std::optional<BoundedWriteHead> prior_write_head;
    // 08-25: hypothesis_id and proposal_binding_digest.
    ClaimRevision claim;
    Digest256 proposal_digest;
    // C++ audit fields.
    StateGeneration before_generation;
    StateGeneration after_generation;
    Digest256 decision_digest;
    Digest256 binding_digest;
    Digest256 binding_receipt;
    Digest256 preview_receipt;
    AuthorityDomain authority_domain = AuthorityDomain::cognitive_state_commit;
};

// Lineage: direct — BoundedWorldWriteResult: the state (the successor, or the
// same state when nothing was written), whether it was authorized and
// committed, the reason, the proposed delta, and the receipt of a commit.
// SWEGCA: src/tinylm_slicer/mosaic_bounded_world_write.py@3bddcb7:233-240
struct BoundedWriteResult final {
    std::shared_ptr<const CognitiveState> state;
    bool authorized = false;
    bool committed = false;
    BoundedWriteStatus status = BoundedWriteStatus::refused;
    std::uint32_t failures = 0;  // gate bits | BoundedWriteFailure bits
    // The dedicated preview's receipt, including a no-commit verdict, and its
    // candidate (the author's proposed_delta) when the arbiter produced one.
    Digest256 preview_receipt{Digest256::Bytes{}};
    std::optional<ArbitrationResult> preview;
    std::optional<BoundedWriteReceipt> receipt;
};

}  // namespace swegca::vrs
