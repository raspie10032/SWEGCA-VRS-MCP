# VRS2 issues 1–3 — execution ledger, 2026-09-14

Status: cross-platform candidate verified; #3 real-store scale remains open.
Base: 7395deb300e54bbea85237924eed64de5359c7ca (v2.1.0).
User authorized addressing all three issues. No issue comments or messages to
other people are authorized. Work in the existing public repository only.

## Implementation contract and current-turn audit

I1. A single main owns identity, persistent experience, VRS, arbitration and
authority. Shared Windows clients transport requests to that owner. Checkpoints
preserve the settled generation; journal entries remain the recovery source.
I2. Internal and final model calls are zero. E2B, including temporary replacement
of the 135.5M specialist, remains replaceable and outside this memory task.
I3. All original addresses, source revisions, six outcomes and related opposing
claims remain accessible. Compression and derived navigation never delete or
hide evidence, establish truth, or add independent observations/reinforcement.
I4. Each request pins one immutable full-current snapshot; writes atomically
publish successors. Model replacement retains main-owned state and connections.
I5. This is software correctness/latency/regression work, not growth. Growth
requires new outcome-bearing experience and longitudinal assimilation, later
source-diverse change, retention and correction. Synthetic/repeated rows and
consolidations are not independent new experience; model metrics are subordinate.
I6. Preserve held-out isolation and ordinary cognitive access. Observation and
maintenance permissions are explicit; World/action/belief/model/distribution/P3
authority is separately gated. No Hermes, source-data uploads, private live-main
changes, speech/LoRA, GPU work or full live VRS runs. The specifically requested
Windows MCP checkpoint repair does not resume private-main cold optimization.

1. yes — I1 assigns all persistent ownership to one main, including shared clients.
2. yes — I2 bounds E2B/135.5M to replaceable specialists and runs neither.
3. yes — I3 retains every original address/outcome and explicit opposing evidence.
4. yes — I4 freezes one request only; new experience uses an atomic successor.
5. yes — I5 requires actual new outcome-bearing experience for growth claims.
6. yes — I5 requires system-longitudinal evidence; current tests are diagnostics.
7. yes — I1/I2/I4 keep identity/experience/connections independent of models.
8. yes — I6 retains held-out and consequential gates without limiting I3 access.

AGENTS preflight: PASS

## Validation planned before closure

- #1: active call path documentation and unrelated-component sharing test.
- #3: real multi-client owner/stdio lifecycle, checkpoint and journal-tail
  recovery, corruption and failed-commit behavior, lossless compaction, exact
  numerical/reference comparison, Korean retrieval and explicit conflict closure.
- #2: repeatable derived groups, exact parent access, revision/conflict exposure,
  retroactive consolidation, snapshot-bound pagination and restart.
- Installed wheel on Windows 3.11/3.12 and Linux, archive exclusion, CPU scale
  diagnostic with long overlapping Korean records; no claims about private data.

Resource preflight: approximately 22 GiB available RAM, 762 GiB free /var;
CPU only, test/benchmark artifacts initially budgeted below 2 GiB RAM/1 GiB disk.
Use /var/tmp for artifacts (prior /tmp quota failures preserved).
Issue #3 local-windows-scale branch and commits are not on the public remote;
no open PR at inspection. Its performance/equivalence claims are reported
external measurements, not verified upstream acceptance.

## Rozephine의 판단
No new real-world cognitive judgment. Existing observations remain unmodified.

## Codex의 판단
Implement and verify the three issue contracts without conflating numerical
settlement, topology, disk compression or derived experience consolidation.

## Codex 작업 실수 및 교정
Initial combined contract/canonical output exceeded the tool output budget;
continued with bounded reads before preflight or implementation. No code/state
mutation occurred. Audited scope, assumptions, evidence and authority boundaries;
no new completion claim. Prior release limitations and failures remain recorded
in VRS2_STANDALONE_LEDGER.md and are not retroactively erased.

