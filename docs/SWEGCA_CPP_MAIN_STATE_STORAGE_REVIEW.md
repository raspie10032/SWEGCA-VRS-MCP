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
- `SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md` §9 requires Main's
  selected committed receipt as the recovery authority, bounded immutable
  linked segments, exact digests, and a manifest naming the state generation.
  The journal's local HEAD cannot override the generation named by that
  receipt. Derived views remain rebuildable.
- `MainOwner` currently constructs generation 0 with a computed state digest.
  A new `JournalStore` starts with a zero state digest in its genesis HEAD.
  There is no C++ state or transaction record codec or state recovery.
- `JournalStore::stage` now rejects reserved state kinds 5–7, while
  `stage_state_records` requires a key only Main can form. This closes the
  generic staging route; Main does not yet call the state route or encode
  those records.
- `JournalStore::open_at_root` can open and verify the exact manifest that a
  Main-supplied root names without adopting the lower journal's HEAD. It
  retains and charges bytes outside that generation and is read-only. Main's
  append-only committed receipt writer, restart selection, and safe write
  resumption after such a selection are not implemented.
- User correction (2026-09-23 18:3x): state content has no separate
  generation number. Recording time stays outside the state as a human
  timeline hint. The current `StateGeneration::ordinal` and manifest
  `state_generation_ordinal` are therefore provisional code to replace.
- The journal limits one record payload to 16 MiB and one generation to
  64 MiB. An initial state may exceed both limits. The experience module
  already streams large blobs through content-addressed 8 MiB parts and a
  bounded-depth digest tree. Its level count and upper digest-list tree are
  now shared in `part_tree.hpp`; the state level-0 part writer and reader do
  not yet exist.
- `JournalStore::stage_from` now shares unchanged immutable extent-index
  paths and copies only changed paths when staging a generation. Its storage
  charge uses checked deltas for changed extents, manifest bytes, and view
  pages. Checkpoint manifest encoding still enumerates every extent by design;
  ordinary staging no longer copies the full extent map.
- The earlier C++ `CognitiveTensor` owned one contiguous byte vector.
  Candidate `462a6f7` replaces it with immutable shared chunks, but the
  writer and large-state startup paths are not connected yet. A disk part
  tree alone would not solve the in-memory copy.
- `MainInitialState::TensorInput` now accepts either a full borrowed span or
  a borrowed `TensorByteReader` consumed into validated 8 MiB chunks. The
  stream path avoids holding caller input and a second whole tensor at once.
  It checks EOF after the expected byte count, matching the span path's exact
  length rule. The borrowed reader must honestly report bytes it writes.
  Main's cold journal recovery has not been connected to this reader yet.
- The user's earlier `mosaic_world_memory_transaction.py` prepares a transaction
  for semantic-memory promotion linked to an existing World-write receipt.
  It does not define a required transaction around state-part staging.
- The user's `mosaic_vrs_event_durable.py@3bddcb7:3-5,152` restores only a
  digest selected by Main and writes no mutable current pointer.
  `mosaic_paper_resident_assimilation.py@3bddcb7:491-535` creates a new
  exclusive pending receipt, fsyncs it, swaps Main's in-memory owner pair,
  renames that receipt to committed, and fsyncs its directory. If rename or
  directory fsync fails after the swap, the new pair remains live for reads;
  `_require_commit_ready` (`:447-449`) blocks later writes until the marker
  is reconciled. Its receipt names the previous and replacement pair IDs;
  the earlier author code does not verify a restart predecessor chain. The
  native C++ receipt must additionally bind its exact journal manifest and
  state publication; its restart selection rule remains undecided.
- The user's earlier bounded writer keeps prior write metadata in `self_state`.
  C++ candidate `SelfState` now holds an opaque caller payload alongside an
  optional typed `BoundedWriteHead` (policy version, receipt digest, revision,
  target role, evidence references, claim and proposal digest). The content
  digest uses domain v4 and binds that
  head's presence and canonical fields. The writer and recovery codec still
  need to update and restore it; the opaque payload's reserved-key boundary
  must be checked before calling this complete. Caller self payload bytes
  cannot represent the reserved write head; only the typed field does.
