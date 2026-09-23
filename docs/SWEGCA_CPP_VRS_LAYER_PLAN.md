# SWEGCA core and VRS layer: relocation plan

Status: plan for review (claude, 2026-09-23). Nothing here is moved yet.

Rules, in the user's words where given:

- (user 2026-09-23 16:3x, direct) "SWEGCA 자체는 철저하게 검증기의 역할임.
  승인, 반려, 기권의 3가지 상태만으로 판단하고 이 행위를 '검증'으로 칭함.
  VRS는 SWEGCA 코어를 이용해서 경험을 '검증'하고 연결을 '강화'하는
  '시냅스'를 뜻함. 각 3가지 상태로 판별한 경험의 연결은 새로운 경험의
  습득을 통해 검증이 변화할 수 있으며 기존 조건에서의 반려가 매번 다시
  반려가 되는 것은 아님."
- (user 16:24 via codex 16:23) the core is the logic elements; VRS and the
  four memory stages (Déjà vu, Recall, Replay, Re-evidence) are built by
  combining them.
- (user 16:10, 16:13) usage counting and budget judgment belong to VRS; the
  baseline resource profile is a floor, not a ceiling.

So the core verifies and only verifies: from addressed evidence it judges a
claim accept, reject or abstain. Everything that finds, replays, admits or
re-judges experience, and everything that strengthens a connection from a
verdict (proposal, Bind, arbitration, the write, the state it changes) is
the synapse, VRS. No verdict is final: a later verification with new
experience may differ (test condition D15), so nothing on either side may
keep a reject as binding.

What does not change: the author's logic and the design board's rules, every
byte format, every failure code, and every test condition's expected result.
Only where each piece lives and which side of the boundary may name which
type change. Build and test gates stay closed (board §10 steps 9-10).

## 1. Classification

The core holds the verifier and the values it needs. It names no file,
journal or index, counts no resource, and writes no state.

| Module (today, `cpp/swegca_architecture/`) | Layer | Reason |
|---|---|---|
| digest_bytes, sha256, strong_types | core | values and identity rules the verifier uses |
| allocation (codex 408d4e2/3923503) | core | the abstract allocator the host supplies |
| byte codec (`ByteReader`, `ByteWriter`, buffer aliases, now in journal_format.hpp) | core | canonical encoding of what the verifier binds (decision digests); split out of journal_format |
| judgment_kernel: EvidenceStatus, EvidenceReason, EvidenceTally, EvidenceJudgment, columns, EvidenceRules, wilson_interval, judge_evidence(_batch); judgment_rules: EvidencePolicy, make_evidence_rules, evidence_policy_digest, standard_normal_quantile | core | the ternary judgment itself (accept, reject, abstain) and its configuration |
| judgment_kernel: GateCondition, GateInput, GateFailure, GateRules, authorize_target(_batch), GateColumns; ArbiterRules, ProposalScores, ArbiterShape, ArbiterBuffers, stable_norm, arbiter_input_valid, arbitrate; judgment_rules: GatePolicy, ArbiterPolicy and their make_ functions | VRS | the gate's output is a write-authorization failure mask and the arbiter's a proposal weight and delta, not a verdict (codex 16:55); both stay pure batch kernels, every condition bit and check kept |
| evidence_accumulator: tally, admit step, record step, decide, EvidenceDecision, ReEvidenceResult | core | the verifier's input and verdict (spec §4.4): the tally the kernel judges; see question 5.4 |
| evidence_accumulator: EvidenceAdmission, ReEvidence, SourceFamilies | VRS | Replay then admission, and Re-evidence: stages feeding the verifier |
| journal_file_io, journal_position, journal_format (record, manifest, page formats), journal_store | VRS | storage and lookup |
| experience (envelope, part tree, ExperienceJournal, views, CueTokens, ExperienceAppend, ExperienceSelector, receipts) | VRS | experience storage, Déjà vu and Recall |
| proposal, evidence_gate / Bind, arbiter, writer (codex) | VRS | strengthening a connection from a verdict (spec §4.5-4.6) |
| cognitive_state, native_tensor | VRS | the connection state strengthening changes |
| cognition | VRS | the autonomy loop that asks for verification and acts on verdicts |
| authority, authority_roles, role_registry, main_owner | VRS | who may write and Main's ownership of the synapse; see 3.4 |

