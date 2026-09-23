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
| judgment_kernel, judgment_rules | core | the ternary judgment itself (accept, reject, abstain) and its configuration |
| evidence_accumulator: tally, admit step, record step, decide, EvidenceDecision, ReEvidenceResult | core | the verifier's input and verdict (spec §4.4): the tally the kernel judges; see question 5.4 |
| evidence_accumulator: EvidenceAdmission, ReEvidence, SourceFamilies | VRS | Replay then admission, and Re-evidence: stages feeding the verifier |
| journal_file_io, journal_position, journal_format (record, manifest, page formats), journal_store | VRS | storage and lookup |
| experience (envelope, part tree, ExperienceJournal, views, CueTokens, ExperienceAppend, ExperienceSelector, receipts) | VRS | experience storage, Déjà vu and Recall |
| proposal, evidence_gate / Bind, arbiter, writer (codex) | VRS | strengthening a connection from a verdict (spec §4.5-4.6) |
| cognitive_state, native_tensor | VRS | the connection state strengthening changes |
| cognition | VRS | the autonomy loop that asks for verification and acts on verdicts |
| authority, authority_roles, role_registry, main_owner | VRS | who may write and Main's ownership of the synapse; see 3.4 |

Déjà vu = cue/index retrieval (views over the journal); Recall = Select with
the judge and its receipt; Replay = exact record and part streaming;
Re-evidence = ReEvidence against the current state. All four are VRS, and
each ends in the core verifier or feeds it.

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
`record().record_digest` (evidence_accumulator.cpp:232-321). Core type:
`ReplayedOriginal { std::string_view address; DigestBytes record_digest; }`.
The provenance checks (context, step, source family over root sources)
stay in EvidenceAdmission (VRS), which alone may call `admit` (friend, as
today).

3.2 Re-evidence. `ReEvidenceResult` and `EvidenceAccumulator::record` stay
core; `ReEvidence` (VRS) replays and constructs the result, as today
(friend). `ReEvidenceJudge` takes the VRS `ExperienceRecord`, so it moves to
VRS with ReEvidence.

3.3 Bind's replay check (codex, evidence_gate.cpp:116-121). Bind holds an
`ExperienceJournal&` to replay each cited original and compare its record
digest. Options for codex to choose: (a) the decision already binds each
admitted original's record digest from admission (AdmittedEvidence), so Bind
compares against the accumulator's current originals without replaying; (b)
Bind takes a core function-ref `RecordDigestOf(address) -> DigestBytes`
that VRS supplies. (a) invents nothing; (b) keeps a second replay.

3.4 Publication authority. Today only MainOwner publishes (friend). With the
journal in VRS, the core keeps the rule by a capability: MainOwner issues a
`PublishCapability` (authority.hpp's existing IssueKey pattern) and
`JournalStore::publish` requires it. The VRS runtime owns the JournalStore
and MainOwner; MainOwner no longer constructs journal objects.

3.5 Memory and storage. Core types take `AllocationContext` (codex). VRS
supplies the counting resource (baseline and scaled profiles), the page
cache's separate resource (`AllocationRefused` → evict and retry) and the
storage budget port (`reserve`, `release`, `charged`; the reopen check
judged by the host).

## 4. Order (each step a pure move or a mechanical change, reviewed alone)

1. Codex: allocation.hpp and MainOwner/base types on AllocationContext
   (in progress, codex/swegca-allocation-integration).
2. Claude: journal/experience/evidence/cognition on AllocationContext;
   storage budget port; PageCache on its own resource. No relocation yet.
3. Claude: split the byte codec out of journal_format.hpp into core
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

- 3.3: (a) or (b)?
- 3.4: does `PublishCapability` belong in authority_roles (a new role) or is
  the existing Main lifetime token enough?
- Do SourceFamilies belong to VRS (they are Main's registry, used only by
  admission) or to the core (a grouping rule)? This plan puts the registry in
  VRS and the rule text in the core comment of EvidenceObservation.
- 5.4 (for the user if we disagree): is accumulation (the tally of admitted
  evidence) part of the verifier, or a VRS stage that hands the verifier a
  finished tally? This plan keeps it in the core because the verdict is a
  judgment of that tally and its binding (spec §4.4); moving it would leave
  the core a kernel over a struct.
- With authority and Main in VRS, 3.4's PublishCapability is a VRS rule; the
  core needs none (the verifier writes nothing).
