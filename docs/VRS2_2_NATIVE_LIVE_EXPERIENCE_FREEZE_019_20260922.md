# VRS 2.2 native live-experience evaluation: v019 readiness

This is a **preflight and self-test receipt**, not a scored v019 model run. Do
not start the three VRS cells until the current Codex task has ended and its
SessionEnd handoff has preserved and moved the resident experience to native
VRS. Verify the resident package, hooks, live ingestion, session-first/main
fallback, and absence of database artifacts in the new task before starting.
Merging the main experience during the active task would violate the frozen
session lifecycle.

## Frozen inputs and isolation

- `tools/run_vrs22_compaction_stress_v019.py` exercises **only** 100k+VRS
  cells for Luna, Terra, and Sol, with medium reasoning, 12 planned real Codex
  compacted events and at least 10 required in each cell. The six frozen v005
  plain cells remain the controls (100k and 250k).
- Clean coding fixture tree SHA-256:
  `6b35f73d7241620a43e30811b86811a8a1ec5a92e7ffa4e5f295054817cb7b0c`.
  The fixture, its visible regression, its hidden tests, its clean Python
  environment, and the grader use no SQLite, Hermes, or external filelock.
  Plain output was separately revalidated against this clean fixture:
  `evals/vrs22_context/results/plain_clean_validation_005.json`, SHA-256
  `9d1e39bbc7ba7d10bfe3970f17bf5ddc28660e3f2cc21f9477dc785bed1c7b36`.
- Runtime source provenance: `65ec52d`; frozen installed wheel SHA-256:
  `e0144305a8fa863f52679b7a4aa1e01901fa23942314e364d36c40c302a656c9`.
  Preflight verified all 48 installed package files against that wheel and
  found zero forbidden source references or dependencies. A subprocess import
  audit found neither `sqlite3` loaded nor `filelock` available in either the
  runtime or the isolated grading environment.
- The frozen Codex model-list cache has a potentially account-bound `identity`
  field. Its exact bytes remain in the private local path in the runner,
  restricted to mode 600, and are verified by SHA-256. Do not copy that cache
  into the public repository. Authentication is mounted read-only into each
  isolated Codex cell and is not copied into the published results.
- Per-cell bubblewrap mounts block sibling outputs, hidden tests, host task
  sessions, and host VRS state. Grading rejects commands that inspect task
  history, hidden tests, or raw VRS storage. The model must use the MCP
  experience address and a four-stage receipt; plain cells have no VRS.

## Verified now

`python tools/run_vrs22_compaction_stress_v019.py --output-dir
/var/tmp/vrs22-v019-reserved --preflight-only` passed. Its self-test receipt is
`evals/vrs22_context/results/v019_selftest_receipt.json`, SHA-256
`9f30aff1ed120325df077fceb775a0a31ab105518a657dd04e5eac8cb4de6d34`.
It covers two simultaneous sessions (main 0 before SessionEnd; all six session
records linked after), installed hook injection/cursor/SessionEnd, a generation
lease across a 6.5-second idle boundary with 81 records and four-stage
session-first recall, process/module/package checks, and mount isolation. It
found zero database artifacts. Grader self-checks: 8 passed.

Full native package suite: 125 passed. Under exact 4 GiB and 625 MB/s per NVMe
read/write limits, 50,000 four-stage Replay calls took at most 0.157172 ms
for 15,630 existing experience records; exact-address lookup had no call at
or above 1 ms. Maximum configured route depth had 50,000 synthetic exact
lookups with a 0.075921 ms maximum. These measured bounds do **not** establish
an all-size hard real-time guarantee or performance for one billion VRS
parameters; the repository does not yet define a mapping from parameters to
native experience units. Do not relabel record count as parameter count.

## After the current SessionEnd handoff

Run the preflight again and inspect the actual new resident state. Then run:

```sh
python tools/run_vrs22_compaction_stress_v019.py \
  --output-dir /var/tmp/vrs22-auto-compaction-confirmation-019
python tools/summarize_vrs22_compaction_stress_v019.py \
  --runs /var/tmp/vrs22-auto-compaction-confirmation-019 \
  --plain-runs /var/tmp/vrs22-auto-compaction-confirmation-005 \
  --output-dir /var/tmp/vrs22-auto-compaction-graded-019
```

Require all three cells to have 10+ actual compactions and complete live
experience receipts before accepting any score. Publish the raw/graded artifact
hashes, per-boundary purpose and dialogue consistency, coding regression and
quality scores, detailed token counts including compaction responses, and
limits or failed cells. Do not fill a missing run with the older v018 results.
