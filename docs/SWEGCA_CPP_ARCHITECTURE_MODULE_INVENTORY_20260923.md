# SWEGCA C++ Architecture Reconstruction Board

## Absolute rule

1. Do not translate or reproduce the existing system in C++.
2. Implement the SWEGCA architecture itself from the beginning in C++.
3. Implement every derived subsystem and every later behavior on top of that
   C++ SWEGCA architecture.
4. Existing code is evidence about concepts, provenance, and defects only. Its
   APIs, module boundaries, processing order, and storage design are not the
   target architecture.
5. Do not introduce a parallel external logic path that bypasses a SWEGCA
   stage, ownership boundary, evidence judgment, or authority check.

### Nano-core execution boundary

The SWEGCA decision core is a small hardware-neutral nano-core. One decision has
an ns-scale target, and batches must retain data-parallel execution across CPU,
GPU, and NPU implementations. Hardware-specific acceleration may change the
executor but cannot change the decision rule or its exact inputs and outputs.

To preserve that boundary, a nano-core translation unit contains only
deterministic operations over fixed-width numeric, digest, and enum data. It has
no allocation, lock, exception, I/O, string, virtual call, or global mutable
state. Batch interfaces use structure-of-arrays views so CPU SIMD and 16-worker
execution do not require a different decision contract. GPU and NPU executors
can implement the same contract later.

Journaling, identity text, receipt assembly, and authority nonce tracking are
outside the nano-core. A gate computes its authorization predicate in the pure
nano-core first; only an accepted result reaches the domain-specific issue key
and process-local authority ledger. The ledger is never on the decision
kernel's inner path.

## 1. Governing decision

This is an architecture reconstruction, not a Python behavior port.

The 15 files under `SWEGCA-Architecture` commit
`5901a5aa2dcbd0ac7ad12ac6dd745699f72288a8` are audited for concepts, data,
provenance, failure cases, and numerical rules. They are not the behavioral
oracle. Existing behavior that conflicts with the SWEGCA architecture must not
be copied into C++.

The implementation authority order is:

1. the user's explicit SWEGCA invariants;
2. `paper/swegca/ARCHITECTURE_SPEC.md` and `TERMINOLOGY.md` at `5901a5a`;
3. source elements that conform to those invariants;
4. new C++ mechanisms required to make the invariants executable.

The final rebuild tree has zero Python source files. libtorch, Python wheels,
prebuilt Python extension wheels, SQLite, sqlite3, FTS, FTS tokenizers, BM25,
and compatibility migration code are excluded.

The current VRS C++ layer remains frozen until the architecture below exists.
Only VRS parts that fit the rebuilt authority and storage graph can later be
reattached.

## 2. Executable invariants

The C++ type and call graph must make these conditions unavoidable:

1. Main owns exactly one persistent `CognitiveState`, identity, accumulated
   experience, evidence state, judgments, and every mutation capability.
2. A model, tool, specialist, manager, worker, emotion, utterance, or action
   intent can only produce an observation or transient proposal. It cannot own
   persistent state or issue authority.
3. Every complete experience remains addressable with source, revision,
   uncertainty, contradiction, outcome, and lineage. Retrieval status never
   grants belief or write authority.
4. Experience becomes evidence only for a named claim after claim-relative
   admission and re-evidence. Producer confidence alone never grants authority.
5. Evidence accumulation returns `accept`, `reject`, or `abstain`. Missing,
   stale, duplicate, source-concentrated, contradictory, or mismatched evidence
   fails closed.
6. A producer creates a proposal without receiving an authority decision. Main
   then binds its claim, evidence addresses, target state generation, exact
   target roles, and delta digest to the later accumulator decision before the
   proposal can enter arbitration.
7. Arbitration only returns a bounded aggregate proposal and conflict receipt.
   It has no commit API. Opposing accepted proposals for a shared role result in
   abstention/no commit for that role.
8. Only the Main-owned guarded writer may mutate `CognitiveState`. It consumes
   an authoritative evidence decision and the exact bound proposal, checks the
   current generation, and writes only authorized roles.
9. A persistent write creates an immutable receipt containing the complete
   evidence and decision binding, before/after state identity, target roles,
   delta identity, and prior head.
10. Semantic-memory promotion consumes that exact receipt. Promotion, rollback,
    retraction, and recovery are Main-owned native-journal transitions.
11. Experience selection and replay grant zero authority for World writes,
    memory promotion, actions, training, distribution, or P3 promotion.