- User clarification (2026-09-23 18:37 KST): a remembered occurrence and
  its immutable source record are memory; the numerical synapse strength
  linking memories is experience. The old hybrid organizer persists
  `vrs_strengths.f16` and passes it into the next generation as
  `initial_vrs_strengths`. The later canonicalization path at
  `mosaic_vrs_canonicalization.py@3bddcb7:319-400` requires and emits f32.
  Its strength history is not reconstructible from memory records alone.
- The user's later `mosaic_memory_activation.py@3bddcb7:462-488` keeps a
  memory index and VRS snapshot as a Main-owned atomic pair. Its
  `replay_memory` (`:725-746`) does not rank by strength. Strength-ordered
  Replay and the five-way tie cap are user instructions from 2026-09-23;
  they are not attributed to that earlier Replay function.

## Required contract

1. Main holds one current state and one journal owner. Before Bind or any
   guarded write, its state content digest and latest state-head publication
   record identity must exactly match the journal generation named by Main's
   selected committed receipt and its verified native state root. Content
   identity and publication order remain separate.
   Re-evidence and the accumulator judge against the content digest; proposal
   `based_on` and publication compare-and-swap use the publication identity.
   If bit-exact rollback restores earlier content, a judgment about that
   content can be current again, subject to the accumulator's own revision
   and new-observation checks.
2. Only a fresh genesis with no committed Main receipt and zero state digest
   can be initialized from the one `MainInitialState` supplied to Main.
   Initialization publishes that state's real content root and publication
   record before any state-changing operation. An existing committed receipt
   is recovered from its named root and is never replaced by caller input.
3. Recovery reads the committed-receipt-selected root by exact address and verifies its
   root record, latest publication record position/digest, state content
   digest, bounded parts, and reconstructed canonical state. It fails closed
   on missing or mismatched data. Cold recovery may read the state bytes;
   Déjà vu through Recall must not do this work.
4. A guarded successor checks Main's selected committed receipt, the current
   lower journal HEAD, and the prior state; it stages only
   changed bounded state parts, then stages the successor root and write
   receipt. The receipt includes before/after state hashes, before/after
   verification-slot hashes, applied-delta hash, target role and revision,
   proposal evidence references, and prior bounded-write metadata. The
   exact decision/proposal/evidence Bind must be checked separately: merely
   having a receipt does not establish it. The native writer cannot publish
   until its currently opaque `SelfState` has a defined way to bind and
   restore bounded-write metadata. The final lower journal HEAD names the
   candidate successor; a new committed Main receipt makes that selection
   durable authority. Following the author's commit order, Main writes and
   fsyncs a pending receipt, swaps its in-memory owner pair by compare-and-swap,
   renames the new receipt to its committed name, then fsyncs the directory.
   A failed final rename or directory fsync leaves a pending or unconfirmed
   committed marker. It keeps the replacement pair live for reads and blocks
   further writes until reconciliation, following the author's commit path.
5. Intermediate part publication, if necessary to respect the 64 MiB
   generation limit, keeps the prior state-head publication identity. Part
   records confer no decision or write authority. A crash before the final
   Main commit receipt leaves the prior state durably selected; a retry may
   verify and reuse already written parts.
   The final stage must be built on the latest journal HEAD while comparing
   the state-head publication identity with the one read before preparation,
   since unrelated experience appends may advance the journal generation.
   A lower journal publication failure before Main's owner swap leaves the
   prior pair current. The native C++ path takes a stricter rule than the
   author's pending-receipt code: it stops guarded work until the lower
   candidate is reconciled, because the lower HEAD may have advanced. A
   failed pending-receipt write also leaves the prior pair current; the
   author's code retains and blocks on a partial marker only if a pending
   file actually exists (`mosaic_paper_resident_assimilation.py@3bddcb7:507-512`).
   After Main's owner swap, a failed marker rename or directory fsync does
   not roll the live pair back: reads keep using it while further writes are
   refused. After restart, recovery considers a receipt visible under its
   committed name and verifies its native chain and named content.
