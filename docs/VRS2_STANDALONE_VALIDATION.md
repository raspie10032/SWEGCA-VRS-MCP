# VRS2 standalone Windows validation — 2026-09-14

Implementation and Windows acceptance are complete for the standalone memory
MCP described here. This report precedes final main/tag publication. It does not
claim the whole Rozephine application, speech work or Claude UI was completed.

Tested source: `e99ddb829cf1fd928049afc2289a471506c7f476`.
Successful CI: [34804114244](https://github.com/raspie10032/SWEGCA-VRS-MCP/actions/runs/34804114244).
The final publication adds this report and ledger evidence; runtime source stays
identical to that tested commit.

## Delivered implementation

- A local main owns persistent identity, observation journal, immutable hot
  memory generations and VRS2 state. The MCP runs in the same process on Windows
  or Linux, with no Linux main, Unix socket, WSL, GPU or model requirement.
- Native first-party four-stage activation, event-signal arithmetic, sparse
  numeric successors, re-evidence strength/promotion types, overlapping
  connectivity regions and evidence paging are included. Twelve modules or
  selected definitions have exact source/AST equivalence to the original source;
  NATIVE_VRS2_PORT.json records per-file hashes. This is a bounded component port
  plus a new standalone text-ingress composition, not the whole private runtime.
- `memory_store` validates explicit local observation permission, records original
  text/source/revision/outcome/metadata, handles idempotent retries and commits a
  durable successor before publication. Identical observations do not inflate
  experience count or reinforcement. Restart validates and restores the journal.
- Query uses all matching lexical keys, exact original addresses and complete
  explicit proposition closure. No arbitrary top-k or evaluator allowlist limits
  memory. Original conditions, uncertainty and all six outcomes remain available.
  Receipt pages preserve the four stages, candidate/replay/verdict identity and
  current snapshot; a previously opened view remains pinned across new ingress.
- Main applies explicit source-bound re-evidence strength changes once per
  ingress. Numerical settlement does not repeatedly reinforce the same evidence.
  Opposing claims prevent reinforcement; same-source explicit retraction weakens
  the old connection while retaining its original. VRS promotion is derived from
  current strength, never supplied as client authority.
- Wheel and source archive contain zero retired external-agent adapters or
  historical swegca_vrs_mcp package files. Historical code/data remains preserved
  in the repository and earlier releases, without automatic migration.

## Actual installed-artifact tests

| Environment | Checks | Failures/errors/skips | Test time |
| --- | ---: | --- | ---: |
| Windows hosted runner, Python 3.11 | 21 | 0 / 0 / 0 | 4.051 s |
| Windows hosted runner, Python 3.12 | 21 | 0 / 0 / 0 | 4.397 s |
| Ubuntu hosted runner, Python 3.12 | 21 | 0 / 0 / 0 | 3.291 s |

Each job built archives, checked native source hashes and archive contents,
installed only its built wheel, then ran the tests. Tests use actual MCP SDK
stdio child processes in both protocol modes and real standalone main logic.
They cover six outcomes, exact original text/provenance, process restart and
stable identity/snapshot, conflict on another page, explicit revision retention,
direct address access, duplicate recording, single-owner lock, read-only mode,
forged authority rejection, stale query rejection, rollback on failed COMMIT,
corruption rejection, hot cognition I/O/model blocking, large-source continuation
and cleanup, native strength updates and independent dense numerical reference.

Both Windows jobs additionally ran the PowerShell installer, launched the actual
console executable and verified the generated Claude configuration path exists.
No Claude application UI/account was exercised. This is Windows installation and
MCP compatibility evidence, not a claim of a completed Claude conversation.

Release wheel selected from the successful Windows Python 3.12 job, without
rebuilding its bytes: `swegca_vrs_mcp-2.1.0-py3-none-any.whl`, 67,957 bytes,
SHA-256 `2793f3a8090bae28063ce635cc9626a87c1c73e43efe83ac429fb27a1b463342`.
Final source archive includes this report; its hash is listed separately in the
release SHA256SUMS file to avoid a self-referential archive/report hash.

Local installed-wheel checks before the final address regression: 20 passed in
1.76 s; final source suite: 21 passed in 1.40 s. These overlap the CI checks and
must not be added together as distinct tests. All 34 historical upstream port
files also retain their exact hashes. Legacy source remained untouched: first
run 216 passed and 3 SQLite disk-I/O failures under /tmp; only those three failures
were repeated under /var/tmp and passed in 0.78 s. The failed run is retained.

## Latency and resource scope

One CPU-only Linux/Python 3.12.14 diagnostic ingested 50 distinct synthetic source
records sharing a connected vocabulary. Median ingress 22.666775 ms; maximum
41.132150 ms; one context page 5.816108 ms with 50 candidates and three returned
records; restart/journal replay 1098.196591 ms. Clocks used perf_counter_ns.
This small diagnostic is not a large-corpus guarantee or a growth experiment.
Restart replay and affected-component topology cost increase with stored work.

The existing native service stayed active with PID105583. No native restart,
existing experience migration, whole live VRS rerun, initialization optimization,
speech/LoRA/model execution, GPU service change or E: write occurred.
Private source experiences, models, credentials and runtime state were not
published. Synthetic test stores remain local test data, not production memory.

## Limits and use contract

The numerical version remains
`vrs-re-evidence-event-signal-f32-v2-experimental`. Literal text/Hangul cues and
explicit proposition comparison do not claim general language understanding,
logical entailment from connectivity or verified factual independence of source
addresses. Recorded agreement is not independent factual corroboration.
Memory is external evidence; historical outcomes and client text are not
instructions. There is no automatic transcript capture or old-store migration.
One process owns a state directory. Use the Windows instructions and retain the
state directory when upgrading the application environment.

## Rozephine의 판단

Standalone main produced source-bound four-stage receipts, preserved opposing
claims and prior revisions, and settled numerical VRS events without an LLM.
Available/retained experience is not a certification of current-world truth.
World, action, persistent-belief, model-update, distribution and P3 authority
remain separate; the MCP exposes no executor or belief-commit bypass.
The existing live Rozephine main was not replaced or evaluated by this release.

## Codex의 판단

The previously missing Windows-local main, observation ingress, persistence and
artifact separation now have implementation and actual Windows evidence. The
new release is suitable for the user's independent Claude Desktop test using
the supplied installation/configuration instructions. General cognitive growth,
all private-runtime features, large-scale latency and actual Claude UI remain
outside this demonstrated acceptance; they are not inferred from test counts.

## Codex 작업 실수 및 교정

1. The v2.0 extraction narrowed the user's standalone intent to a bridge and
   checked external-agent execution, not archive exclusion. The user's repeated questions
   exposed the omission. I also ended turns after explanation/status rather than
   implementing the authorized correction, making the user repeat the request.
   This correction adds the local runtime and explicit requirement-to-evidence
   gates. The old release and its narrower evidence remain preserved.
2. The first extraction named the wrong native delta function; corrected to
   prepare_event_delta. Initial page integration supplied a mutable root and
   mismatched cursor-release arguments; corrected before the passing tests.
3. Initial source packaging retained historical external-agent docs/tools. Archive
   inspection failed; explicit pruning and selected release inputs corrected it.
   The failed artifacts were retained locally and were never released.
4. First actual Windows run 34804004050 built archives but failed native source
   hashes because Git checkout converted LF to CRLF. I had omitted eol rules.
   Added .gitattributes and kept exact hash checks; succeeding run 34804114244
   validates both Windows versions. The first failure is not counted as success.
5. Legacy SQLite tests had three temporary-directory I/O failures; their exact
   cause was not initially proven. Later CI artifact download explicitly failed
   with /tmp disk quota exceeded. Command-local TMPDIR=/var/tmp recovered the
   download without deleting unrelated data. This supports a temporary-storage
   explanation but does not prove every earlier I/O failure had that cause.
6. Combined document/log output caused truncation and extra bounded reads.
   No production-state mutation resulted. Required scope, evidence, authority and
   completion claims were audited; prior speech/LoRA failures and incomplete
   full-system goals remain unresolved by this memory release.
