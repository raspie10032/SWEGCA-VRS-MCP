# SWEGCA core and VRS layer: relocation plan

Status: plan for review (claude, 2026-09-23). Nothing here is moved yet.

Rule (user 2026-09-23 16:24 via codex 16:23): the SWEGCA core is a set of
logic elements; SWEGCA-VRS and the four memory stages (Déjà vu, Recall,
Replay, Re-evidence) are the higher system built by combining them. Usage
counting and budget judgment belong to VRS (user 16:10); the baseline
resource profile is a floor, not a ceiling (user 16:13).

What does not change: the author's logic and the design board's rules, every
byte format, every failure code, and every test condition's expected result.
Only where each piece lives and which side of the boundary may name which
type change. Build and test gates stay closed (board §10 steps 9-10).

## 1. Classification

An element takes inputs and gives a judgment, a transition, or a value. It
names no file, directory, journal or index, and counts no resource.

| Module (today, `cpp/swegca_architecture/`) | Layer | Reason |
|---|---|---|
| digest_bytes, sha256, strong_types | core | values and identity rules |
| allocation (codex 408d4e2/3923503) | core | the abstract allocator the host supplies |
| byte codec (`ByteReader`, `ByteWriter`, `LedgerBytes` aliases, now in journal_format.hpp) | core | canonical encoding used by elements (cognition's AutonomyControl, envelopes); split out of journal_format |
| authority, authority_roles, role_registry | core | authority rules |
| cognitive_state, native_tensor | core | state and its transition |
| judgment_kernel, judgment_rules | core | pure kernels |
| evidence_accumulator: tally, admit step, Re-evidence record step, decide, EvidenceDecision, ReEvidenceResult | core | accumulation and decision (spec §4.4) |
| evidence_gate / Bind, proposal, arbiter (codex) | core | spec §4.5-4.6 |
| cognition | core | autonomy transition kernel |
| main_owner (authority, lifetime) | core | Main's authority; see 3.4 for what it stops owning |
| journal_file_io, journal_position, journal_format (record, manifest, page formats), journal_store (segments, manifest, HEAD, view trees, page cache, storage port) | VRS | storage and lookup |
| experience: envelope, part tree, ExperienceJournal, index views, CueTokens, ExperienceAppend | VRS | experience storage and Déjà vu / Recall retrieval |
| experience: ExperienceSelector, VerdictSink, SelectionReceipt | VRS | Recall (the judge it calls is Rozephine's runtime cognition, passed in) |
| evidence_accumulator: EvidenceAdmission, ReEvidence, SourceFamilies | VRS | Replay then admission, and Re-evidence: stages that combine journal replay with core steps |

Déjà vu = cue/index retrieval (views over the journal); Recall = Select with
the judge and its receipt; Replay = exact record and part streaming;
Re-evidence = ReEvidence against the current state. All four are VRS.

## 2. Target layout

- `cpp/swegca_architecture/` (namespace `swegca::architecture`): core only.
  No header here includes a VRS header (checked by the lineage gate, 4.3).
- `cpp/swegca_vrs/` (namespace `swegca::vrs`): journal, experience, the four
  stages, the host's counting allocator and storage budget, and the runtime
  that composes Main.

## 3. Boundary types (minimal, from current dependencies)

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
4. Claude: `git mv` journal_*, experience.* into `cpp/swegca_vrs/`, namespace
   `swegca::vrs`; EvidenceAdmission, ReEvidence, SourceFamilies into
   `cpp/swegca_vrs/evidence_stages.*`. Includes and qualifiers only; the
   lineage gate checks every moved definition keeps its tag.
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