12. External action authority remains a separate explicit Main decision after
    observation/evidence/judgment. A cognition phase or action intent is not the
    permission to execute.

## 3. Reconstructed C++ subsystems

The Python module boundaries are not retained as the architecture. The native
system is divided by ownership and authority.

### A. `architecture/state`

- Owns the canonical `CognitiveState`, role registry, slot storage, structured
  World graph, goal/value/self state, evidence references, and generation ID.
- Keeps one initial 32-role profile from `SLOT_ROLES` while making role identity
  string-addressable so capacity extension does not create a second state.
- Provides immutable read snapshots. Only `MainStateWriter` can construct a
  successor generation.
- Legacy `WorldState` and its compatibility view do not exist in the rebuilt
  runtime. Producers receive only the formal immutable `CognitiveState`
  snapshot.

### B. `architecture/experience`

- Owns the append-only original-experience journal and immutable generations.
- Stores complete records without success, verification, or file-type filters.
- Preserves raw bytes, canonical structured representation where available,
  stable address, source span, digest, time, outcome, uncertainty,
  contradiction, and revision lineage.
- Builds replaceable exact-address, source, cue, content-digest, namespace,
  resource, validity, and transaction views from the journal.
- `Select(q, U) -> (C, J, rho)` returns addresses and replay handles. `J` records
  every candidate's selection/rejection, relevance, contradiction, verification
  state, revision, rationale, and rejection evidence. It does not declare truth.

### C. `architecture/evidence`

- Defines claim identity, evidence observation, source/context/producer axes,
  re-evidence result, freshness, contradiction, and provenance binding.
- Accumulates only admitted evidence for one exact claim revision.
- Issues process-local authoritative decisions with visible canonical digests.
- Records rejected and abstained inputs without turning them into authority.
- Binds the decision to all evidence addresses and the claim revision used.

### D. `architecture/proposal`

- Creates transient proposals from detached state snapshots.
- Requires claim ID/revision, evidence addresses, target state generation,
  target-role set, bounded delta, source identity, confidence, contradiction,
  and uncertainty. It contains no evidence decision or authority capability.
- Rejects empty or mismatched evidence binding on every authority-bearing path.
- The Main evidence gate is the only constructor of `BoundProposal`, which pairs
  a proposal with its exact decision digest and successful `Bind` result.
- `ProposalArbiter` accepts only `BoundProposal` values, then validates, bounds,
  combines, and suppresses conflicts. It cannot write state.

### E. `architecture/authority`

- `MainStateWriter` is the only component with a state mutation capability.
- It checks evidence currentness, decision/proposal binding, state generation,
  definitions, counterfactual and intervention support, diversity, runtime
  context, device/storage domain, capacity, role mask, and norm bounds.
- It publishes a successor `CognitiveState` plus immutable write receipt, or an
  explicit no-commit receipt.
- The six authority domains are six distinct types:
  `CognitiveStateCommitAuthority`, `SemanticMemoryPromotionAuthority`,
  `ExternalActionAuthority`, `TrainingModelUpdateAuthority`,
  `DistributionAuthority`, and `P3PromotionAuthority`. None converts to another.

### F. `architecture/memory`

- Maintains episodic, quarantined, semantic, retracted, and superseded records
  as journaled states over original experience addresses.
- Semantic promotion requires the authoritative decision, exact state-write
  receipt, matching claim and evidence set, and current native journal heads.
- Validity intervals and revision chains are append-only. Historical versions
  remain replayable.

### G. `architecture/transaction`

- Journals state/memory stages with generation compare-and-swap and immutable
  transaction heads. Generation compare-and-swap is **re-created
  (user@2026-09-23)** to enforce the current-head requirements in SWEGCA
  §4.8-4.9 without SQLite transactions.
- Supports `prepared`, `memory_committed`, `state_committed`, `completed`,
  `rollback_pending`, and `rolled_back`. The last two native stage records are
  **re-created (user@2026-09-23)** to keep rollback distinct and replayable as
  required by SWEGCA §4.9.
- Recovery scans incomplete heads and appends compensation records. Compensation
  records are **re-created (user@2026-09-23)** to implement §4.9 recovery
  without deleting historical evidence or revisions.

### H. `architecture/cognition`

- Runs request-local producers over detached snapshots and joins every worker
  before return.
- Implements the observe/hypothesize/request evidence/collect/re-evidence/
  verify/remember/propose action/observe result flow as state-transition input,
  not as execution authority.
