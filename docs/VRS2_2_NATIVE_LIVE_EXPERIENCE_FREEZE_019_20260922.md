# VRS 2.2 native live-experience evaluation: v019 readiness

This is a **software preflight and self-test receipt**, not a scored v019 model
run. Do not start the three VRS cells until the current Codex task has ended
and its SessionEnd handoff has preserved and moved the resident experience to
native VRS. The runner now rejects a full run until it finds the actual
SessionEnd handoff receipt, native resident main, installed MCP and hooks, no
old database/upgrade marker, and a four-stage Replay of one retained original
main experience. Verify live ingestion and session-first/main fallback in the
new task before accepting any score.
Merging the main experience during the active task would violate the frozen
session lifecycle.

## Frozen inputs and isolation

- `tools/run_vrs22_compaction_stress_v019.py` exercises **only** 100k+VRS
  cells for Luna, Terra, and Sol, with medium reasoning, 12 planned real Codex
  compacted events and at least 10 required in each cell. The six frozen v005
  plain cells remain the controls (100k and 250k). Each VRS cell receives a
  verified clone of the retained native main experience, then accumulates its
  own task in live session VRS. A session miss must replay retained main
  experience through all four stages before the cell starts.
- Clean coding fixture tree SHA-256:
  `6b35f73d7241620a43e30811b86811a8a1ec5a92e7ffa4e5f295054817cb7b0c`.
  The fixture, its visible regression, its hidden tests, its clean Python
  environment, and the grader use no SQLite, Hermes, or external filelock.
  Plain output was separately revalidated against this clean fixture:
  `evals/vrs22_context/results/plain_clean_validation_005.json`, SHA-256
  `9d1e39bbc7ba7d10bfe3970f17bf5ddc28660e3f2cc21f9477dc785bed1c7b36`.
- The six historical plain controls are also archived privately at
  `/home/raspie/.local/share/vrs22-eval-runtime-v019/private/plain-v005-control-evidence.tar.zst`.
  SHA-256: `e817623972c8161d3f9985811465d4f084977d3c781d0bb79088560f315ae9bb`;
  size: 5,532,766 bytes. The archive retains result events, messages, exact
  model-edited code, and 72 session JSONL files. It excludes old VRS arm
  directories, authentication, and Codex internal database state. Independent
  reads of those session files found 10 or more ordered real `compacted`
  events in every plain cell. The summarizer restores and hash-checks this
  archive automatically if the old `/var/tmp` tree disappears.
- Historical plain workspaces contain the retired storage substrate. The
  summarizer copies only each model's unchanged `checkpoint.py` output onto
  the clean v019 fixture for executable grading. This prevents old substrate
  files from being counted as model edits or imported during grading. All six
  restored plain cells passed measurement validation; clean executable code
  scores were 4/20 for five and 20/20 for Terra 100k plain. The sanitized
  receipt is `evals/vrs22_context/results/plain_restored_validation_005.json`,
  SHA-256 `13d20ec66941fc2ef4eb15ba1c5461b8a91473d649c1952d8fc56ee7005eb274`.
- Runtime source provenance: `7e35fb8`; frozen candidate wheel SHA-256:
  `5d1a04bcd575a79956aa043bac479e3cd5705637794418b49f99c40aa7a62b97`.
  Preflight verified all 48 installed package files against that wheel and
  found zero forbidden source references or dependencies. Installed files also
  match the product source, with no extra installed modules. A subprocess import
  audit found neither `sqlite3` loaded nor `filelock` available in either the
  runtime or the isolated grading environment.
- The frozen Codex model-list cache has a potentially account-bound `identity`
  field. Its exact bytes remain in the private local path in the runner,
  restricted to mode 600, and are verified by SHA-256. Do not copy that cache
  into the public repository. Authentication is mounted read-only into each
  isolated Codex cell and is not copied into the published results.
- Per-cell bubblewrap mounts block sibling outputs, hidden tests, host task
  sessions, resident host VRS state, host runtime sockets, and the private
  frozen plain archive.
  Grading rejects commands that inspect task
  history, hidden tests, or raw VRS storage. The model receives only the
  `checkpoint` retrieval cue, then must locate the original experience through
  the MCP and obtain a four-stage receipt; plain cells have no VRS.

