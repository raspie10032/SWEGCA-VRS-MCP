# VRS 2.2 evaluation path audit — 2026-09-22

The model run is still gated. This audit checks the complete path that would
produce and grade the three new 100k, medium-effort VRS cells. The six frozen
plain controls remain historical evidence; no model was called for this audit.

| Path | Concrete check | Current result |
| --- | --- | --- |
| Fixture and model inputs | Frozen coding tree, hidden-test/rubric hashes, pinned Codex binary and model cache | Preflight passes these checks |
| Native product install | Wheel hash, 48 installed package files, prohibited dependency/import scan | Pass; no SQLite/Hermes/filelock dependency in the installed package |
| Existing main experience | Copy only native primary, automatic shards and completed linked shards with reflinks; compare every copied file hash against a stable source | Synthetic primary, automatic shard, linked shard and 15,630-record offline clone pass |
| Exact main read | Prepare exact Replay and read projections, then force a session miss through LayeredMCP and require main Replay plus all four stages | Pass on the 15,630-record offline clone and a second evaluation-cell clone |
| Live ingress | Installed lifecycle hook and separate transcript tailer admit complete public records to session VRS; private encrypted reasoning is explicitly excluded | Two-session watcher and installed-hook selftests pass |
| Active/ended boundary | Compare the original main generation with its baseline; active sessions write temp only, SessionEnd adds only the new completed shards | Populated-main selftest passes: 1 original, 2 temp, then 2 newly linked |
| Evaluation isolation | Model workspace, Codex home, auth mount, shell guard, hidden tests and sibling-cell access | Bubblewrap/Landlock selftest passes |
| Compaction evidence | Original-thread `compacted` JSON events, ordered windows, response IDs and >=10 minimum | Runner and grader checks inspected; model VRS cells not yet run |
| VRS model use | Session routing, exact address, status/context/release, session-first receipt and four-stage receipt at each probe | Static gate and grader regressions pass; model calls pending |
| Code grading | Hidden duplicate metadata/array tests, original regression, complete fixture diff, code quality and response consistency | Ten grader regressions pass; historical plain cells regraded unchanged |
| Tokens | Unique provider response records, compaction subset, cache/read/write, output and reasoning output | Extraction and validity checks inspected; new VRS totals pending |

## Corrections made during this audit

1. A failed code solution is now a measured quality outcome. Measurement
   validity depends on compactions and protocol, while executable success is
   reported separately. This preserves bad model results instead of discarding
   them as invalid runs.
2. The changed-file check now compares the entire fixture tree. Newly created
   subdirectories and symlinks are detected; a model cannot hide a scope change
   outside the previously enumerated package/test directories.
3. If the first Codex call fails after creating a session, cleanup still runs
   the isolated session finalizer and saves a cleanup receipt.
4. Each VRS cell now starts with the retained native main experience. The
   source clone checks native primary, automatic shards and completed linked
   shards; active session directories are excluded. A non-assimilating original
   fallback probe must succeed before any model call. The seed is stopped and
   frozen before cell cloning.
5. The fallback probe waits for exact Replay and current VRS projections to
   finish. Without this gate, a newly cloned 15,630-record main returned zero
   candidates despite holding the original episodes.
6. Session accounting and grading distinguish the original main baseline from
   newly captured temporary records and newly linked SessionEnd records.
   Comparison output now has an explicit measurement eligibility flag.

## Evidence and limits

- Full isolated path selftest: `/var/tmp/vrs22-whole-path-selftest-20260922-final/receipt.json`, PASS.
- Grader regressions: `10 passed`; `git diff --check` passes.
- Offline retained-experience probe: 15,630 original records, seed and cell
  clone both returned main fallback and complete four-stage Replay.
- Six frozen plain cells regraded without model calls: all had ten actual
  compactions and valid measurement traces; five code scores were 4/20, one
  Terra short-plain code score was 20/20.
- The real Codex task is still active. Its SessionEnd handoff has not occurred;
  the resident state still has the legacy database and old hook/config. The
  VRS-only model run remains blocked by the live preflight. No live migration
  or model comparison is claimed here.
- The hard <1 ms Replay and 1B-parameter seconds goals are separate product
  performance gates; this evaluation-path audit does not establish them.
- Automated answer/code text scores are preliminary. Final quality judgment
  requires review of raw answers, diffs and executable test evidence.