6. Initialization, cold recovery, and guarded writes must fit the VRS host's
   configured memory profile and preserve the canonical state content byte
   stream. Its content digest excludes generation ordinal, as the user's earlier
   bounded writer's bit-exact rollback check requires. The host VRS layer
   counts memory and judges its configured limit; the SWEGCA code uses its
   injected allocator.
7. State part, root, and receipt records use reserved nonzero kinds with zero
   experience-search index entries. Experience decoding rejects those kinds,
   so they cannot be treated as recalled experience or admitted evidence.
8. A still-reversible write receipt retains its prior state root and parts
   for bit-exact rollback while that receipt can be the current linked head.
   Reclamation must preserve this reachability before freeing orphan parts.
9. Main also owns the current VRS synapse-strength data as first-class
   persistent experience. Its published values and lineage must survive
   recovery from Main's selected committed receipt; they are not a rebuildable
   search view of memory records. The strength root is separate from
   `CognitiveState`, as in the user's later implementation. The user's current
   rule updates the live session VRS immediately, without waiting for a
   queued worker. The proposed
   C++ publication invariant is to expose new session memory and its live VRS
   effect together, so a read cannot see one without the other; a read lease
   would bind their published generation. At session end the live VRS becomes
   a block with connections. Selected blocks are merged and processed by VRS
   during idle time. The exact logical block and connection records remain
   to be derived from the
   user's architecture. A physical COW byte block in
   `mosaic_vrs_block_store.py` is a storage unit, not by itself this logical
   session block.
   New C++ strength persistence and computation use f32, matching the user's
   later canonicalization path. The old f16 artifact is source history, not a
   compatibility format or a per-edge rounding rule.
10. Any detached VRS proposal, including idle block merging, carries the
    source generation and parent VRS identity it read. Main alone checks
    the proposal's declared source set and its exact coverage, including
    duplicate inputs, and confirms that the parent is still current before
    publishing. Detached work cannot delay the live-session VRS update.
    Memory-record positions are not `PublishedStateId`:
    that type names a CognitiveState publication. Raw cue hit counts grant no
    strength mutation authority. The user's `refine_vrs` stability bit is
    a geometry test, not a SWEGCA three-state decision; their exact interface
    remains open, including abstention.

## Candidate representation for review

### C++ identity boundary agreed with Claude (2026-09-23 18:36 KST)

- The target `CognitiveState` holds only canonical content and its
  `content_digest()`, with no publication ordinal, wall clock, or journal
  position. The current C++ class still holds a provisional
  `StateGeneration(ordinal, digest)`. Main creates the initial content; a
  guarded writer creates successor content.
- `PublishedStateId` is `(content_digest, publication RecordPosition)`, where
  the position has segment, byte offset, sequence, and record digest. Only a
  successful Main-owned state-head publication may construct it. The latest
  publication record changes on every transition, including a bit-exact
  rollback that reuses an older content root.
- `StateSnapshot` must hold a `shared_ptr<const CognitiveState>` and that
  exact `PublishedStateId`; only Main constructs snapshots. The current C++
  `StateSnapshot` holds the state and Main lifetime but has no `head()` or
  publication field yet. The target interface exposes `state()` and `head()`;
  a producer cannot construct or replace a head identifier.
- The journal now has a private `for_each_index_match_in` over a caller-pinned
  `PublishedSnapshot`; `resolve_in` and `replay_in` already accept that same
  snapshot. Main can therefore keep one journal generation through all cue
  lookups and exact reads. Main's combined memory/VRS strength read lease is
  still pending and must bind that journal snapshot to the strength root
  named by the same Main committed receipt.