## Verified now

The static inputs for `python tools/run_vrs22_compaction_stress_v019.py
--output-dir /var/tmp/vrs22-v019-reserved --preflight-only` passed, while its
current status is `PENDING_LIVE_HANDOFF` because the actual task has not ended.
The one-shot SessionEnd watcher is
`swegca-vrs22-sessionend-handoff-7e35fb8-20260922.service`, pointing to the
new installed candidate; it does not merge while this task is active.
The same gate rejects a full run before creating an output directory or making
model calls. The current software self-test receipt is
`/var/tmp/vrs22-whole-path-selftest-natural-read-20260922/receipt.json`, SHA-256
`dc78a3600248b1eca28243cc3445d2f001c358c2066e854d6a9252509ab7a191`.
It covers two simultaneous sessions (main 0 before SessionEnd; all six session
records linked after), installed hook injection/cursor/SessionEnd, a generation
lease across a 6.5-second idle boundary with 81 records and four-stage
session-first recall, exact-address four-stage Replay of an original experience
after main attachment, process/module/package checks, and mount isolation. It
found zero database artifacts. It also checks every original session envelope
against its host-visible ingress record after SessionEnd, including two
simultaneous sessions. Grader self-checks: 17 passed. The original
experience Replay in this receipt belongs to the isolated self-test; the
resident main must separately pass the same probe after handoff.

Current native package suite: 127 passed. An earlier exact-address run under
4 GiB and 625 MB/s per NVMe read/write limits made 50,000 four-stage Replay
calls with a maximum of 0.157172 ms
for 15,630 existing experience records. This exact-address diagnostic does
not measure the corrected user-input → first-Recall route. Maximum configured route depth had 50,000 synthetic exact
lookups with a 0.075921 ms maximum. These measured bounds do **not** establish
an all-size hard real-time guarantee or performance for one billion VRS
parameters; the repository does not yet define a mapping from parameters to
native experience units. Do not relabel record count as parameter count.
The current natural-query path was separately measured on the 15,630-record
copy; a 100-match query took a warm median 15.152 ms through Replay.
That diagnostic does not grade the corrected 1 ms transition. See
`docs/VRS2_2_NATURAL_REPLAY_BOTTLENECK_20260922.md`.
The runner records Replay time only as a diagnostic; live handoff and the
actual transition measurement remain separate readiness checks. Once SessionEnd is complete, preflight will report
`PENDING_PRODUCT_PERFORMANCE` until the full performance requirements are
actually repaired and verified. A finite 15,630-record benchmark alone cannot
certify the all-size bound or the one-billion-parameter goal.

## After the SessionEnd handoff and product performance repair

Run the preflight again, inspect the actual new resident state and require
`READY` after performance repair. Then run:

```sh
python tools/run_vrs22_compaction_stress_v019.py \
  --output-dir /var/tmp/vrs22-auto-compaction-confirmation-019
python tools/summarize_vrs22_compaction_stress_v019.py \
  --runs /var/tmp/vrs22-auto-compaction-confirmation-019 \
  --output-dir /var/tmp/vrs22-auto-compaction-graded-019
```

Require all three cells to have 10+ actual compactions and complete live
experience receipts before accepting any measurement. Automated text and code
scores remain provisional until a reviewer checks every boundary answer,
recovery/final message, complete code diff, and executable test log. Publish the raw/graded artifact
hashes, per-boundary purpose and dialogue consistency, coding regression and
quality scores, detailed token counts including compaction responses, and
limits or failed cells. Do not fill a missing run with the older v018 results.

## Later candidate status on 2026-09-22

The frozen readiness steps above originally referred to product commit
`7e35fb8`. The current evaluation runner, grader and one-shot post-SessionEnd
handoff watcher now use source commit `3e65550` and wheel SHA-256
`4b06afd976f3b983f37ba905fdb4b99eede691940733afa142952b0749992fe7`.
All 48 installed files match that source and wheel; native tests passed
127/127, grader regressions 20/20, and the installed-wheel whole-path
self-test passed. The current performance receipt records 10/10 natural
100-match and natural 1,008-match Replay times separately from the
user-input → first-Recall gate. The live performance gate was not established. The live SessionEnd handoff has not
occurred, and no new VRS model cell has been run.