## 2026-09-20 implementation checkpoint — local source, not released

The public issues #1–#3 were read in their current open state. The existing
`codex/vrs2-issues-1-3` worktree already contained uncommitted `store.py`,
`numeric.py`, and `checkpoint.py` changes. Exact pre-edit bytes and SHA-256 for
those files and this ledger were copied to
`/var/tmp/vrs2-issues-recovery-20260920/`. No user store, private main,
model, or external issue comment was changed.

Initial `tests/standalone --maxfail=3` stopped with three identical setup errors:
`ModuleNotFoundError: swegca_vrs2.consolidation`. The two unfinished imports
were temporarily detached; original 21 standalone tests then passed. The
missing functionality was implemented afterward, without treating this
temporary green baseline as acceptance.

- #3: a loopback-only resident main and authenticated per-connection MCP bridge
  now let separate stdio clients share one `Main` and one state directory.
  Per-connection read-only catalog remains read-only even when main allows
  ingestion. Main calls are serialized; no external listener is opened.
  An on-demand bridge startup and two concurrent real SDK stdio clients passed
  locally on Linux. Actual Windows execution remains unverified.
- #3: checksummed data-only checkpoints retain the journal as source of truth.
  Clean shutdown, an eight-new-record background checkpoint, uncheckpointed
  tail replay, corrupt-checkpoint rejection, explicit journal rebuild, failed
  checkpoint COMMIT and exact source restoration passed local tests. A mixed
  Hangul/digit lookup vanished after checkpoint restore before repair; decode
  now reconstructs retrieval postings from original text. A mixed-case
  proposition ID failed to bring its opponent into recall; postings now use
  normalized episode cues. Both original failures were retained as regressions.
- #3: the pre-existing guarded vector settlement and sequential unboxed region
  adapter were retained. Twenty seeded 40-node directed graphs matched the
  native f32 reference bitwise for candidate scores and pending state. This is
  bounded evidence, not equivalence for every graph or a 2,620-record store.
  Main now orders already-recalled candidates by query-word coverage and
  fanout-weighted score, while preserving every candidate, replay, judgment,
  original address and explicit proposition opponent. Ranking grants no truth
  or action authority.
- #2: explicit offline maintenance can create a deterministic, immutable
  derived navigation group from two or more exact parent addresses, bound to
  one pair snapshot. It does not add an observation, VRS edge, factual summary,
  promotion or authority. Recall by the group ID expands exact parents and
  displays current revision/conflict state; originals remain directly readable.
  Idempotency, historical original access, retroactive grouping, restart and
  corruption rejection passed local tests. `compact` converts old observation
  rows to lossless compressed blobs and preserves the full journal; two-source
  CLI round-trip passed. This small test is not a large-store space estimate.
- #1: README now names the active `memory_store -> Main.ingest -> Graph.append`
  path, both `prepare_event_delta` calls, affected numeric propagation,
  component-level region rebuild, unchanged component sharing, and the
  changed-component receipt diagnostics. The pre-existing unrelated-component
  assertion remained in the standalone suite.

Bounded performance diagnostic on this Linux source branch, CPU only: 20
synthetic overlapping records of 499 characters had median ingest 106.353 ms,
last 155.424 ms, clean restart 22.199 ms. A separate 60-record/749-character
overlap run had median 274.949 ms, last 461.206 ms, maximum 514.751 ms,
885 nodes/84,960 edges, clean restart 80.339 ms. A 40th-ingest profile attributed
0.327 of 0.658 s to numeric settlement and 0.073 s to region construction.
These inputs are synthetic and small; they neither refute nor reproduce the
issue author's 2,620-record Windows measurements. The cost still grows with
shared cues. No real-store latency or memory target has passed.

### Rozephine의 판단

No new real-world main judgment or growth run occurred. Derived groups and
ranked records remain source-bound navigation. Historical outcomes and current
conflicts remain distinct; neither grants action or durable belief authority.

### Codex의 판단