- Proposal `based_on`, Bind, arbitration, gated capabilities, and CAS compare
  the publication identifier. Re-evidence, admission, and accumulator
  `judged_against` compare only the content digest. Replay can inspect
  `StateSnapshot::state()` to judge the current content.
- The receipt carries display time outside `CognitiveState`. Neither that
  time nor a new ordinal determines succession or authority. The user's earlier
  bounded writer's self-state write revision remains content, since rollback
  must restore it. A typed `BoundedWriteHead` in `SelfState` is included in
  content-digest domain v4; its writer and recovery path are still pending.
- The current lower-journal candidate uses two local HEAD publications
  because its encoder needs a record position before it can encode a
  manifest naming that position. First, Main publishes root and candidate
  state-write records while retaining the prior state head. It verifies the
  state-write record and stages a record-free final generation from the
  latest lower HEAD, comparing that HEAD's state publication identity to the
  prior one. Only the final lower HEAD names the new state content digest
  and publication position; it is still not Main's durable commit. An
  intervening experience append can advance the journal generation without
  changing the state identity, so the final stage is rebuilt from the latest
  lower HEAD. Main then follows the append-only pending/committed receipt
  protocol above. No staged object manufactures a published `StateSnapshot`.
  Main-owned recovery must use `open_at_root`, never standalone `open`, so
  the lower HEAD cannot expose either uncommitted candidate as Main's state.
  Failure before Main's owner swap keeps the prior pair current; a later
  marker failure keeps the replacement pair live for reads and blocks writes
  until reconciliation.
- A candidate receipt in a published record is not itself a committed-write
  receipt. Recovery, rollback eligibility, and audit count it only if a
  Main committed receipt selects the final manifest that names its exact
  record position and digest. An orphan after a crash before that Main
  commit has no write authority, even if the local journal HEAD advanced.
  Immutable records carry no mutable `committed` flag. This two-local-HEAD
  layout is a C++ journal mechanism under review, while the append-only
  Main receipt follows the author's commit order.

This is an interface contract, not a claim that state publication or the
four-stage VRS path is already implemented.

### Derived address-view rebuild and record validation

- `JournalStore::rebuild_view` currently checks the journal record chain and
  builds the address and index trees, but it does not decode experience,
  part, or cue-binding payloads. A recovered view cannot be called fully
  validated on that basis alone.
- The first bounded pass builds an unpublished exact-address tree from the
  verified record chain. Once its pages are flushed, a second bounded pass
  may ask each record's Main-owned decoder to validate its kind and payload
  before the lower journal HEAD publishes the replacement view. That second
  pass is cold recovery work outside the input-to-Recall latency budget.
- A cue binding that carries a target `RecordPosition` must prove that the
  rebuilt address tree resolves its target address to that same position.
  Reading bytes at the supplied position alone does not establish that it
  is the position in the verified chain. The target record must also have
  the expected address and kind, and precede the binding in record order.
- `RebuildValidator` and its borrowed `RebuildReader` now provide the second
  pass with exact resolve and replay from the unpublished tree. A binding
  decoder can compare its stored target `RecordPosition` with the formal
  address view before reading the target. Main-owned decoders for
  each record kind still need to be connected; without one, no caller may
  rebuild a view. A callback failure leaves the previous HEAD authoritative
  and removes unpublished rebuild logs. The callback must use the borrowed
  reader and must not reenter JournalStore publication while its lock is held.
  The borrowed adapters cannot be copied or moved and must not outlive their
  callable. The
  validator must have no external side effects: record visitors run before
  the segment's trailing chain digest is checked. The second pass releases
  its first-pass collector buffers and runs before decoding, but it still
  reads one whole published segment into an accounted buffer of at most
  64 MiB. Its memory bound is therefore not merely page depth plus one
  record; streaming segment verification remains an open implementation task.

