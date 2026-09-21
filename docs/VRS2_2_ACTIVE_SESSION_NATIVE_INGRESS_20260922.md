# VRS 2.2 active-session native ingress repair, 2026-09-22

The active Codex session is still on the pre-repair installed runtime. Its
configured hook points to the old worktree, and its temporary VRS directory
contains SQLite. The SessionEnd-only live handoff watcher is active, but no
SessionEnd marker or native live main has appeared. This report tests a separate
native shadow of the same host-visible transcript; it does not claim that the
live hook has switched.

## Actual ingestion failures and source repairs

The isolated shadow used the current 150+ MB transcript as ingress, with a
4,294,967,296-byte memory cap, zero swap, and 625,000,000 B/s SSD read and
write caps. Neither the transcript nor the resident live state was modified.

1. The first installed-wheel attempt failed after 561 transcript lines with
   `internal: OSError`. Direct execution of the same next batch identified
   `EMFILE` at `ExactReplayStore._open_segment`. The user service had a 1,024
   soft descriptor limit. The source now sizes its read-segment cache and
   exact-address/source write batches from that limit. It prevalidates
   cross-chunk address and source lineage before publishing any chunk.
2. After 18,618 lines, a new source routed through a cold shard whose journal
   was ahead of its checkpoint. The source now treats a complete, generation-
   matched exact/source directory as authoritative for a miss. An incomplete
   shard is opened through its full native journal owner, preserving the
   original VRS state instead of using an out-of-date checkpoint view.
3. After a further restart, a newly created auto shard had a native journal
   without a current checkpoint. Startup now checkpoints the recovered full
   generation and confirms its pair ID before accepting ingress. The exact
   address/source routing directory is completed before a new source is
   admitted. A cold shard discovered during backfill has its true count bound
   to the cursor, so completion checks do not rely on a missing cached count.

The completed source run captured a prefix of **36,985** transcript
lines. Of those, 31,590 host-visible lines entered native VRS experience and
5,395 private or control lines were explicitly excluded by the existing
transcript extractor. The 31,590 lines generated **32,210** original
observation envelopes because long visible content is split into bounded
parts. The native shadow spans four native VRS stores. The independent
`tools/audit_vrs22_session_native_capture.py` recomputed each expected
request ID and normalized original body from the captured transcript prefix,
then checked all native journals. It found 32,210 of 32,210 exact matches,
zero missing, unexpected or changed originals, and zero SQLite module loads.
Only counts are in the committed receipt; dialogue and source addresses are
not printed.

Receipt: `evals/vrs22_context/results/live_session_native_shadow_integrity_20260922.json`
(SHA-256 `5f66b0151b4c9f61cebb10a2b73cf495fb410ce574f909083594aa25030d4b3d`).
The full standalone source regression suite passed **130/130 in 182.36 s**
after the fixes.
This proves content integrity for one isolated, currently active session
prefix. It does not prove live hook installation, current main migration,
temporary-first/main-second live reads, the <1 ms Replay target, or the
user-defined one-billion VRS-parameter seconds target.
Those remain separate gates before VRS model evaluation.