- Keeps real-time internal LLM calls at zero. A replaceable language model may
  only express Main-owned content.

The request-local producer runner is still missing. The author's
`run_dynamic_cognition` (`mosaic_synapse_arbiter.py@5901a5a:365-455`)
runs the first route member before optional fanout,
validates each proposal and its source identity, joins workers even when a
worker fails, checks that Main's state stayed unchanged, and returns the
proposals with a trace of operation type, executed cores, fanout decision,
elapsed time, primary weights, and false manager/worker retention flags.
It refuses an empty operation type or producer registry, a decisive threshold
outside [0, 1], an unknown operation route, an empty or duplicate route, and
any route entry absent from the registered immutable producer definitions.
The current C++ proposal is batch-one, so the author's all-batch decisive
check reduces to its one score; `fanout_used` records that additional route
members ran, not how many threads ran simultaneously. The VRS host owns any
CPU worker cap, including the headroom reserved for the Palworld server.
No numeric cap is fixed in SWEGCA.

The author's preview covers every executed proposal. The approved C++
authority sequence first binds each proposal to Main's evidence decision,
then passes only `BoundProposal` values to `ProposalArbiter` for the
multi-proposal no-commit preview. A failed Bind prevents that preview. The
elapsed-time trace must be finalized after the preview to cover the same
request interval as the author path. `MainOwner` does not yet own the
experience/evidence objects needed to connect these stages; a producer-run
intermediate result must not be presented as a completed cognition result.

## 4. C++ type, ownership, and lifetime design

The epistemic objects are different C++ types. They are never represented by a
shared untyped row whose meaning changes by convention.

- `Observation`: immutable time-indexed input with producer provenance. Before
  Main appends it, it has no persistent address. Appending gives it an address
  and still grants no authority.
- `ExperienceAddress`: digest-bound address issued only by the Main experience
  journal.
- `ExperienceRecord`: immutable original or derived experience plus source,
  revision, outcome, uncertainty, contradiction, and lineage.
- `SelectionReceipt`: candidate judgments `J` and selected/rejected addresses.
  Each judgment carries selection/rejection, relevance, contradiction,
  verification state, revision, rationale, and rejection evidence. Its
  authority type is statically `NoAuthority`.
- `ClaimRevision`: Main-owned identity of the exact hypothesis or mutation being
  judged.
- `EvidenceObservation`: an experience address admitted for one
  `ClaimRevision`, with support/refute/insufficient result and evidence axes.
- `EvidenceDecision`: constructible only by `EvidenceAccumulator`; it contains
  decision status, claim revision, accumulator revision, admitted and rejected
  address sets, the Main-supplied evaluated delta and mask digests, rule
  configuration digest, and visible decision digest.
- `StateSnapshot`: immutable detached bytes and generation identity. It exposes
  no writer or mutable reference.
- `SynapseProposal`: transient producer output bound to source, claim revision,
  nonempty evidence addresses, source state generation, role mask, delta digest,
  confidence, contradiction, and uncertainty. It contains no decision digest.
- `BoundProposal`: constructible only by the Main evidence gate after `Bind`;
  contains the immutable proposal, exact `EvidenceDecision` digest, and binding
  receipt. Only this type can be passed to `ProposalArbiter`.
- `ArbitrationReceipt`: bounded aggregate delta plus accepted, rejected,
  conflicted, and no-commit roles. It has no commit capability.
- `WriteAuthorization`: move-only capability constructible only by the evidence
  gate after exact `Bind` checks.
- `StateWriteReceipt`: immutable before/after state generation and hashes,
  claim, decision, evidence set, proposal/arbitration digests, role set, delta,
  prior head, and authority domain.
- `SemanticMemoryPromotionAuthority`: distinct move-only capability issued only
  by Main after checking a current `StateWriteReceipt` and semantic-promotion
  gates. The receipt is required evidence for the capability; it does not itself
  confer that authority.
- `ExternalActionAuthority`, `TrainingModelUpdateAuthority`,
  `DistributionAuthority`, and `P3PromotionAuthority`: unrelated capability
  types with no implicit conversion from reads, decisions, or write receipts.