- Reserve native state-part, state-root, and state-publication record kinds
  distinct from experience kinds 1–3 and cue binding kind 4. Reuse the
  existing bounded part-tree
  *mechanism*; do not inherit an old VRS ranking or reinforcement policy.
  These state records carry no experience search index entries, and the
  experience decoder must reject their kinds. Kinds 5–7 are reserved, the
  experience decoder rejects them, and generic `JournalStore::stage` now
  rejects them too. `stage_state_records` requires a Main-only key. The
  state payload codec and Main-owned writer remain to be implemented before
  these records can be published as a state transition.
- Derive the state-root exact address from the state digest with a reserved
  prefix. The manifest must name the content digest and the latest state-head
  publication record position/digest, so recovery can use the same verified
  address tree without a memory-size scan. Main is
  already a `JournalStore` friend and can use one held snapshot with its
  private `resolve_in` and `read_in`; an `ExperienceAddress` wrapper is wrong
  for a state record.
- The state content digest excludes publication identity and recording time.
  A bit-exact rollback may reuse the former content root at a new state-head
  publication record.
  Keep write receipts separate from the state root. The root binds canonical
  content while receipts record write lineage. Each publication record must
  be new even when its content root is reused, so compare-and-swap can
  distinguish rollback from the earlier occurrence of that content.
- Encode enough canonical state fields and typed tensor-part references in
  the root to reconstruct and recompute the existing state digest exactly.
  A root payload that exceeds one record must itself use bounded parts.
- Keep state parts immutable and content addressed. A single-slot write
  may use copy-on-write immutable chunks, changing only those intersecting
  the slot while the root links unchanged chunks. Use a tree per tensor:
  the canonical stream emits the role list before tensor bytes, so appending
  a role shifts the 8 MiB boundaries of a whole-stream tree. Tensor chunks
  are already 8 MiB, and the current public constructors make only the final
  chunk short. With a live immutable state snapshot, the level-0 digest list
  can consume each borrowed tensor chunk directly. A separate state address
  prefix and record kind prevent
  identical experience/state part bytes from colliding in the journal;
  raw SHA-256 part digests can stay shared. Recompute the canonical state
  content digest from the root's reconstructed stream rather than assuming
  a tree digest equals it. A bounded stream supplies large initial tensors
  without simultaneous whole-tensor copies. Hashing the canonical stream
  may still take time on a write, but it does not belong to the
  input-to-Recall latency budget.
- A zeroed tensor may repeat the same 8 MiB chunk throughout its level-0
  digest list; shared chunks can also recur across tensors or generations.
  Preserve every digest in each ordered list, but stage a content address at
  most once per generation and resolve already published state parts before
  staging. Otherwise duplicate-address checks would reject a valid tensor.
- `CognitiveState::for_each_content_chunk` and `content_digest()` now use one
  canonical byte emitter. Its optional section callback now marks prefix,
  each tensor, the start of its chunks, entities, relations, evidence and final
  fields without adding bytes to the digest preimage. A future bounded
  state-part writer can consume
  exactly that preimage. The experience `plan_blob` pulls from a span
  or random-access reader and rereads it when staging; the state emitter
  pushes chunks into a sink. Share only the input-independent upper digest
  tree and level-count logic. The tensor's existing fixed chunks provide
  its level-0 parts; bounded push splitting is needed for metadata sections.
  State reads use Main's held snapshot with `resolve_in`
  and `read_in`, rather than experience `replay_part`. This emitter alone
  does not persist or recover state parts, and its borrowed sink must finish
  each chunk before returning.
  A writer failure may stop the stream mid-part; no partial part or state HEAD
  may publish, and unpublished bytes must be removed before guarded work resumes.