The four stages, in the order the user approved
(docs/SWEGCA_VRS_MCP_ORDER_FOR_REVIEW.md@30b73e7, 2026-09-22; codex 16:39):
user input arrives at the resident hook and is used at once as the key of
Déjà vu on the pinned session VRS (the main VRS when nothing matches), with
no model decision, capsule lookup, fallback probe, transcript scan or index
build before it; the matched cues preactivate weighted regions,
coactivation witnesses are queried, an eligible portal is planned and one
cue page navigated; then Recall completes the candidate address set, Select
takes the first current original in the author's Recall order, Replay
opens only that original (and relevant opposing ones), and Re-evidence
judges them against the current generation, keeping unresolved conflict.
The journal's cue/index views are what Déjà vu reads; regions, coactivation
and portals are the VRS blocks and their connection points (§6). All four
stages are VRS, and each ends in the core verifier or feeds it.

## 2. Target layout

- `cpp/swegca_architecture/` (namespace `swegca::architecture`): the
  verifier only. No header here includes a VRS header (checked by the
  lineage gate, step 6).
- `cpp/swegca_vrs/` (namespace `swegca::vrs`): journal, experience, the four
  stages, admission, strengthening (proposal, Bind, arbiter, writer, state),
  cognition, authority and Main, the host's counting allocator and storage
  budget.

## 3. Boundary types (minimal, from current dependencies)

With strengthening in VRS, the only core boundary is the verifier's input
and output (3.1, 3.2). 3.3 and 3.4 are now choices inside VRS, kept here
because they were open.

Only what the current code already passes across is turned into a core type;
nothing new is invented.

3.1 Replayed original for admission. Today `EvidenceAccumulator::admit`
takes `const ExperienceRecord&` and reads only `record().address` and
`record().record_digest`, plus the root family and root context digests
admission computed (sorted spans; the correlation groups, codex 16:41).
Core type: `ReplayedOriginal { std::string_view address; DigestBytes
record_digest; std::span<const DigestBytes> root_families, root_contexts; }`.
The provenance checks (context, step, source family over root sources) and
the family registry stay in EvidenceAdmission (VRS), which alone may call
`admit` (friend, as today); grouping by what evidence shares is the
verifier's rule and stays in the core.

3.2 Re-evidence. `ReEvidenceResult` and `EvidenceAccumulator::record` stay
core; `ReEvidence` (VRS) replays and constructs the result, as today
(friend). `ReEvidenceJudge` takes the VRS `ExperienceRecord`, so it moves to
VRS with ReEvidence.

3.3 Bind's replay check (codex, evidence_gate.cpp). Decided (codex 16:37,
16:39): Bind and the journal are both VRS, so Bind keeps its
`ExperienceJournal&` and replays each exact admitted address itself: the
record decodes as an original or derived experience, its parts verify
(`verify_parts`), and its current record digest equals the admitted one. A
derived experience is accepted, as admission accepts it (test D13). No core
port (`RecordDigestOf`) is added.

3.4 Publication authority. Today `JournalStore::publish` is public and has
no caller yet; MainOwner holds no journal, only the initial Cognitive
State (codex 16:40). The MainLifetime lease also reaches StateSnapshot, so
it cannot serve as the right to publish without leaking it. Proposal:
`publish` becomes private with one friend, the VRS Main composition that
owns the JournalStore (MainOwner once it holds the journal in step 4). No
token object exists, so none can be lent or kept; the pattern is the one
SourceFamilies and EvidenceAdmission already use. No public `publish`
remains in the final state. `compact_view` and `rebuild_view` also move
HEAD, so they join `publish` behind the same friend (codex 16:53). `stage`
only builds a detached generation and stays callable, except that records
of the experience kinds (original, derived, part) are staged only through
ExperienceJournal (ExperienceAppend), which derives a derived record's root
sources and contexts from its published lineage; so no record's root sets
are self-declared, by induction over publication, without replaying
ancestors at read time. The core needs none (the verifier writes nothing).

