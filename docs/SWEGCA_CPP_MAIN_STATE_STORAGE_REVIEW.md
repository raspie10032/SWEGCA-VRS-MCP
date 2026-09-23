# Main state storage review (2026-09-23)

Status: design candidate. It does not authorize a new VRS decision rule or
claim a working Main writer. Claude and Codex must cross-check the format and
crash cases before code uses it.

## Source and current-code facts

- SWEGCA `ARCHITECTURE_SPEC.md` §4.7–4.9 describes a bounded
  verification-slot commit, a receipt with before/after state and slot
  hashes plus copied evidence references, rollback/retraction, and recovery.
  Its §4.5 specifies the intended decision/proposal evidence binding and
  reports the old writer's E001/E002 gap. These sections describe the old
  local protocol, not a native byte format.
- `SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md` §9 requires one
  published HEAD as the recovery root, bounded immutable linked segments,
  exact digests, and a manifest naming the state generation. Derived views
  remain rebuildable.
- `MainOwner` currently constructs generation 0 with a computed state digest.
  A new `JournalStore` starts with a zero state digest in its genesis HEAD.
  There is no C++ state or transaction record codec or state recovery.
- The journal limits one record payload to 16 MiB and one generation to
  64 MiB. An initial state may exceed both limits. The experience module
  already streams large blobs through content-addressed 8 MiB parts and a
  bounded-depth digest tree.
- `CognitiveTensor` currently owns one contiguous byte vector. A disk part
  tree alone cannot prevent a verification-slot write from copying an entire
  large scratch tensor in memory.
- `MainInitialState::TensorInput` currently borrows one full byte span, and
  Main copies it into a tensor. A large caller-held initial tensor plus that
  copy can exceed a 4 GB VRS host profile before any write. Large
  initialization and cold recovery need bounded streaming inputs as well.
- The original `mosaic_world_memory_transaction.py` prepares a transaction
  for semantic-memory promotion linked to an existing World-write receipt.
  It does not define a required transaction around state-part staging.
- The original bounded writer keeps prior write metadata in `self_state`.
  Current C++ `SelfState` is an opaque `CanonicalPayload`, so the native
  writer still needs a typed, digest-bound way to update and restore that
  metadata without assuming an undocumented payload schema.

## Required contract

1. Main holds one current state and one journal owner. Before Bind or any
   guarded write, the current state generation must exactly equal the
   published HEAD's state generation and a verified native state root.
2. Only a fresh genesis HEAD with zero state digest can be initialized from
   the one `MainInitialState` supplied to Main. Initialization publishes that
   state's real generation and root before any state-changing operation. An
   existing nonzero HEAD is recovered from its own root and is never replaced
   by the caller's input.
3. Recovery reads the HEAD-selected root by exact address and verifies its
   record, ordinal, state digest, bounded parts, and reconstructed canonical
   state. It fails closed on missing or mismatched data. Cold recovery may
   read the state bytes; Déjà vu through Recall must not do this work.
4. A guarded successor checks the current HEAD and prior state, stages only
   changed bounded state parts, then stages the successor root and write
   receipt. The receipt includes before/after state hashes, before/after
   verification-slot hashes, applied-delta hash, target role and revision,
   proposal evidence references, and prior bounded-write metadata. The
   exact decision/proposal/evidence Bind must be checked separately: merely
   having a receipt does not establish it. The native writer cannot publish
   until its currently opaque `SelfState` has a defined way to bind and
   restore bounded-write metadata. Only the final Main-published HEAD names
   the successor state. Main swaps its current pointer only after that
   publication succeeds.
5. Intermediate part publication, if necessary to respect the 64 MiB
   generation limit, keeps the prior state generation. Part records confer no
   decision or write authority. A crash before the final HEAD leaves the
   prior state current; a retry may verify and reuse immutable parts.
   The final stage must be built on the latest journal HEAD while comparing
   the state generation with the one read before preparation, since unrelated
   experience appends may advance the journal generation.
   If publication throws, Main cannot infer the durable state from its old
   pointer because HEAD replacement may already have happened. It stops
   guarded work and reopens from the published HEAD before another write.
6. Initialization, cold recovery, and guarded writes must fit the VRS host's
   configured memory profile, preserve the canonical state byte stream, and
   compute the same `swegca.cognitive_state.v1` digest. The host VRS layer
   counts memory and judges its configured limit; the SWEGCA code uses its
   injected allocator.
7. State part, root, and receipt records use reserved nonzero kinds with zero
   experience-search index entries. Experience decoding rejects those kinds,
   so they cannot be treated as recalled experience or admitted evidence.
8. A still-reversible write receipt retains its prior state root and parts
   for bit-exact rollback while that receipt can be the current linked head.
   Reclamation must preserve this reachability before freeing orphan parts.

## Candidate representation for review

- Reserve native state-part, state-root, and state-write-receipt record kinds
  distinct from experience kinds 1–3. Reuse the existing bounded part-tree
  *mechanism*; do not inherit an old VRS ranking or reinforcement policy.
  These state records carry no experience search index entries, and the
  experience decoder must reject their kinds.
- Derive the state-root exact address from the state digest with a reserved
  prefix. The manifest already holds state ordinal and digest, so recovery
  can use the same verified address tree without a memory-size scan. Main is
  already a `JournalStore` friend and can use one held snapshot with its
  private `resolve_in` and `read_in`; an `ExperienceAddress` wrapper is wrong
  for a state record.
- The existing state digest includes its generation ordinal. A bit-exact
  rollback creates a new successor ordinal, so it does not reuse the former
  state digest or root address. Keep write receipts separate from the state
  root; the root binds canonical state fields, while the manifest names the
  current ordinal and the receipt records write lineage.
- Encode enough canonical state fields and typed tensor-part references in
  the root to reconstruct and recompute the existing state digest exactly.
  A root payload that exceeds one record must itself use bounded parts.
- Keep state parts immutable and content addressed. A single-slot write
  may use copy-on-write immutable chunks, changing only those intersecting
  the slot while the root links unchanged chunks. A bounded stream supplies
  large initial tensors without simultaneous whole-tensor copies. Hashing
  the canonical stream may still take time on a write, but it does not
  belong to the input-to-Recall latency budget.

## Decisions before implementation

- Confirm that intermediate part HEADs with an unchanged state generation
  preserve §9's rule that one final HEAD exposes a state successor.
- Define exact state-root and receipt payloads and how a root larger than
  16 MiB is parted without a second unrelated codec.
- Define restart behavior when genesis or intermediate HEAD still has a
  zero state digest, including what initial input may be accepted.
- Verify that all tensor readers and digest users can read a chunked
  canonical stream without constructing a full contiguous copy.
- Replace the whole-tensor startup input for the large-state route with a
  bounded stream, so its producer and Main never require simultaneous full
  copies. Use the same canonical validation as the small input route.
- List crash points around part publication, final HEAD publication, and
  Main pointer swap; assert the published state is always recoverable.
- Decide how to reclaim published parts that no state root uses. The
  semantic-memory `prepared` protocol does not itself free their storage
  and is not a source-backed prerequisite for staging state parts.
- Define the native typed bounded-write metadata and its state-digest
  binding before implementing receipts, rollback, or retraction.

This storage path is outside the hot Déjà vu → Recall path. It cannot be used
as a substitute for the SWEGCA-based four-stage VRS navigation and judgment.