- The current canonical v4 byte order is prefix (domain, owner, roles), then
  each of three tensors' partition and header followed by its chunks, then
  suffix (graph, evidence, goals, values, self and write head). A state tensor
  tree root can hold its partition/header and ordered top digest list, with
  lower digest lists in bounded parts when needed;
  unchanged tensors keep their exact root addresses. A zero-byte tensor has a
  root with an empty level-0 list. A small tensor uses the same form, with no
  tensor-specific inline exception. The writer must keep the immutable Main
  state snapshot alive through every borrowed chunk and prove that prefix,
  each reconstructed tensor and suffix concatenate to the existing v4 digest
  preimage. A tensor root contains only partition, header and content digest
  lists: generation, owner, time and predecessor addresses stay outside it,
  so an unchanged tensor retains its exact root address. The canonical emitter
  now reports section boundaries while retaining one byte-encoding
  implementation. Each tensor's `tensor_chunks` marker follows its header,
  including when it has zero chunks, so a writer need not infer the header
  length or chunk start from write calls. The writer is not yet connected.
- Prefix and suffix may themselves exceed one record and need bounded parts.
  Splitting suffix into stable sections could avoid rewriting an unbounded
  graph when the write head changes. One section-boundary callback in the
  canonical emitter can name prefix, each tensor, entities, relations,
  evidence and final fields. Which sections become separate trees and how
  their roots are referenced remain format decisions. The writer must not
  introduce a second serializer or content-defined split.

## Native record graph cross-check (2026-09-24, candidate)

Claude's record graph proposal uses the three already reserved Main-only kinds:
kind 5 for immutable state parts and bounded section descriptors, kind 6 for a
fixed-size state-content root, and kind 7 for each state-head publication.
The canonical v4 emitter has eight ordered sections: prefix, three tensors,
entities, relations, evidence, and final fields. A descriptor per section
would let the root link unchanged sections while recovery reconstructs the
exact existing v4 stream and recomputes `content_digest`. This is a storage
proposal, not a new state or VRS decision rule. The exact payload codec and
non-tensor inline representation remain undecided.

- Each tensor descriptor must retain the emitter's header and an ordered
  chunk-digest list. The level-0 list is empty for a zero-byte tensor; a small
  tensor still uses that list and has no tensor-specific inline exception.
  Part splitting is at fixed 8 MiB chunk boundaries. A changed verification
  slot can reuse every unaffected chunk and descriptor. The non-tensor
  sections may need bounded 8 MiB parts as they grow; an inline rule for
  those sections still requires an explicit format decision.
- A kind-6 root is content-addressed as `state-root:<content_digest>` and
  links the eight section descriptors in canonical order. This fixed-size
  root can be reused when rollback returns to bit-identical content.
  Recovery must verify every referenced kind, address and digest, then hash
  the reconstructed v4 stream; a tree digest alone does not prove the state
  digest.
- A kind-7 publication is separate from the reusable root and must include
  predecessor publication identity, root address, content digest and its
  transition body. Its address must hash the **entire canonical payload**:
  hashing only predecessor and body would collide for distinct genesis
  contents. A rollback or retraction publishes a new identity even when it
  reuses an existing root. A bounded-write body needs all fields of the
  `BoundedWriteReceipt`; variable fields must have a bounded part form so a
  large receipt never exceeds one 16 MiB record. The predecessor is Main's
  last selected committed publication, never a newer orphan in the lower
  journal. If a retry encounters an already published kind-6 root or kind-7
  candidate, it resolves and verifies that record's kind and full canonical
  payload before reusing its exact position; the journal refuses staging a
  duplicate address. Whether a selected-root writable resumption retains
  such orphan records remains the open resumption decision below.
- Retraction cannot be verified by asserting that its result equals the
  referenced receipt's `before_state_hash`: it preserves later unrelated
  cognition. Cold recovery must reproduce the author's active-receipt and
  slot checks on the predecessor and verify the resulting content, or use an
  equivalent exact proof. Strict rollback has different preconditions and
  does restore the receipt's exact before-state hash.
- Publishing parts across several intermediate generations and then a root
  and publication record is possible only if each encoded `RecordDraft`
  satisfies its required source, source revision and operation ID, each
  record fits 16 MiB, and each segment **including headers and record
  framing** and each generation fit 64 MiB. No empty-budget assumption can
  replace those encoded-size checks. Intermediate lower HEADs confer no Main
  state authority; only Main's committed marker selects a final manifest.