3.5 Memory and storage. Core types take `AllocationContext` (codex). VRS
supplies the counting resource (baseline and scaled profiles), the page
cache's separate resource (`AllocationRefused` → evict and retry) and the
storage budget port. As built (claude, step 2): `StorageBudget::allows(used)`
is asked with the journal's whole use (published generation, logs a
rewrite left, what a write in progress adds) at open, stage, publish,
rebuild and compaction; `storage_charged()` reports the use; the host
counts and judges, the journal fixes no limit. The store shares
ownership of the budget (`std::shared_ptr<const StorageBudget>`), so no
teardown order can leave it dangling (codex 17:04).

3.6 The write path (codex 17:08-17:13, claude 17:1x). One authorized write
is one proposal, as the author's registered route is:
`bounded_verification_write` takes one proposal targeting only the
verification slot, previews the arbiter on that proposal alone and
commits with a receipt (mosaic_bounded_world_write.py@5901a5a:332-421);
multi-proposal arbitration is preview-only on the registered route
(COMPONENT_LEDGER.md@5901a5a:20). The order is Bind -> Arbiter preview ->
Gate -> Writer, with no other entry:

- Bind (codex): replays each exact admitted address and yields a
  `BoundProposal` (3.3).
- Arbiter: `ArbitrationResult` of that single proposal (weight, bounds,
  accepted, the changed verification role, generation and step).
- Gate: `GateOutcome EvidenceGate::authorize(const EvidenceDecision&,
  const EvidenceAccumulator&, const BoundProposal&, const
  ArbitrationResult& single_preview, const MainGateEvaluation&, const
  CognitiveState&, std::uint64_t step) const`. It checks the
  BoundProposal's decision, binding and step against the preview's
  single-input binding receipt, accepted flag, changed verification role
  and generation/step, then sends every existing condition bit to
  `authorize_target` (none removed). The capability's operation binds the
  decision, binding, bound receipt, preview receipt, verification role,
  role registry and generation. The separate `VerificationProposal`
  entry is gone, so no capability is issued without Bind.
- Writer (codex, not built): verifies the capability names the same
  results, commits, and keeps every field of the author's receipt
  (receipt id over before-state hash, delta hash, revision, evidence
  refs; before/after state and slot hashes, applied delta hash, prior
  write metadata). The guarded verification write keeps the state's
  tensor type: the author assigns the new slot into a clone of the
  existing scratch tensor, casting to its stored type
  (mosaic_bounded_world_write.py@5901a5a:317-330; codex 17:16). Promotion
  of state and delta happens only in the arbiter's own `commit=True`
  successor, which is not a registered route. A rollback or retraction
  restores the prior type and bytes exactly (codex 16:58). Proposals accepted together
  are written one at a time, each through this path on the state the
  previous one produced; no receipt spans several proposals.

## 4. Order (each step a pure move or a mechanical change, reviewed alone)

1. Codex: allocation.hpp and MainOwner/base types on AllocationContext
   (in progress, codex/swegca-allocation-integration).
2. Claude: journal/experience/evidence/cognition on AllocationContext;
   storage budget port; PageCache on its own resource. No relocation yet.
3. Claude: split judgment_kernel/judgment_rules into core
   `evidence_kernel.hpp`/`evidence_rules.*` and VRS `gate_kernel.hpp`,
   `arbiter_kernel.hpp` with their policies (pure moves, tags kept, no
   condition bit or check removed). Claude: split the byte codec out of journal_format.hpp into core
   `byte_codec.hpp` (cognition and envelopes use it). Pure move.