This is an unreleased candidate. Linux local correctness checks pass for the
bounded paths above. Windows installed-wheel/host CI, large overlapping Korean
store behavior, package archive, crash between SQLite commit and checkpoint,
security review of the local endpoint, and any final issue-closure claim remain
pending. The present synthetic trend does not justify closing #3.

### Codex 작업 실수 및 교정

The first baseline run printed repeated copies of the same missing-module setup
error and exceeded the output budget; subsequent failures used short tracebacks
and narrow tests. A new consolidation test compared a frozen tuple to a list;
the assertion, not the product, was wrong and was corrected before rerun. Scope,
assumptions, interventions, evidence, authority and completion claims were
reviewed: no training, private-main operation, public comment, release or
growth claim was made. Earlier negative evidence remains in the preceding
ledger and tagged validation report.

### Local candidate validation before cross-platform CI

The candidate version is `2.2.0.dev0`, separate from the published v2.1.0
wheel and tag. Source `tests/standalone`: 52 passed / 4.64 s. A fresh isolated
environment imported the installed wheel from `site-packages`; its full
standalone suite: 52 passed / 5.14 s. `uv build` and
`tools/verify_standalone.py --dist /var/tmp/vrs2-issues-build-final-20260920`
passed with 12 unchanged native port files and zero Hermes files in both
archives. Final candidate wheel: 83,048 bytes / SHA-256
`408c36d50ce75cadf7f13823d55a6f71f7efec343985d0233245c6a59f0fdcad`.
Source archive: 180,585 bytes / SHA-256
`7298cf091cc9d92a90f1027e6acab87227612a6e3e3323213c9b7c463e853767`.
UTF-8 no BOM checks and `git diff --check` passed. These local checks do not
substitute for actual Windows CI or the reported 2,620-record real store.

### Codex 작업 실수 및 교정 — local validation

The first ledger append patch used a line break not present in the file and
failed without mutation; the exact tail was read before reapplying. The first
artifact command assumed `python -m build` was installed in the local venv;
it was not, so `uv build` was used and actual archives verified. A first
installed-wheel run preceded the final security and rollback regressions;
the wheel was rebuilt and the full 52-test suite repeated from that final
artifact. No earlier test count was added to the final count.

### Cross-platform installed-artifact CI — candidate commit 7ccff95

Workflow run [35455043726](https://github.com/raspie10032/SWEGCA-VRS-MCP/actions/runs/35455043726)
completed successfully on the exact pushed commit
`7ccff95a7a4092f1c012ff776c6dc749b19d8fec`: Windows/Python 3.11
52 passed in 7.51 s, Windows/Python 3.12 52 passed in 9.28 s, and
Ubuntu/Python 3.12 52 passed in 7.76 s. Each job built and validated archives,
installed its built wheel, and ran the standalone suite from that installation.
Both Windows jobs also ran the installer and checked the generated Claude
executable path. This supersedes the earlier checkpoint's pending Windows-CI
status; it does not verify a real Claude UI session or the issue author's
2,620-record Windows store.

The candidate remains `2.2.0.dev0` on `codex/vrs2-issues-1-3`. #1's active
path and topology/numeric distinction are documented; #2's derived group is
implemented and covered by installed-artifact tests. #3's resident owner,
checkpoint, compaction, and retrieval corrections pass bounded tests, but its
real-store latency, memory footprint, and large overlapping Korean retrieval
remain unmeasured. Keep #3 open; do not infer a production-scale pass from the
52-test matrix or synthetic 60-record diagnostic. No stable release or private
main change was made.

### Codex 작업 실수 및 교정 — CI checkpoint

Audited scope, assumptions, interventions, evidence, authority, and completion
claims for this checkpoint. The preceding paragraph previously said Windows CI
was pending; the run now supersedes that dated statement. No new Codex error
or corrective code mutation occurred during CI observation. The remaining
real-store evidence gap is stated explicitly rather than described as a pass.