`MainOwner` is noncopyable and owns the only mutable heads for Cognitive State,
experience, evidence accumulators, and transactions. The state-directory owner
lock prevents a second live Main for the same identity. `MainOwner` creates
short-lived `StateSnapshot` values and request scopes. Producer scopes own only
detached snapshots and terminate before their proposals are accepted. No
producer object, proposal, receipt, or read view holds a mutable Main pointer.
In this C++ design, detached means a leased `shared_ptr<const CognitiveState>`
captured before the request; the snapshot cannot follow a later Main head swap
and exposes no mutable state. Its immutable tensor chunks may be shared rather
than copied for every producer. This is the C++ counterpart of cloning mutable
Python tensors before a producer runs, not permission to retain the request
snapshot after the producer scope ends.
Every request destroys its producer instances and request-local identity state
after joining its workers; only explicitly registered immutable producer
definitions survive between requests.

Authority capability constructors are private to their issuing Main-owned
component. Capabilities are move-only, name one domain, one identity, one
generation, and one operation digest, and are consumed exactly once. Visible
JSON or binary fields alone cannot forge them.

Main records a fresh process-local one-use nonce when issuing a capability. A
consumer accepts the capability by rvalue reference, marks the nonce spent
before performing the authorized mutation, and rejects missing, unknown, or
spent nonces. Capabilities are never persisted. Every restart begins with zero
valid capability nonces, as required for process-local authority.

Issuance and consumption use separate domain-specific passkey types. Only the
named gate can construct an issue key, and only the matching writer or executor
can construct the consume key. A consumer supplies the current state generation
and actual operation digest; the ledger spends the nonce before rejecting a
generation or operation mismatch, so every retry requires a new gate decision.

### Native tensor value

`CognitiveTensor` is an owned, fixed-rank value with immutable bounded chunks,
explicit shape, and one of `bfloat16`, `float16`, `float32`, or `float64`.
`RoleMask` is a
separate boolean type. Tensor bytes, shape, dtype, and byte order are included
in canonical hashes. There is no autograd, model graph, optimizer, Python
device, or libtorch state. Retained SWEGCA equations use explicit typed
reductions and round-to-nearest-even conversions; other torch behavior is not
reproduced.

## 5. Required processing graph

```text
complete observation
  -> original experience append and stable address
  -> Select(q, U) -> (C, J, rho), with candidate judgments J and
     zero-authority receipt rho
  -> authority-limited transient producer proposal from detached state
  -> claim-relative evidence admission and re-evidence
  -> evidence accumulation: accept | reject | abstain
  -> exact decision/proposal/evidence/state-generation binding gate
  -> BoundProposal
  -> conflict-aware bounded arbitration accepting BoundProposal only,
     no commit capability
  -> Main guarded authorization
  -> CognitiveState successor + immutable write/no-write receipt
  -> receipt-bound episodic/quarantine/semantic transition
  -> native transaction publication or recovery/retraction
```

No edge may bypass experience addressing, evidence judgment, decision binding,
or Main authorization.

## 6. Failure-model closure

| SWEGCA failure | C++ enforcement |
|---|---|
| 1. Observation mistaken for authoritative state | `Observation` cannot construct `CognitiveState`; only journal append and the guarded writer issue persistent successors. |
| 2. Retrieved failed/unverified experience treated as permission | `SelectionReceipt` has the static `NoAuthority` type; retrieval returns addresses only. |
| 3. High-confidence producer bypasses evidence admission | `SynapseProposal` carries no write capability; the gate requires an authentic accepted `EvidenceDecision` and exact binding. |
| 4. Stale, duplicate, or source-concentrated evidence accepted | Accumulator generation, seen-address set, freshness, source/context/producer diversity, per-axis samples, and regime-change gates fail closed. |
| 5. Opposing proposals averaged into a residual | Arbiter marks shared negative-direction roles conflicted and zeros/no-commits those roles before whole-delta bounding. |
| 6. Decision for claim A authorizes delta for claim B | `ClaimRevision`, evidence set, decision digest, delta digest, mask digest, and state generation must match structurally in `Bind`. |
| 7. Proposal changes undeclared roles | `RoleMask` is validated before arbitration; masking occurs before norm calculation; writer verifies the same mask and writes only authorized roles. |
| 8. State write and semantic promotion tear | Promotion consumes the exact write receipt and a separate promotion capability inside the staged native transaction journal. |
| 9. Worker mutates shared state or retains identity | Workers receive owned detached snapshots and no Main pointer; every request joins workers and destroys producer instances and request-local identity state. |
| 10. Rollback/retraction after unrelated change | Rollback requires exact current after-state hash; retraction requires current linked head and preserves unrelated successors; recovery uses transaction-stage heads. |