- The final record-free generation cannot yet bind the new kind-7 identity:
  `ManifestFields` still has a provisional state ordinal and content digest,
  but no publication `RecordPosition` and digest. That manifest format, its
  codec and recovery checks must change before state publication code can
  use this graph. The Main marker then has to bind that exact final
  `JournalRoot`, publication identity, content digest and strength root.
  The migration also reaches `stage_from`, `stage_state_records`, `stage_view`,
  `state_generation` and `replay_at_head`, which currently pass or reconstruct
  the ordinal-bearing `StateGeneration`; changing only the manifest bytes
  would leave the public snapshot and Replay bound to the old identity.

The predecessor chain, writable resumption after `open_at_root`, orphan
genesis, pending-marker restart selection, part reclamation and strength-root
format remain open. The author's pending → fsync → in-memory swap → rename →
directory-fsync order governs the Main marker; a candidate lower HEAD never
overrides the last selected committed marker. Source boundaries:
`CognitiveState::for_each_content_chunk` and `part_tree.hpp` define the native
stream/part limits; `JournalStore::stage_state_records` and `ManifestFields`
define the current lower-journal mechanism; the author's
`mosaic_bounded_world_write.py@3bddcb7:478-541` distinguishes rollback from
retraction, and `mosaic_paper_resident_assimilation.py@3bddcb7:491-535`
supplies the Main commit ordering.

### Publication-identity migration scope (2026-09-23 audit)

Replacing the two manifest state fields alone is insufficient. The current
`StateGeneration(ordinal, digest)` crosses eighteen C++ files. The migration
must preserve each existing freshness check while moving the ordinal out of
content and using the exact Main-published `RecordPosition` where publication
order matters:

- `CognitiveState` computes content and currently constructs the provisional
  ordinal. The separate `PublishedStateId` type exists, but `StateSnapshot`
  does not yet hold it; Main must add and fill that slot only after a verified
  publication. Initial content has no publication identity until Main
  publishes genesis.
- Proposals, arbitration, gate capabilities and the guarded writer currently
  bind `based_on` or a capability to `CognitiveState::generation()`. Their
  compare-and-swap boundary must use the Main snapshot's publication ID;
  comparing only a content digest would admit an old occurrence after a
  bit-exact rollback.
- Re-evidence results and accumulator coverage currently carry or compare
  `StateGeneration`. Their claim-relative judgment remains tied to the
  canonical content digest, with the journal replay and the Main snapshot
  obtained under the same selected head. No provisional ordinal may survive
  inside an evidence digest merely because it is present in today's code.
- `JournalStore`, `ExperienceAppend`, `ExperienceJournal`, `EvidenceAdmission`,
  `EvidenceGate` and `MainStateWriter` currently pass or reconstruct that
  type. Experience appends and view rewrites keep the selected state-head
  identity unchanged; only Main's final state-publication generation may
  advance it. An initial journal has zero content digest and no publication.

This map records the work needed to uphold the existing checks; it does not
claim the migration or a cold-recovery path has been implemented.

As of 2026-09-24, the isolated `codex/state-identity-integration` branch has
removed `StateGeneration` from the C++ tree and migrated the manifest,
snapshots, proposal/arbiter, evidence, journal, gate, authority and guarded
writer interfaces to content digest plus the exact Main publication. Static
lineage and diff checks passed. This branch is still incomplete: Main does not
publish genesis, construct a verified `PublishedStateId`, persist the state
record graph or marker, or recover the selected state. `MainOwner::snapshot()`
therefore refuses its as-yet-unpublished initial state. Build and product
tests remain stopped under the architecture inventory's §10 gate.

## Decisions before implementation

