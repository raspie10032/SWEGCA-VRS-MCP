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
- The <1 ms Déjà vu → Recall transition and 1B-parameter seconds goals are
  separate product checks; this evaluation-path audit does not establish them.
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

The installed product wheel at this pass was the `0db5817` source candidate, SHA-256
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
all-size <1 ms Déjà vu → Recall transition or one-billion-parameter seconds goals.

## Fourth pass: read-path total and current wheel

The natural Recall path called `logical_record_count` and `logical_cue_total`
before Replay; each previously summed every shard on every query. Main now
keeps exact aggregate totals and updates them on shard attachment, shard
creation and generation refresh. This changes no candidate, region, portal,
original observation or Replay logic. A regression makes shard dictionaries
refuse value iteration during a query and checks totals after another ingest.

The first full-suite attempt used a Python environment with pytest but without
the product installed in its child-process environment: 124 tests passed and
the two stdio SDK tests could not import `swegca_vrs2.server`. A separate
offline environment installed the current product plus test dependencies, and
the complete suite then passed **126/126 in 182.85 seconds**. This is a source
and installed-test result, not a resident deployment result.

Current product source commit: `f1ea47a`. Offline-built wheel SHA-256:
`ade989360646b009d40031e45091d2043b3af8ee4ca0854cdd08817fba5ee285`.
Its separate installed runtime passed dependency and all 48 source/wheel/
installation file comparisons, with no extra installed package files. The
full isolated evaluation self-test passed on this wheel at
`/var/tmp/vrs22-whole-path-selftest-read-totals-20260922/receipt.json`, SHA-256
`1fd5faade3d045a64e6d017f2ebc8e00fc8301518475741f7b62fc1a988142ef`.
The 16 evaluation-grader regressions also passed. The one-shot SessionEnd
watcher is now `swegca-vrs22-sessionend-handoff-f1ea47a-20260922.service`;
the former `0db5817` watcher is inactive. The live marker remains absent,
so no active legacy state was converted and no VRS model cell was run.

The cached aggregate removes one identified shard-count-dependent operation.
It does not measure the all-size <1 ms Déjà vu → Recall transition or define
the user's one-billion-parameter unit. Replay can scale with matched candidate
count, but its timing is a separate diagnostic.

## Fifth pass: experience must locate the original instruction

The fourth-pass VRS evaluation prompt supplied the exact `memory:` address of
the first instruction. That made the four-stage Replay check valid but bypassed
the system's ability to locate the experience. It was a test-design fault:
successful exact-address Replay alone cannot establish retrieval of the
correct location from accumulated experience.

The VRS prompt now supplies only the `checkpoint` retrieval cue and the source
session identifier. It omits the exact experience address and omits
`exact_episode_id` from the MCP call. The model must use a natural Recall
through session VRS, receive a complete four-stage Replay, and retrieve the
original instruction's address and content from that packet. The runner keeps
the expected address privately for verification. Both runner and independent
grader reject a supplied exact address, a different query, main fallback, an
incorrect original Replay, missing session routing, or an incomplete receipt.
The grader still compares the full original instruction hash and provenance;
an unrelated result containing only the word `checkpoint` cannot satisfy it.

An installed-runtime integration test admitted the original instruction and
an irrelevant later user message to a fresh session VRS. The natural query
returned exactly one candidate, the original instruction, through the session
layer; main stayed unopened. Its Déjà vu, Recall, Replay and Re-evidence query
values were the cue, and the original Replay content/provenance checks passed.
The evaluation grader suite is now **17/17 passed**. The six frozen plain cells
were regraded unchanged: each still has ten real compactions and valid
measurement; five code scores are 4/20 and Terra 100k plain is 20/20. The
live preflight still reports `PENDING_LIVE_HANDOFF`, so no new model cell was
started.

The full isolated path self-test was rerun after this change and passed:
`/var/tmp/vrs22-whole-path-selftest-natural-20260922/receipt.json`, SHA-256
`fbbf5fd25c585880b88c1ae64abfb72a61d4f556296ca7e3f52add6f4be4a8d2`.

The cue tells the VRS arm the broad topic and is absent from the historical
plain probes. That difference must be disclosed with any comparison; the
rubric still requires the detailed instruction, diagnostic, scope and revision
to come from the original experience. Natural Recall at all main sizes remains
not measured here at the corrected Déjà vu → Recall transition boundary.

## Sixth pass: measured natural Replay and updated runtime

The repaired evaluation now depends on natural experience location, so that
product path was measured on the copied 15,630-record native main under the
specified 4 GiB RAM and 625 MB/s SSD read/write limits. The installed
`f1ea47a` path took a warm median 46.694 ms through Replay for 100 matches.
Source commit `7e35fb8` defers only per-cue strengths until cross-shard portal
construction; the same query took 15.152 ms. A single-match warm median was
0.196 ms, but its first call was 9.112 ms. These Replay diagnostics do not
measure the corrected <1 ms Déjà vu → Recall transition. Measurements are in
`docs/VRS2_2_NATURAL_REPLAY_BOTTLENECK_20260922.md`.