## 7. Invariant enforcement map

| Invariant | Native enforcement point |
|---|---|
| I01 one authoritative persistent Cognitive State | noncopyable `MainOwner`, identity owner lock, private successor constructor, immutable snapshots |
| I02 producer output is not belief | separate `SynapseProposal`; arbiter has no commit API; producer types cannot construct state or authority |
| I03 state-changing evidence is traceable | nonempty bound evidence set, decision/proposal digest binding, `StateWriteReceipt` |
| I04 undefined remains insufficient | explicit insufficient evidence outcome and `abstain`; no prior-filled state mutation |
| I05 confidence is not authority | confidence is only one proposal weight factor; only accumulator/gate capabilities authorize |
| I06 missing critical evidence fails closed | evidence, definition, counterfactual, intervention, diversity, freshness, context, role, storage, and capacity checks |
| I07 persistent mutation is provenance-bound and reversible | complete write receipt plus native transaction, rollback, retraction, and recovery records |
| I08 producer identity is not truth rank | no privileged producer enum/order; diversity is recorded as a proxy and never an authority token |
| I10 capability differs from automatic authority | distinct nonconvertible move-only authority-domain types consumed by scoped writers |
| Experience-authority separation | `SelectionReceipt<NoAuthority>` and no conversion from read/replay handles to authority |

I09, I11, and I12 remain candidate invariants because the normative source does
not yet promote them. They cannot be silently invented as completed SWEGCA
rules.

## 8. Fifteen-module source audit and disposition

Each source range below is an audit input. `Keep` means preserve the
architecture concept. `Reshape` means implement it through the reconstructed
ownership graph. `Remove` means do not reproduce the incompatible behavior.

### L0

#### `mosaic_omni.py` (1-81)

- Symbols: `SLOT_ROLES` 16-32, `_WorldShape` 35-37,
  `SurfaceResidualRef` 40-49, `WorldState` 52-81.
- Keep: role ordering and surface residual provenance.
- Reshape: World tensor/mask validation into canonical state/view types.
- Remove: a separately owned persistent legacy `WorldState`.
- Torch audit: rank/shape and boolean-mask dtype checks only.

#### `mosaic_cognitive_kernel.py` (1-576)

- Symbols: JSON/text checks 19-29; evidence/event types 32-104; World entity,
  relation, and graph 107-172; state config 175-193; tensor payload 196-217;
  `CognitiveState` 220-304; registry/query/evidence/bundle types 307-442;
  sizing specifications and estimates 445-576.
- Keep: sole persistent state, owner ID, structured graph, provenance-bearing
  events, representations, physical evidence, and resource estimates.
- Reshape: state serialization around native tensor bytes and immutable
  generation IDs; all mutations through `MainStateWriter`.
- Remove: public construction paths that can masquerade as a published
  successor without a Main-issued receipt.
- Torch audit: bfloat16/float16/float32/float64 tensors; common rank, dtype, and
  storage domain; floating-only state; serialization and reconstruction.

#### `mosaic_evidence_revision.py` (1-190)

- Symbols: SHA-256 15-20; contract/verification 28-60; authority check 63-66;
  ledger load 69-77; verification 80-190.
- Keep: immutable artifact identities, contiguous revisions, report/ledger/state
  binding, metric consistency, authority issuance, and evidence addresses.
- Reshape: verify native journal generations and receipts directly. Imported
  JSON reports are evidence artifacts, not the primary authority store.
- Remove: file layout as an authority prerequisite.

#### `mosaic_sqlite_memory.py` (1-18)

- Source behavior: `_fts_query` 8-18 creates NFKC/casefold trigrams and an FTS
  query string.
- Remove completely: SQL query strings, FTS semantics, tokenizer compatibility,
  and this module boundary.
- Recreate: query input becomes a normal Main-owned SWEGCA read input; native
  cue/region navigation returns original experience addresses.

### L1

#### `mosaic_cognitive_slot_topology.py` (1-129)

- Symbols: topology/audit records 12-34; registered pools 37-46; topology
  construction 49-73; capacity audit 76-129.
- Keep: semantic roles, fixed roles, pool-local capacity, overflow visibility,
  lease/rotation/archive audit.
- Reshape: initial 32-role profile plus native string-addressable extension in
  the same `CognitiveState`; capacity failure returns no-commit evidence.
- Remove: treating the absence of an extension profile as an accepted steady
  state.