- Enumerate every Main-owned state-changing transition before fixing the
  kind-7 body variants and their cold-recovery checks. Retraction preserves
  later unrelated cognition; the records that published those later changes
  cannot be silently omitted from the predecessor chain.
  Current C++ construction routes cover genesis content plus the guarded
  writer's bounded write, strict rollback and retraction; `cognition_runner`
  produces proposals without changing Main. The author's
  `mosaic_autonomous_cognition.py@5901a5a:170-186` also changes goal/self
  content, while `mosaic_cognitive_slot_memory.py@5901a5a:226-266` changes
  slots/self metadata. The architecture inventory §3A/E grants successor
  construction only to MainStateWriter. Those source operations have no
  reconciled Main publication and authority route in this rebuild. Until
  that boundary is specified, a kind-7 decoder must reject unknown bodies;
  it cannot treat an arbitrary state-content change as a verified transition.
- Confirm that intermediate part HEADs with an unchanged state generation
  preserve §9's rule that one Main committed receipt exposes a state successor.
- Reconcile the reserved kind-7 name `state_publication_record_kind` with
  this review's state-write-receipt term before defining its payload or
  publication semantics.
- Define exact state-root and receipt payloads and how a root larger than
  16 MiB is parted without a second unrelated codec.
- Decide where the canonical stream's non-tensor prefix (domain, owner and
  appendable role list), each tensor's scalar/shape/length header, and the
  later graph/payload sections live. If any are embedded in the root, define
  how that root is parted when they grow past one record while preserving
  the per-tensor copy-on-write boundaries.
- Define the inline and empty-tensor representation. `part_levels(0)` gives
  a zero level-0 count and depth 1, while experience stores small blobs
  inline; a state tensor codec must state which rule it uses before writing.
  Decision for the isolated native state codec (2026-09-24): use the same
  fixed 8 MiB part-tree rule for every section, including an empty section;
  there is no inline exception. `tensor_chunks` is a marker inside each of
  the three tensor sections, separating its fixed header from the ordered
  chunk digest list. A section descriptor and its address must be a
  deterministic function of those canonical bytes, so a retry of identical
  content can verify and reuse the same root. Descriptors, digest lists and
  non-tensor sections over one part are split by the existing part-tree
  rule; each decoder checks declared lengths and level counts before reads.
- Define restart behavior when genesis or intermediate HEAD still has a
  zero state digest, including what initial input may be accepted.
- Verify that all tensor readers and digest users can read a chunked
  canonical stream without constructing a full contiguous copy.
- Connect the existing bounded `TensorByteReader` startup input to cold
  journal recovery and confirm that its producer can stream without keeping
  a second full tensor copy. Keep the same canonical validation as the span
  input route.
- List crash points around part publication, final lower HEAD publication,
  pending Main receipt fsync, Main owner compare-and-swap, committed receipt
  rename, and directory fsync; assert the selected durable state is always
  recoverable and a pending or unconfirmed committed marker blocks further
  writes without rolling back an already swapped live pair.
- Decide how to reclaim published parts that no state root uses. The
  semantic-memory `prepared` protocol does not itself free their storage
  and is not a source-backed prerequisite for staging state parts.
- Define the native typed bounded-write metadata and its state-digest
  binding before implementing receipts, rollback, or retraction.
- Move publication identity out of immutable `CognitiveState` into Main's
  published-state snapshot boundary. Replace the separate ordinal with the
  exact state-head publication record position/digest. Keep recording time
  only in external transition metadata, never in content hash or CAS.
- Published snapshots now share an immutable ordinal extent index. An
  ordinary staged append copies only the changed tail/new ordinal paths;
  the host allocator accounts for each node and control block. Recovery still
  validates the manifest chain in a mutable table and builds one immutable
  index before readers observe it. The index caches checked record bytes,
  so `universe()` does not scan every extent. Checkpoints still deliberately
  list all extents, but the encoder pulls them from the index and the touched
  overlay without an extra `all` vector. It retains one complete bounded
  manifest output buffer, its digest, and decode verification. Disk charging
  retains checked incremental accounting and a full cold-recovery recount.

This storage path is outside the hot Déjà vu → Recall path. It cannot be used
as a substitute for the SWEGCA-based four-stage VRS navigation and judgment.