4. `git mv` into `cpp/swegca_vrs/`, namespace `swegca::vrs`, includes and
   qualifiers only; the lineage gate checks every moved definition keeps
   its tag. Claude: journal_*, experience.*, and EvidenceAdmission,
   ReEvidence, SourceFamilies into `evidence_stages.*`. Codex: proposal,
   evidence_gate, arbiter, writer, cognitive_state, native_tensor,
   authority*, role_registry, main_owner. Claude: cognition.
5. Claude: 3.1 (`ReplayedOriginal`), 3.2. Codex: 3.3, 3.4.
6. Both: a layering check in the lineage gate (no `swegca_architecture/`
   file includes `swegca_vrs/`), test conditions regrouped by layer (A, D1-D9
   core; B, C, D10-D13 VRS), and the design board's module inventory updated.

## 5. Open questions for codex

- 3.3: decided (b without a port: Bind replays in VRS), codex 16:37.
- 3.4: private `publish` with the Main composition as its only friend
  (above); codex to confirm.
- SourceFamilies: the registry is VRS; the grouping rule (families and
  contexts linked by shared roots) is the core accumulator's.
- 5.4: accumulation stays in the core (codex 16:37, spec §4.4).

## 6. VRS composition (user 2026-09-23 16:4x)

The user: "VRS는 블록의 경험을 마구 뒤섞은 후에 SWEGCA 검증을 통해
시냅스의 연결을 강화 또는 약화함. 시냅스의 강도에 따라 경험의 신뢰도를
결정함. VRS는 단일 블록이 아닌 여러 블록으로 구성되며, 해당 블록 사이에는
서로를 호출할 수 있는 연결점이 존재해야함. 단순히 SWEGCA의 검증만으로는
경험의 연결 시냅스를 만들 수 없고, 이론상 모든 결정이 '기권'으로 수렴하게
되어있기 때문에 VRS가 반드시 필요한 것. VRS의 블록 크기는 일정 크기
이상으로 커지지 않도록 조절해야함."

What VRS must hold, beyond the journal and the four stages: blocks of
experience (several, each kept below a size bound by splitting),
connection points through which blocks call each other, a shuffle of a
block's experiences before verification, strengthening and weakening of
connections from the verifier's verdicts, and reliability of an
experience from the strength of its connections (not from a verdict). The
core verifier abstains unless the evidence meets its sample, diversity
and regime conditions, and a verdict is never permanent (D15); without the
synapse, decisions tend to abstain. The mapping to the author's sources
(regions, portals, coactivation, signal strength, state update in the
prior `cpp/` VRS) and what the rebuild still lacks are listed in 6.1.

### 6.1 Where each part is today (read of `cpp/` and author sources, 2026-09-23)

The prior VRS lives in top-level `cpp/`; its lineage tags point at
`src/swegca_vrs2/engine/*.py` in this repository (revisions 7536139,
c06092a) and at `src/tinylm_slicer/*@3bddcb7` (not in this repository).
The author's `src/swegca` has no VRS code; only rule 8 has an author
source there.