#### `mosaic_synapse_arbiter.py` (1-455)

- Symbols: slot view/snapshot 20-51; proposal/result/trace types 54-122;
  evidence proposal 125-178; sufficiency gate 181-216;
  `SingleWorldArbiter` 219-362; dynamic cognition 365-455.
- Keep: detached producer snapshots, finite bounded proposals, explicit target
  roles, weight calculation, per-role and whole-proposal bounds,
  distinct-source directional conflict suppression, joined request workers.
- Reshape: proposal fields gain exact claim/evidence/state-generation identity
  but no decision or authority. Main later binds the decision to the proposal's
  claim, evidence set, delta, and mask. Arbitration returns only a candidate and
  conflict/no-commit receipt.
- Remove: `commit=True`, direct `WorldState` mutation, and authority-bearing
  proposals with empty or unrelated evidence addresses.
- Parameter fact: `SingleWorldArbiter(nn.Module)` has no trainable parameters,
  buffers, random initialization, seed, checkpoint, or loaded weights. It stores
  only maximum slot delta, maximum world delta, and minimum weight scalars.
- Torch operations audited: concat, clone, finite/all/any, softmax, dtype tiny,
  log, sum, zero/one/full, boolean masking, where, stack, unsqueeze, vector norm,
  clamp, inner product, weighted reduction/division, flatten, absolute value,
  split, equality, and addition. Native arithmetic is implemented only for
  equations retained by the reconstructed arbiter.

#### `mosaic_external_memory.py` (1-350)

- Symbols: document/resource records 14-41; query compiler 44-54; storage class
  57-322; evidence-text projection 324-350.
- Keep: complete document/resource payloads, provenance, content digest,
  duplicate identity, exact listing, tombstone semantics, and bounded evidence
  text.
- Reshape: one Main-owned original-experience journal plus immutable derived
  address/resource/content/namespace views.
- Remove: database path/tokenizer configuration, connections, triggers, SQL
  schema, upsert-in-place, FTS, and BM25 ordering.

### L2

#### `mosaic_cognitive_slot_memory.py` (1-374)

- Symbols: exact slot hash 22-28; lease/request/plan 31-97; manager 100-145;
  planning 148-210; read/replace 213-266; archive/protect/apply 269-374.
- Keep: role-local leases, deterministic eviction, protected/referenced
  exclusion, required verified archive, stale-plan rejection, exact content
  identity, and revisioned manager metadata.
- Reshape: plans name an immutable state generation and can only be applied by
  `MainStateWriter`; archive references point to native experience addresses.
- Remove: any helper that can return a published state outside the guarded
  writer.
- Torch audit: contiguous exact bytes for hashing, clone, indexed read/write,
  and exact dtype/storage-domain agreement.

#### `mosaic_evidence_accumulator.py` (1-488)

- Symbols: capabilities 22-45; config/observation/group/axis/state/decision/
  update 48-258; Wilson interval 261-282; assessment 285-356; update 359-428;
  proposal gate 431-443; audit append 446-488.
- Keep: accept/reject/abstain, claim-scoped evidence, source/context/producer
  diversity, duplicate/expiry/insufficient rejection, axes, regime-change
  checks, immutable revision, and process-local authority.
- Reshape: decision contains claim ID/revision, exact admitted/rejected
  evidence-address sets, and the Main-supplied evaluated proposal delta/mask
  digests. Re-evidence is an explicit input stage. Audit records are native
  journal records. The Main gate checks exact decision binding after decision
  issuance and before arbitration.
- Remove: a boolean sufficiency mask as the only connection between evidence
  judgment and a semantic delta.
- Numeric audit: Beta posterior, Wilson interval, square root, and inverse
  standard-normal CDF. The chosen native formulas become their own documented
  numerical contract instead of depending on Python `NormalDist`.

### L3

#### `mosaic_versioned_memory.py` (1-407)

- Symbols: UTC/check 20-32; mutation/store 35-102; register/upsert 104-224;
  revalidation 226-255; expiry 257-304; rollback 306-360; search 362-407.
- Keep: accepted-decision requirement, validity interval, supersession, unique
  update identity, audit, due revalidation, rollback, and active-at-time read.
- Reshape: immutable native journal revision chains and compensating records.
- Remove: schema creation, SQL transactions, row mutation, and FTS candidate
  dependency.

#### `mosaic_autonomous_cognition.py` (1-410)