The new offline wheel SHA-256 is
`5d1a04bcd575a79956aa043bac479e3cd5705637794418b49f99c40aa7a62b97`.
All 48 product files matched source, wheel and installed runtime; the full
native suite passed **127/127**, evaluation grader **17/17**, and the isolated
evaluation path self-test passed at
`/var/tmp/vrs22-whole-path-selftest-natural-read-20260922/receipt.json`
(SHA-256 `dc78a3600248b1eca28243cc3445d2f001c358c2066e854d6a9252509ab7a191`).
The active one-shot SessionEnd watcher now points to this wheel; the previous
watcher is inactive. The armed session has no SessionEnd marker, resident native
main or handoff receipt yet. The model cells remain unrun.

The runner previously treated a successful handoff as sufficient to start
model calls. The historical runner used natural Replay times as a 1 ms
performance gate; that interpretation was wrong and has been removed. Replay
times remain diagnostics. Actual stage-transition evidence, all-size coverage,
and the one-billion-parameter seconds claim are separate readiness checks.
Preflight names the pending product-performance state. The independent grader requires the
run's product-performance audit. The evaluation grader regressions are now
**18/18 passed**.

## Seventh pass: the actual evaluation cell lifecycle

The earlier audit tested the installed hooks separately, but the isolated
100k+VRS cell's `CODEX_HOME` contained no `hooks.json`. Thus a successful
watcher self-test did not prove that the model cell would run the native
PreToolUse session router or the compact/turn ingress hooks. The evaluation
runner now generates its cell-local hook file from the exact installed product
wheel, points it at that cell's native VRS state, and invokes Codex with the
reviewed hook source enabled. The cell is otherwise isolated from the user's
Codex home. Because the measured conversation spans many `codex exec`
processes, the cell omits `SessionEnd` from this local hook file; the runner's
single finalizer publishes SessionEnd and attaches the temporary shards only
after the coding turn. The separately installed-product self-test continues to
exercise the full `SessionEnd` hook.

The new cell test executes the generated PreToolUse command, checks exact
`session_id` injection, and checks that no end marker was published. The CLI
argument regression checks all start/resume/fork forms and the hook trust
flag without a model call. A reread of the CLI source found only one
resume/fork subcommand; the apparent duplicate came from overlapping printed
source ranges and required no fix.

| Evaluation dependency | Current evidence |
| --- | --- |
| Frozen fixture, hidden tests, rubric, model cache, CLI binaries, product wheel | Static hashes and source/wheel/installed-file checks pass |
| Retained main seed and natural session-first lookup | Clone/fallback and complete four-stage Replay self-tests pass |
| Active ingress and hook routing | Two-session supervisor, installed full hook, generation lease, and new cell hook command pass |
| SessionEnd boundary | Active main unchanged; cell finalizer attaches only after its last turn; installed full hook separately passes |
| Model isolation and executable grader | Mount/guard self-tests and 20 grader regressions pass |
| Real compactions, token totals, scored code and dialogue | Gates and parsers inspected; three new VRS model cells still unrun |

The model-free whole-path self-test receipt is
`/var/tmp/vrs22-whole-path-selftest-cell-hooks-v2-20260922/receipt.json`,
SHA-256 `1c3b69bb8f4a0035be2123be3b20ff3e336502872f1152621d454f314708b7b8`.
All six execution checks in that receipt report PASS; the retained-main check
includes the new cell hook probe. The evaluation grader suite passed **20/20**.
The current preflight remains `PENDING_LIVE_HANDOFF`, and the independent
product-performance audit remains unready because its actual transition and
scale checks have not been established in that runner. No new model cell or compression comparison was run. Native
hook execution by a real Codex model cell remains to be confirmed by its raw
tool/hook trace after the live and performance gates pass.

## Eighth pass: first-query candidate and synchronized test runtime

Source commit `3e65550` compiles fixed lexical patterns during module loading
and skips portal enumeration when no portal pair was selected. It does not
change the original Replay, full portal projection, region membership or
cross-shard navigation. A copied-main first single-match query with its cue
table already read took 0.904 ms through Replay, while a cold table query took
2.500 ms. The installed wheel, SHA-256
`4b06afd976f3b983f37ba905fdb4b99eede691940733afa142952b0749992fe7`,
matched all 48 product source files and passed the complete native suite
**127/127**. The wheel's 100-match and 1,008-match Replay times are
diagnostics; live handoff and corrected transition checks still controlled
model evaluation.

The VRS-only runner and grader now pin that same product commit, wheel and
new cgroup performance receipt. Evaluation grader regressions passed
**20/20**. The installed-wheel whole-path self-test passed at
`/var/tmp/vrs22-whole-path-selftest-first-query-20260922/receipt.json`
(SHA-256 `cba977013e785b2cb12c9dc3a5ed783d3e9fe46b0ed6fc9203a5b3c9a9979f6e`).
The one-shot post-SessionEnd handoff watcher now runs from this wheel; the
previous watcher is inactive. The current Codex task has no SessionEnd marker
or native resident main, and no model cell was run. Product source, evaluator
and future handoff are version-aligned, while full performance acceptance
remains unproved and contradicted by the measured multi-match cases.
