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

## Second pass: complete evaluation inputs and scoring

The second pass checked the runner, private controls, model fixture, hidden and
visible tests, isolated Codex home, shell guard, mounted runtime, native
experience clone, live watcher, SessionEnd finalizer, telemetry extraction,
grader, comparison output, deployment watcher, and the instructions here.
It found and corrected six evaluation faults:

1. The model mount could read the resident host VRS state and the private plain
   control archive. Both are now hidden behind empty mounts in every cell; the
   process ID namespace is also private, and host runtime sockets are hidden.
   The independent mount test verifies
   that the model can still read its fixture and test Python while those paths,
   sibling outputs, hidden tests, and host sessions remain inaccessible.
2. The rubric's keyword scorer gave 100/100 goal and context scores to an
   answer that explicitly reversed four requirements. Direct negations now
   score zero on the contradicted items, with an adversarial regression test.
   The result schema marks all automated text/code scores as provisional and
   requires review of every boundary answer, final messages, full diff and
   executable test log before final quality comparison.
3. The runtime check compared wheel bytes to installed files but did not
   compare the wheel to product source or reject extra installed Python files.
   Preflight now requires all 48 package files to match source, wheel, and
   installation, with zero extra installed package files.
4. A generic PASS handoff receipt could satisfy the SessionEnd gate without
   proving it came from the armed task. Preflight now checks the receipt schema,
   armed timestamp, exact ended session, merge status, and ended timestamp.
5. The grader executed model-edited Python directly with host filesystem and
   network access. It now runs each visible/hidden test in a private mount,
   process and network namespace, under the read-limited shell guard, with a
   minimal environment. An isolation regression runs Python inside that view
   and verifies host experience, plain controls, sibling files, and the current
   task identifier are unavailable. Sandbox startup failures halt grading
   instead of counting as model code failures.
6. The runner hashed frozen tests and the rubric, while the later summarizer
   did not verify them before grading. The summarizer now checks the exact
   fixture tree, both hidden tests, shell guard, and rubric hashes and records
   those hashes in its output.

The source commit used for the new product wheel is `97065d3`, and its wheel
SHA-256 is `5955caf16f340ca9cd3c723eca29771e24a8984ccfdda8d1d70c962328d84e8a`.
The full isolated path self-test passed with this wheel:
`/var/tmp/vrs22-whole-path-selftest-isolated-final-20260922/receipt.json`, SHA-256
`b07a82a46a727113e79cbc49bd482032503936c6f7d3d03cbc100fb54f2afc34`.
Fourteen grader regressions passed. Regrading the six frozen plain cells inside
the new isolated grader produced
the same measurement validity, ten actual compactions each, and the same
executable code scores (five 4/20, one 20/20). No model call was made.

The token price constants were checked against the [official OpenAI pricing
table](https://developers.openai.com/api/docs/pricing) for standard short
context processing. Cost remains a list-price equivalent, separate from
recorded token counts and any account charge. The real resident preflight is
still `PENDING_LIVE_HANDOFF`; its SessionEnd receipt is absent, and the active
legacy state has not been migrated. A single one-shot watcher now points at
the new wheel and will act only after that actual SessionEnd. No VRS model
comparison is available yet.

## Third pass: original content across the entire test path

The installed product wheel is now the `0db5817` source candidate, SHA-256
`b99c1cbd4f8f51f3ff57836706db92ae2838554a90fb03c9d2844f29bcfed7c2`.
The test code was checked from ingress through scoring, including the frozen
fixture, hidden tests, Codex process/mounts, installed MCP/hook, transcript
watcher, session VRS journal, layered read, Replay packet, SessionEnd attach,
compaction events, token records, isolated executable grader and plain controls.

Two additional false-pass paths were closed:

1. A model-visible VRS packet could have the right address and four-stage
   receipt while its original Replay content was absent, deferred or different.
   The runner and the independently parsing grader now require one complete
   Replay whose original text hashes to the frozen first instruction, whose
   source is the current Codex session transcript, whose role is user, and
   whose historical content has no truth or action authority. This is checked
   at every boundary, recovery and coding VRS use. A real installed-wheel
   session-first four-stage Replay of the full 837-character instruction passed
   this content check. Wrong text, wrong source and deferred Replay fail the
   regression test.
2. The former session coverage check counted original observations but did
   not compare their content. After actual SessionEnd in each evaluation cell,
   the runner now reconstructs every complete host-visible ingress record,
   including chunk identity and provenance, and compares its normalized
   observation envelope with every original VRS journal observation by stable
   request ID. This is an audit only; model recall still uses VRS experience.
   The grader requires equal counts and SHA-256 digests, with zero missing,
   unexpected or changed requests. Deliberately altering the transcript after
   VRS admission makes the audit fail.

The full isolated self-test passed with two concurrent sessions: 6 expected
original observations equaled 6 session VRS originals after normalization,
with 2 private reasoning records explicitly excluded. The populated-main test
likewise matched 2 of 2 session observations and preserved its original main.
The complete receipt is
`/var/tmp/vrs22-whole-path-selftest-complete-20260922/receipt.json`, SHA-256
`88da95e8eb34f56d8e6322018b4848e56e3d79492e3fadcec719b24bd43ec2be`.
All 16 evaluation-grader regressions passed. The six frozen plain cells were
regraded in the current isolated grader: each remains valid with ten actual
compactions; five still score 4/20 code quality and Terra 100k plain still
scores 20/20. No new model call was made.

The current live preflight remains `PENDING_LIVE_HANDOFF`: the armed SessionEnd
receipt and native resident main do not yet exist, and the old installed
config/hook/database are still active. The one-shot post-SessionEnd watcher is
running. The three new VRS model cells must remain unrun until that handoff,
then the same full-path gates and manual answer/diff review must pass. This
source and isolated evaluation evidence does not establish the unconditional
all-size <1 ms Replay or one-billion-parameter seconds goals.