- Symbols: phase/event enums 15-38; config/event/transition 41-134; transition
  table/helpers 137-186; state machine 189-410.
- Keep: explicit phases, duplicate/hypothesis guards, evidence requests,
  re-evidence results, failure budgets, reversible-action requirements, and
  explicit transition receipts.
- Reshape: transitions are proposed Main state updates. `memory_write_allowed`,
  `tool_action_allowed`, and evidence-action intent do not themselves carry an
  execution capability; separate Main authority consumes them.
- Remove: any interpretation of a phase transition as action permission.

#### `mosaic_unrestricted_experience.py` (1-537)

- Symbols: artifact/snapshot 23-155; selection receipts 158-290; hot index
  293-339; discovery/index/selection 342-537.
- Keep: all regular artifacts, stable addresses, metadata snapshot digest,
  path containment/change detection, zero-authority receipts, selection and
  rejection judgments, hot address/cue lookup without live I/O.
- Reshape: Main-owned native original-experience journal and C++ address/cue
  views. Raw legacy files may be source artifacts, but all new experience uses
  the native format.
- Remove: `sqlite_tables`/`sqlite_rows`, SQL inspection, and file-type-specific
  authority implications. A legacy SQLite file is only raw addressed bytes.

### L4

#### `mosaic_memory_promotion.py` (1-261)

- Symbols: tier/candidate/decision 28-107; promotion decision 110-163; document
  projection 166-197; tier application 200-234; verified update 237-261.
- Keep: provenance quarantine, reject/retract, accepted and counterfactually
  verified semantic promotion, regime-change quarantine, episodic fallback,
  and version-preserving promotion.
- Reshape: promotion is bound to exact claim, decision, write receipt, and
  native experience addresses. It appends tier transitions and never copies a
  fact into an unrelated database row.
- Remove: direct mutation of separate episodic/semantic databases.

### L5

#### `mosaic_bounded_world_write.py` (1-487)

- Symbols: config/gates 34-92; gate capability/digest 95-172; receipt/result
  175-197; serialization 200-259; state hash and authorization 262-314;
  write 317-421; rollback/retraction 424-487.
- Keep: fail-closed gates, current evidence/revision, definitions,
  counterfactual/intervention support, diversity, runtime/device/capacity/role
  checks, bounded delta, immutable receipt, exact rollback, and LIFO retraction.
- Reshape: `MainStateWriter` consumes the exact authoritative decision and bound
  proposal. The receipt records the claim, decision digest, admitted evidence
  set, proposal digest, source state generation, target roles, and successor.
- Remove: successful writes from empty evidence addresses, unrelated decision
  and delta, or raw low-level arbiter calls. This explicitly closes the known
  architecture gap instead of reproducing it.

### L6

#### `mosaic_world_memory_transaction.py` (1-361)

- Symbols: config/receipt 27-46; journal prepare/stage 49-130; recovery 133-183;
  episodic record 186-209; linked promotion 212-273; rollback/retraction
  276-361.
- Keep: receipt binding, staged publication, incomplete-operation recovery,
  rollback order, and head-safe retraction.
- Reshape: Main-owned native transaction journal with immutable stage records,
  expected-head compare-and-swap, fsync before manifest publication, and
  compensation records.
- Remove: SQLite schema/transaction assumptions and deletion of history.

## 9. Native storage reconstruction

The storage design follows SWEGCA ownership rather than database compatibility.

Each journal record contains:

- record kind and format generation;
- stable original or derived address;
- source identity, source revision, and previous revision address;
- claim/proposition and outcome metadata when applicable;
- operation and transaction ID;
- canonical payload length and digest;
- previous record digest and current record digest;
- authority domain, with zero authority as the default.

The byte format uses a fixed magic and format version, fixed-width little-endian
header fields, length-prefixed UTF-8 identifiers, canonical payload bytes, and
SHA-256 digests. A checksum detects torn records. Addresses refer to immutable
record positions plus content digests; they never refer to a mutable row. The
hash chain is **re-created (user@2026-09-23)** to provide the immutable lineage
and revision binding required by SWEGCA I03 and I07 without SQLite.