| Part | Prior `cpp/` | Its tag | Rule | In the rebuild |
|---|---|---|---|---|
| 1 Blocks | connectivity_regions.cpp:311 `ConnectivityRegions::build` (local_moves :151, communities :205, memberships :233); graph_regions.cpp:112 | mosaic_vrs_connectivity_regions.py@7536139:159-199, :47-78, :81-96, :99-118; store.py@7536139:282-298 | regions by modularity over the experience/cue graph (move score w_in - deg*tot/mass, sweeps 100, levels 32); membership = neighbour mass / total, a node may belong to several; only affected components recomputed; not published unless converged | missing |
| 2 Block size bound | not found (receipt says `original_experiences_split: 0`, connectivity_regions.cpp:430; only work budgets and a cache byte cap) | - | - | missing, and missing before: to be designed |
| 3 Connection points | graph_regions.cpp:202 bridges; coactivation_associations.cpp:81; portal_lifecycle.cpp:170 `plan_portals`; portal_navigation.cpp:14; region_navigation.cpp:74, :86 | mosaic_vrs_connectivity_regions.py@7536139:213-248; mosaic_vrs_coactivation_navigation.py@c06092a:118-183; mosaic_vrs_portal_lifecycle.py@c06092a:89-156; tinylm_slicer portal_activation@3bddcb7:81-122, local_navigation@3bddcb7:57-107 | an experience whose cues span two or more regions is a bridge; the first eligible bridge becomes a portal into the destination region's cues, ordered by decayed coactivation mass | missing |
| 4 Shuffle | no random shuffle found; nearest is deterministic coactivation: coactivation.cpp:45, coactivation_associations.cpp:39 | mosaic_vrs_coactivation.py@c06092a:107-158; mosaic_vrs_coactivation_navigation.py@c06092a:77-116 | experiences recalled together recorded per (topology, region) | missing; the user's shuffle is not in the prior code |
| 5 Strengthen | vrs_state_update.cpp:137, :196; event_signal_strength.cpp:113; graph_append.cpp:135 (:213); event_vrs_kernel.cpp:61 | mosaic_vrs_state_update.py@7536139:75-152; mosaic_vrs_event_signal.py@7536139:53-97; store.py@7536139:193-251; mosaic_vrs_event_kernel.py@7536139:134-209 | support x1.01; new link 0.5; same-claim same-polarity strengthens the older link; strengthen and weaken together is `abstain_conflict` (unchanged); kernel: stable when 1-0.5*abs(l*sign-r) >= 0.75 and abs(l)+abs(r) >= 0.1, bounds [0.25*base, 4*base] | missing (judgment_kernel has only the arbiter's proposal weight) |
| 6 Weaken, decay | vrs_state_update.cpp:137 (refute); graph_append.cpp:213 (retracted); event_vrs_kernel.cpp:61 (unstable); portal_lifecycle.cpp:170 (time decay of portal mass only) | as 5; mosaic_vrs_portal_lifecycle.py@c06092a:89-156 | refute x0.995, floor 0.25*base; portal mass H/(H+age), ineligible past maximum_age or below minimum_mass; no time decay of link strength found | missing |
| 7 Strength to reliability | memory_promotion.cpp:23, :33, :56; memory_evidence.cpp:124; graph_regions.cpp:281 | mosaic_memory_promotion.py@7536139:4, :32-40, :55-83; mosaic_memory_activation.py@7536139:432-458; store.py@7536139:305-307 | strength >= 1.0 is promoted (semantic evidence, "retained"), below is "available"; promote / retain / revoke / remain_unpromoted; f16 rounding across 1.0 is an error | missing |
| 8 A reject is not final | graph_append.cpp:213-249; vrs_state_update.cpp:165, :224; memory_evidence.cpp:188 | store.py@7536139:223-249; mosaic_vrs_state_update.py@7536139:131-151; mosaic_memory_activation.py@7536139:461-555 | a reject only multiplies by 0.995 and never deletes; a later support raises it again, promotion can be revoked and regained; author: every observation re-judges (mosaic_evidence_accumulator.py@5901a5a:285-428) | verdict side present (judgment_kernel, evidence_accumulator, cognition); strength side missing |

Order for VRS (after steps 1-6): the synapse state (links, strength,
promotion) and its update from verdicts (5, 6, 7, 8) first, since blocks
and portals are read paths over it; then blocks (1) with the size bound (2)
and connection points (3); then the shuffle (4), whose rule must come from
the user's definition, as the prior code has none. Each is ported with its
lineage tag from the prior engine, and the bound and the shuffle are
designed with codex and put to the user where the definition leaves a
choice.
