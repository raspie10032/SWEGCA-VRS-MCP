# SWEGCA core and VRS layer: relocation plan

Status (2026-09-23 17:4x): the core phase (steps 3-4) is closed: the core
is split out, matches the author's judgment to the bit and passes its
C++-only tests. The VRS steps (5-7), the accumulator and stages
correction and section 6 are pending; nothing in `cpp/swegca_vrs/` is
moved yet. 2026-09-23 18:2x: the four stages are drafted in 6.2 with the
user's decisions; the author's VRS kernel is located in the original
experiment repository (6.1).

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
| byte codec (`ByteReader`, `ByteWriter`, buffer aliases, in journal_format.hpp) | VRS | the core does not use it: the accumulator hashes its own length-prefixed fields; it encodes records, envelopes and cognition |
| evidence_kernel (was judgment_kernel): EvidenceStatus, EvidenceReason, EvidenceTally, EvidenceJudgment, columns, EvidenceRules, wilson_interval, judge_evidence(_batch); evidence_rules (was judgment_rules): EvidencePolicy, make_evidence_rules, evidence_policy_digest, standard_normal_quantile | core | the ternary judgment itself (accept, reject, abstain) and its configuration |
| gate_kernel (from judgment_kernel): GateCondition, GateInput, GateFailure, GateRules, authorize_target(_batch), GateColumns; arbiter_kernel: ArbiterRules, ProposalScores, ArbiterShape, ArbiterBuffers, stable_norm, arbiter_input_valid, arbitrate; gate_rules, arbiter_rules (from judgment_rules): GatePolicy, ArbiterPolicy and their make_ functions | VRS | the gate's output is a write-authorization failure mask and the arbiter's a proposal weight and delta, not a verdict (codex 16:55); both stay pure batch kernels, every condition bit and check kept |
| evidence_accumulator: tally, admit step, record step, decide, EvidenceDecision, ReEvidenceResult, ReplayedOriginal | VRS | Main-owned state around the judgment (allocation, sets, friends of Main's stages); it builds the tally the core judges and carries the core's verdict in a decision (codex 17:24: a stateful accumulator in the core reopens friend-by-name spoofing; the core is the pure judgment) |
| evidence_stages: EvidenceAdmission, ReEvidence, ReEvidenceJudge, SourceFamilies | VRS | Replay then admission, and Re-evidence: stages feeding the verifier |
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

Corrected by the user on 2026-09-23 18:0x (6.2 governs): Recall yields
addresses only; Replay opens one original, not "relevant opposing ones";
Re-evidence runs only when the current input conflicts with it. "The
author's Recall order" above is the prior engine's (-cue_overlap, address)
order (mosaic_memory_activation.py@7536139:301-348), not the author's
(SWEGCA-Architecture@5901a5a has no such order), and cue overlap is
duplication, not strength (6.1). The Select criterion is decided (6.2).

## 2. Target layout

- `cpp/swegca_architecture/` (namespace `swegca::architecture`): the
  verifier only. No header here includes a VRS header (checked by the
  lineage gate, step 6).
- `cpp/swegca_vrs/` (namespace `swegca::vrs`): journal, experience, the four
  stages, admission, strengthening (proposal, Bind, arbiter, writer, state),
  cognition, authority and Main, the host's counting allocator and storage
  budget.

## 3. Boundary types (minimal, from current dependencies)

The core boundary is the kernel's own: `EvidenceRules` and an
`EvidenceTally` in, an `EvidenceJudgment` (accept, reject, abstain and
its reasons) out, with no allocation, lock, exception or I/O. 3.1 to 3.4
are choices inside VRS (codex 17:24 moved the accumulator there), kept
here because they were open.

Only what the current code already passes across is turned into a boundary type;
nothing new is invented.

3.1 Replayed original for admission. Today `EvidenceAccumulator::admit`
takes `const ExperienceRecord&` and reads only `record().address` and
`record().record_digest`, plus the root family and root context digests
admission computed (sorted spans; the correlation groups, codex 16:41).
Type (VRS, the accumulator's input): `ReplayedOriginal { std::string_view
address; DigestBytes record_digest; std::span<const DigestBytes>
root_families, root_contexts; }`. Duplicates are one experience (user
2026-09-23) by the author's rule: a seen published address is refused
(mosaic_evidence_accumulator.py@5901a5a:392-418), and the address is the
digest of the experience's identity (kind, source, revision, previous,
outcome, payload with step, context and blobs), so the same experience
recorded twice is one address. No other digest selects "the same
experience" (codex 17:44; review B2: a raw-and-structured digest folded
independent replications into one).
The provenance checks (context, step, source family over root sources) and
the family registry stay in EvidenceAdmission (VRS), which alone may call
`admit` (friend, as today); grouping by what evidence shares is the
accumulator's rule. The friends are complete wherever the accumulator
is visible: MainOwner from authority_roles.hpp, the stages from
evidence_stages.hpp included at the accumulator header's end.

3.2 Re-evidence. `ReEvidenceResult` and `EvidenceAccumulator::record` stay
with the accumulator; `ReEvidence` (VRS) replays and constructs the result, as today
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

## 4. Order (user 2026-09-23 17:2x: the core, then tests, then VRS)

The user's order was always architecture, then tests, then VRS, and "the
SWEGCA architecture" is the core ("swegca 아키텍처 구현이 코어 구현이었다는
말"). Steps 1-2 below mixed VRS modules into the core work; from here the
core is closed and tested first, and VRS waits.

Done:
1. Codex: allocation.hpp and MainOwner/base types on AllocationContext
   (codex/swegca-allocation-integration).
2. Claude: journal/experience/evidence/cognition on AllocationContext;
   storage budget port; PageCache on its own resource (581f9b2, 6642262).
   VRS work stops here until the core is tested.

Core (claude/core-close; closed 2026-09-23 17:4x):
3. Done (claude c26a14a, 1d4fd2d). Claude: split judgment_kernel/judgment_rules into core
   `evidence_kernel.hpp`/`evidence_rules.*` and VRS `gate_kernel.hpp`,
   `arbiter_kernel.hpp` with their policies (pure moves, tags kept, no
   condition bit or check removed). The core is then
   `evidence_kernel.hpp` and `evidence_rules.*` over digest_bytes,
   sha256, strong_types and allocation's types. Also, in VRS: split
   evidence_accumulator from the stages feeding it (`evidence_stages.*`:
   SourceFamilies, EvidenceAdmission, ReEvidenceJudge, ReEvidence), the
   accumulator taking `ReplayedOriginal` (3.1) and counting an experience
   once by its address (user 17:0x, codex 17:44), its friends complete wherever it is
   visible (codex 17:24). The byte codec stays in journal_format: the
   core does not use it, so it is VRS. The accumulator part was taken
   out of c26a14a and waits for VRS step 5 (below). 1d4fd2d makes the
   kernel judge as the author does (codex 17:28): an unmeasured regime
   window scores 0 and is still compared, and the Wilson z is AS241 as
   CPython computes NormalDist().inv_cdf (equal to the bit on 50,597
   probabilities).
4. Codex: a C++-only build of the core alone (no Python), with the
   arithmetic flags of evidence_kernel.hpp (-ffp-contract=off, no fast
   math), and its tests: A for core types, D8 (each negative condition
   alone), D8p and D9 on the kernel, rule validation and the policy
   digest, the kernel's batch and item paths equal, one judgment in
   nanoseconds on the baseline profile. Claude cross-reviews. Board
   section 10 steps 9-10 open for the core here. D1-D7, D8s, D10-D16 and
   the duplicate rule test the accumulator and stages: VRS tests.
   Done (codex/swegca-core-test-prep 6eac79a, claude cross-review):
   CMake target `swegca_core` (sha256, strong_types, evidence_rules) and
   `cpp/tests/evidence_core_test.cpp`; a fail-closed guard on
   nonfinite derived values and on overlapping batch columns (c13c9af;
   D8(h), I04: the author's Python would accept on a NaN posterior).
   The judgment equals the author's code (5901a5a:261-357, run
   verbatim) to the bit in status, reason and the four outputs on seven
   cases, pinned in the test. GCC 16.2.1, -Wall -Wextra -Werror
   -ffp-contract=off -fno-fast-math, -O0 and -O2: exit 0.
   `cpp/tests/evidence_core_bench.cpp` (-O2, one thread, AMD Ryzen 7
   9800X3D): about 41 ns per judgment, 54 ns per batch item.

VRS (pending; the core has passed):
5. `git mv` into `cpp/swegca_vrs/`, namespace `swegca::vrs`, includes and
   qualifiers only; the lineage gate checks every moved definition keeps
   its tag. Claude: journal_*, experience.*, evidence_stages.*,
   gate/arbiter kernels and rules. Codex: proposal, evidence_gate, arbiter,
   writer, cognitive_state, native_tensor, authority*, role_registry,
   main_owner. Claude: cognition.
6. Claude: 3.2. Codex: 3.3, 3.4, the Main composition and the writer (3.6).
7. Both: a layering check in the lineage gate (no `swegca_architecture/`
   file includes `swegca_vrs/`), test conditions regrouped by layer (A,
   D8, D8p, D9 core; B, C and the other D items VRS), the design board's module inventory
   updated; then the synapse (section 6).

## 5. Open questions for codex

- 3.3: decided (b without a port: Bind replays in VRS), codex 16:37.
- 3.4: private `publish` with the Main composition as its only friend
  (above); codex to confirm.
- SourceFamilies: the registry is VRS; the grouping rule (families and
  contexts linked by shared roots) is the accumulator's.
- 5.4: accumulation was kept in the core (codex 16:37, spec §4.4), then
  moved to VRS (codex 17:24): the core is the pure judgment.

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
synapse, decisions tend to abstain. What the prior engine had for each part
(regions, portals, coactivation, signal strength, state update in the
prior `cpp/` VRS) and what the rebuild still lacks are listed in 6.1.

### 6.1 Where each part is today (read of `cpp/` and author sources, 2026-09-23)

**The prior engine is not a rule source (user 2026-09-23 18:0x).** The
user: "옛 VRS는 너희 에이전트들이 코딩하는 과정에서 불필요하다고 멋대로
swegca 아키텍처를 잘라내서 제대로 동작을 안했거든". Rules come only from,
in order: the user's definitions, the author (SWEGCA-Architecture@5901a5a),
and documents the user approved. The table below records what the prior
code did, so that a missing SWEGCA part can be found and restored; its
"Prior trace" column states behavior, not a rule to port. Where the prior
code creates strength or rank without verification, the trace is a cut
and is not copied:
- Recall ordered by cue overlap. The user: "cue가 가장 많이 겹친다는건,
  중복이라는 뜻이지. 이걸 왜 경험강화를 했나". Overlap is duplication, and
  a duplicate is one experience (user 17:0x), not a strength.
- The same claim with the same polarity strengthens the older link (x1.01)
  with no correlation grouping.
- Coactivation mass feeding itself: experiences recalled together gain mass
  that ranks them to be recalled together again.
Coactivation and portals stay as SWEGCA parts (blocks and connection
points, user 16:4x); repeated calling is never evidence of reliability.
Reliability comes from strength, and strength changes only by verification.
A number in the column is adopted only where the author's original
experiment has it with the same meaning (below); the rest (0.5, H/(H+age),
sweep and level counts) have no source above the prior code.

**The author's original VRS kernel (found 2026-09-23 18:2x).** The user:
"원 실험 저장소에 어지간하면 다 있긴 할걸". The original experiment
repository `tinylm slicer` (same committer as SWEGCA-Architecture) holds
`refine_vrs` in
tools/organize_rozephine_mixed_experience_connections_hybrid.py@7190660997:455-539
(2026-08-19, "Name and validate the VRS convergence loop"). Per shuffle
cycle it permutes the edges at random (`randperm`), propagates the
experience states over them in that order (tanh(direct + 0.2 * aggregate /
degree), blended 0.8/0.2), then re-verifies every edge: stable when
compatibility 1 - 0.5*abs(state[source]*sign - state[target]) >= 0.75, the
ends are informed (abs sum >= 0.1) (and, from 0885b1e97f on 2026-08-25, neither end is unresolved; HEAD e88324e0d3:831-942); a stable
edge is strengthened x1.01, every other edge weakened x0.995, clamped to
[0.25, 4] times its base; the spread of states across cycles gives a
shuffle stability. This is the user's 16:4x definition (shuffle, then
strengthen or weaken by verification), and it is the source for parts 4-6.
The prior engine's x1.01 per new same-polarity record (part 5 row) reuses
the constant with another meaning, a count of evidence; that is the cut
trace, not the rule. The copy in this repository,
src/swegca_vrs_mcp/core/vrs_refinement.py@51cf1f9, is an extraction of the
same kernel. Where the kernel's "stable" test and the SWEGCA core verifier
meet (the verifier is ternary) is a design item with codex.

The prior VRS lives in top-level `cpp/`; its lineage tags point at
`src/swegca_vrs2/engine/*.py` in this repository (revisions 7536139,
c06092a) and at `src/tinylm_slicer/*@3bddcb7` (not in this repository).
The author's `src/swegca` has no VRS code; only rule 8 has an author
source there. The original experiment repository above does.

| Part | Prior `cpp/` | Its tag | Prior trace (not a rule source) | In the rebuild |
|---|---|---|---|---|
| 1 Blocks | connectivity_regions.cpp:311 `ConnectivityRegions::build` (local_moves :151, communities :205, memberships :233); graph_regions.cpp:112 | mosaic_vrs_connectivity_regions.py@7536139:159-199, :47-78, :81-96, :99-118; store.py@7536139:282-298 | regions by modularity over the experience/cue graph (move score w_in - deg*tot/mass, sweeps 100, levels 32); membership = neighbour mass / total, a node may belong to several; only affected components recomputed; not published unless converged | missing |
| 2 Block size bound | not found (receipt says `original_experiences_split: 0`, connectivity_regions.cpp:430; only work budgets and a cache byte cap) | - | - | missing, and missing before: to be designed |
| 3 Connection points | graph_regions.cpp:202 bridges; coactivation_associations.cpp:81; portal_lifecycle.cpp:170 `plan_portals`; portal_navigation.cpp:14; region_navigation.cpp:74, :86 | mosaic_vrs_connectivity_regions.py@7536139:213-248; mosaic_vrs_coactivation_navigation.py@c06092a:118-183; mosaic_vrs_portal_lifecycle.py@c06092a:89-156; tinylm_slicer portal_activation@3bddcb7:81-122, local_navigation@3bddcb7:57-107 | an experience whose cues span two or more regions is a bridge; the first eligible bridge becomes a portal into the destination region's cues, ordered by decayed coactivation mass | missing |
| 4 Shuffle | no random shuffle found; nearest is deterministic coactivation: coactivation.cpp:45, coactivation_associations.cpp:39 | mosaic_vrs_coactivation.py@c06092a:107-158; mosaic_vrs_coactivation_navigation.py@c06092a:77-116 | experiences recalled together recorded per (topology, region) | missing; the prior engine lost it; the author's shuffle is `refine_vrs` in the original experiment (above) |
| 5 Strengthen | vrs_state_update.cpp:137, :196; event_signal_strength.cpp:113; graph_append.cpp:135 (:213); event_vrs_kernel.cpp:61 | mosaic_vrs_state_update.py@7536139:75-152; mosaic_vrs_event_signal.py@7536139:53-97; store.py@7536139:193-251; mosaic_vrs_event_kernel.py@7536139:134-209 | support x1.01; new link 0.5; same-claim same-polarity strengthens the older link; strengthen and weaken together is `abstain_conflict` (unchanged); kernel: stable when 1-0.5*abs(l*sign-r) >= 0.75 and abs(l)+abs(r) >= 0.1, bounds [0.25*base, 4*base] | missing (judgment_kernel has only the arbiter's proposal weight) |
| 6 Weaken, decay | vrs_state_update.cpp:137 (refute); graph_append.cpp:213 (retracted); event_vrs_kernel.cpp:61 (unstable); portal_lifecycle.cpp:170 (time decay of portal mass only) | as 5; mosaic_vrs_portal_lifecycle.py@c06092a:89-156 | refute x0.995, floor 0.25*base; portal mass H/(H+age), ineligible past maximum_age or below minimum_mass; no time decay of link strength found | missing |
| 7 Strength to reliability | memory_promotion.cpp:23, :33, :56; memory_evidence.cpp:124; graph_regions.cpp:281 | mosaic_memory_promotion.py@7536139:4, :32-40, :55-83; mosaic_memory_activation.py@7536139:432-458; store.py@7536139:305-307 | strength >= 1.0 is promoted (semantic evidence, "retained"), below is "available"; promote / retain / revoke / remain_unpromoted; f16 rounding across 1.0 is an error | missing |
| 8 A reject is not final | graph_append.cpp:213-249; vrs_state_update.cpp:165, :224; memory_evidence.cpp:188 | store.py@7536139:223-249; mosaic_vrs_state_update.py@7536139:131-151; mosaic_memory_activation.py@7536139:461-555 | a reject only multiplies by 0.995 and never deletes; a later support raises it again, promotion can be revoked and regained; author: every observation re-judges (mosaic_evidence_accumulator.py@5901a5a:285-428) | verdict side present (judgment_kernel, evidence_accumulator, cognition); strength side missing |

Order for VRS (after steps 1-6): the synapse state (links, strength,
promotion) and its update from verdicts (5, 6, 7, 8) first, since blocks
and portals are read paths over it; then blocks (1) with the size bound (2)
and connection points (3); then the shuffle (4), whose rule must come from
the user's definition, as the prior code has none. Each part is designed
from the user's definition and the author, with a lineage tag to that
source; the prior engine only shows where a part was and what was cut. The
bound, the shuffle and every constant are designed with codex and put to
the user where the definition leaves a choice.

### 6.2 The four memory stages (draft, 2026-09-23)

Purpose, the user (17:5x): "기억의 4단계는 방대한 기억에서 정확한 경험을
빠르게 불러오기 위한 과정임. 이거 중요". The pass condition is speed at
scale and the exact address.

Definition, the user (18:0x; same meaning as the corrections of 2026-09-22
15:0x and 15:2x): "리콜이 원경험 주소, 리플레이가 원경험. 재검증은 현재
입력과 경험이 충돌할때 재검증하기 야".

Withdrawn: the wording in claude's msg 193-194 that Re-evidence runs after
every Replay and also opens opposing originals. Re-evidence is conditional.

| Stage | Does | Does not | Core parts used | Source |
|---|---|---|---|---|
| Déjà vu | reacts first to the input's keys: which caller keys hit, and how many addresses each key holds | build a union of postings; open any record | none (exact key lookup, no verdict) | user 18:0x; author `HotExperienceIndex.lookup_semantic_key` (mosaic_unrestricted_experience.py@5901a5a:294-320: prebuilt, no I/O or hashing on lookup, "Building the index is deliberately separate from lookup") |
| Recall | yields original addresses, a bounded page | open content; judge | none | user 18:0x ("리콜이 원경험 주소") |
| Replay | opens the most probable original, one | open every candidate | digest check of the exact record on read | user 18:0x; author exact `lookup_address` (:314-318) |
| Re-evidence | re-judges only when the current input conflicts with the replayed experience; the verdict updates strength (6.1 parts 5, 6, 8) | run on every read | evidence kernel and accumulator (tally, decide) | user 18:0x; author accumulator (mosaic_evidence_accumulator.py@5901a5a:285-428); a verdict is not permanent (user 16:3x) |

Budget: Déjà vu to Recall together under 1 ms, at the store's scale; the
budget does not extend to Replay or Re-evidence. Neither the author nor the
prior engine met or measured it (VRS_REGIONS.md@c06092a:147). The prior
cost grew with the fanout of a broad cue: the union of postings in Déjà vu,
and Recall scoring every candidate before choosing (1 item 0.79 ms, 1,008
items 121 ms, NATURAL_REPLAY_BOTTLENECK@c06092a:154-167). The rebuild's
Déjà vu reads per-key counts that the index already holds, so its cost does
not depend on how many addresses a key has. Recall reads one bounded page.
A benchmark at the profile's scale is part of the stage's tests.

Where the author differs from the user's definition, the user's definition
governs. `select_experience_for_cognition` (:475-540) unions the postings of
every query key, falls back to the whole universe when no key matches, and
judges and reads every candidate. That is the full-scan path the four stages
exist to avoid; its contract survives only in its receipt (each candidate
decision is replayable, zero authority).

Cues. The author's `ExperienceArtifact` (:34-55) carries no cues; semantic
keys enter only the postings, supplied beside the artifacts to
`build_hot_experience_index` (:398-436). The user (18:0x): "cue는 호출을
위한 호출자로 쓰이는거지 경험에 넣으면 안되는게 아닐까?". The rebuild keeps
authored cues inside the experience record (`Observation.semantic_cues`,
`RecordDraft.index`, `experience_index_conflict`), which differs from both.
Approved by the user (18:2x, "가. 승인"): cues leave the record and form a
caller index layer beside it; the record's address no longer depends on cues.
The interface goes to codex before the change (journal index and kind rules).

Decided by the user (18:2x):
- Replay's "most probable": "vrs 강도가 높은 것으로." The one current
  original with the highest VRS strength (reliability comes from strength,
  16:4x); current by the author's versioned rule
  (mosaic_versioned_memory.py@5901a5a:391 excludes non-current). Not cue
  overlap: overlap is duplication (18:0x). Ties, the user (18:3x): "정말
  희박한 확률이겠지만 vrs 강도가 같다면, 5건 까지는 다 불러와." When
  several current originals share the highest strength, Replay opens up to
  five. Open: which five when six or more tie (proposed, the author's address
  order, mosaic_unrestricted_experience.py@5901a5a:501; held until the user
  confirms, codex 18:24). The user (18:3x) on how often ties occur: "옛
  스토어는 검증 규칙을 무시하고 단순하게 중복은 증폭시켜서 그 사단이
  난거고... 이론상 vrs 강도 겹침은 꽤 희박해야 정상임..."; the cap is a
  guard for a rare case.
- One experience's strength (codex 18:25 asked for its source). In the
  author's VRS memory an experience is an edge ("vrs-edge:N") or a
  canonical edge group ("vrs-edge-group:N"), and its strength is that
  group's strength with no aggregation:
  `ResidentVrsStrengthIndex.strength`,
  tinylm-slicer-sanabi-bazzite@3bddcb7:src/tinylm_slicer/mosaic_paper_vrs_resident_adapter.py:31-43, :92-96
  (from c925bd7d9d, 2026-09-05); a missing group reads 0.0. Duplicate edges
  merge into one canonical group whose strength is the occurrence-weighted
  mean (mosaic_vrs_canonicalization.py@3bddcb7:285-295, :403; from
  089f4db3ab, 2026-09-01), the same shape as the user's duplicate rule. Open:
  the author's group key, and how it maps to "same published address".
  3bddcb7 (2026-09-13) is that repository's origin/main, the revision the
  prior `cpp/` tags as `src/tinylm_slicer/*@3bddcb7`: those rows of 6.1 are
  author-sourced and are to be separated from the rows tagged at the
  prior engine (7536139, c06092a).
- Speed of that choice (codex 18:27): scanning every Recall candidate for the
  highest strength grows with a broad cue's fanout and breaks the Déjà vu to
  Recall budget. The current top addresses by strength must be kept
  incrementally where the caller index and region pages are, updated when a
  strength changes; no unsourced cap or approximate top-k.
- Where the kernel meets the verifier (codex 18:26): `refine_vrs`'s stable
  test is boolean (compatibility, informed, not unresolved); the core
  verifier is ternary. Abstain is not mapped to either side by us; the
  junction needs a source.
- Parallel form (codex 18:28; user profile: 16 threads, no one-thread
  bottleneck): the propagation in shuffled order is order dependent and is
  kept as ordered; the per-edge re-verification and strength update after
  it are independent and parallel.

State generation, the user (18:3x): "애초에 세대번호는 그냥 기록시간으로
외부에 두면 되는거 아니냐 굳이 넘버링안하고 시간으로 두면 시간선흐름정도는
추측이 될거같은데". The state's identity is its content digest, as the
author's `cognitive_state_hash` (mosaic_bounded_world_write.py@5901a5a:262-283)
has no ordinal. No separate ordinal counter is kept (codex 18:31). A
bit-exact rollback reuses the same content root, so the root's first
position cannot tell before from after: every state transition (and the
initialisation) publishes its own receipt record with a new sequence and
record digest; the manifest names (content digest, latest publication
position and digest), and compare-and-swap is on that publication identity.
The record time sits only in the transition receipt, as a timeline for
people, never as order or authority. The author's write revision
(self_state `_WRITE_KEY`, mosaic_bounded_world_write.py@5901a5a:384) stays a
content field and rewinds with a rollback.
- The conflict that triggers Re-evidence: "흑백논리라면 반대결과가 맞지만
  3상으로 취급하는 이상 같은 상이 아니면 불러와서 재검증." Any difference
  between the current input's state and the experience's state among accept,
  reject and abstain, not only the opposite outcome. Recorded as verdict
  `replay-picks-highest-vrs-strength-reevidence-on-any-state-mismatch`.

One Replay is one original experience: its head record and every part the
head needs, each verified by digest. Reading a part
(`ExperienceRecord::for_each_chunk`, experience.cpp:851-881, through
`JournalStore::replay`) is internal to that one Replay and is not counted
as another (codex 18:18); the stage receipt counts Replays per original.

Rebuild changes this implies: `ExperienceSelector::select` (judges every
candidate and replays each selected) splits into Recall (addresses) and
Replay (one); Re-evidence is called only on a state mismatch. Regions, portals and
coactivation (6.1 parts 1-4) come after this read path.