Journal data is split into bounded immutable segments linked by predecessor and
successor manifests. The lower journal writes detached segment, page, and
manifest files and fsyncs them. Main's durable commit then records the exact
manifest location and digest and the state publication in a new, append-only
receipt: create the pending receipt exclusively, fsync it, compare-and-swap
Main's in-memory owner pair, rename that new receipt to its committed name,
and fsync its directory. Main writes no mutable current pointer; the lower
journal's HEAD is a native standalone pointer that Main does not read for
recovery. The committed receipt Main selects is the authoritative recovery
root. Verifying its predecessor chain is a native C++ requirement, beyond
the author's existing receipt fields. A pending marker, or a committed name
whose directory fsync failed, is not a proven durable commit marker; it is
retained and blocks further writes until reconciliation. The exact cold
selection rule among committed receipts remains to be defined.

Once Main owns a journal, it opens that journal only through its selected
root (`open_at_root`), never through standalone `open`. The lower journal's
own HEAD may support standalone use but cannot override a generation named
by Main's committed receipt. Under Main-selected recovery,
readers ignore detached files outside the selected generation while retaining
and charging their bytes until explicit reconciliation. Standalone `open`
still removes or truncates bytes past its own HEAD; it is not Main's recovery
path. A bad checksum or digest in a selected segment fails closed and is never
automatically truncated; torn-write cleanup applies only to unpublished
bytes in standalone mode or a future explicitly reconciled Main mode.
Transaction recovery starts from Main's selected committed receipt and appends
compensation records. Segment linking prevents any requirement to rewrite an
unbounded Main file. Segmented manifests and append-only commit receipts are
**re-created (user@2026-09-23)** to implement the one-current-generation,
recoverable publication requirements of I01, I07, and §4.8. The native Main
receipt writer, restart selection, and write resumption after an older selected
generation remain implementation work. After Main's in-memory owner swap, a
failed marker rename or directory fsync keeps the new pair live for reads and
refuses later writes until reconciliation; it does not roll the live pair
back. A failure before that swap leaves the prior pair current and retains
any partial pending marker.

Main prepares records and derived pages in a detached generation. The selected
manifest names the journal tail, state generation, view generations, and exact
digests. A newly committed Main receipt exposes the successor on disk; Main's
in-memory owner pair is replaced between writing its pending and committed
receipt names. Derived views are rebuildable and never become the source of
truth.

The native read path consists of exact address lookup, cue/region navigation,
and replay of the minimum exact original experience required by the current
claim. It does not scan every experience, use SQL, or treat retrieval score as
evidence authority.

## 10. Implementation order

1. Noncopyable Main ownership, distinct epistemic/authority types, canonical
   `CognitiveState`, role registry, native tensor storage, immutable snapshots,
   and the private successor boundary.
2. Main-owned native record addresses, journal generations, canonical digests,
   segment manifests, and publish/recovery heads.
3. Unfiltered original experience append, exact address replay, and zero
   authority selection receipts.
4. Claim/re-evidence/evidence accumulator and authoritative decision binding.
5. Bound transient proposal and commit-free conflict arbiter.
6. Guarded state write and complete write/no-write receipts.
7. Tiered memory promotion, versioning, native transactions, rollback,
   retraction, and recovery.
8. Autonomous cognition transition inputs and separate external action
   authority.
9. Before writing test code, publish the C++ test-condition list for static
   review. Conditions come from SWEGCA invariants I01-I08 and I10,
   experience-authority separation, failure model 1-10, E001/E002 rejection,
   compile-rejection cases, and forced interruption before and after each
   publication fsync boundary.
10. After the condition list is accepted, build the completed architecture and
    run the C++-only tests. Report every failure, fix the architecture, and
    rerun without weakening an expected result.
11. Only after the architecture passes those tests, build VRS on top of its
    Select and Re-evidence/evidence-accumulator boundaries.
12. Remove every `.py`, Python packaging path, generator, checker, and test from
    the rebuild tree after required replacements exist.

Static source and lineage review is performed at each commit. Build, syntax
compilation, product tests, and benchmarks remain stopped throughout
architecture implementation. They open only at step 10 after the complete
architecture and its test-condition list have passed static review
(`re-created (user@2026-09-23)`). VRS remains frozen until step 10 passes.

## 11. Open items

- The 2026-08-20 versus 2026-08-25 writer source decision remains deferred.
- Float16/bfloat16 arithmetic, Unicode normalization/case folding, and inverse
  standard-normal behavior need explicit native numerical contracts only where
  the reconstructed logic uses them.
- Final physical page sizes and compaction schedule must satisfy 4 GB RAM,
  500 GB disk, 5 Gbps SSD, and 16-worker limits without deleting any
  architecture stage or Main authority check.
