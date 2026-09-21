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

## Installed source-equivalent wheel and live handoff readiness

Commit `d6e89bb` was built as an offline wheel with SHA-256
`39ba1ed06642947eeb5e9429b7da33e5d6ed4444ae269aab5cd33c4c24333e90`.
The separate installed runtime has all 48 package files byte-equal to source
and wheel, no extra product files, no SQLite/filelock/Hermes product references
or dependencies, and no SQLite module loaded during its process check.

This installed wheel captured another 258 active-session lines into the same
isolated native temporary VRS. A second full-prefix audit reported **32,468
of 32,468** original observations equal, zero missing, unexpected or changed
originals. Its receipt is
`evals/vrs22_context/results/live_session_native_shadow_installed_wheel_20260922.json`
(SHA-256 `245442bc1b8595e4439464a28e6a131f33cda127a242442cd0aa37323dba0540`).
The installed-wheel whole-path self-test passed, including temporary-first
reads, main fallback, exact Replay, SessionEnd-only linkage and hook routing;
its receipt is `/var/tmp/vrs22-whole-path-selftest-native-ingress-20260922/receipt.json`
(SHA-256 `754ef08cfcf34cd5bb377075a4f811513c3398da14d23aae37b9fe31e9367865`).
The evaluation grader passed **20/20**.

The one-shot live watcher now runs from this installed wheel and remains
active until the real SessionEnd marker. The previous watcher is inactive.
At this report, the live preflight is `PENDING_LIVE_HANDOFF`: no SessionEnd
receipt exists, the native live main has not replaced the legacy state, and
the current hook still points to the old runtime. This is an explicit open
gate, not a successful live switch.

The installed wheel's cgroup Replay receipt is
`evals/vrs22_context/results/first_ranked_original_replay_native_ingress_wheel_20260922.json`
(SHA-256 `6edb80b56341b994f68ea8ce5151b34b990cb0981b6d5f1692dc10935f3a1822`).
First ranked original Replay took 0.680 ms with one match, 19.127 ms with
100 matches, and 117.526 ms with 1,008 matches on first calls. The 1 ms
goal remains unmet; the model evaluation gate remains closed.

## Exact MCP Replay after a graceful temporary-VRS shutdown

A direct MCP read of a recent original in the isolated temporary VRS initially
failed with `read_projection_pair_mismatch`. The original and the current VRS
generation existed, but shutdown closed the last hot shard with a checkpoint
without publishing its matching read projection. `Resident.close` now uses the
same complete-projection publication as hot-shard eviction before releasing
the shard owner. A regression closes and reopens a real split main and checks
its exact address through Déjà vu, Recall, Replay and Re-evidence.

The older shadow projection was rebuilt from its complete native shard, after
which the installed-wheel MCP exact-address query returned one matching
original with a valid four-stage receipt, `grants_authority=false`, and
`internal_llm_calls=0`. This was a local isolated MCP path, not the still-old
live Codex MCP connection. The revised source standalone suite passed
**131/131 in 182.61 s**. The installed runtime still needs a rebuild from
this later source commit before this shutdown fix is a live candidate.

## Installed projection-close candidate

Commit `0cd027daab26317b9eef21c97fdeed0bb9871cfe` was built offline into
an isolated wheel (SHA-256 `c32c8a27fd973ab04d5cf2111ef7e84c5199d448f370df6477539c61f21e525b`).
All 48 product files matched source, wheel and installed runtime; the audit
found no SQLite, filelock or Hermes product dependency or source reference.
The installed runtime is still separate from the active Codex MCP connection.

The new wheel's first-ranked original Replay receipt is
`evals/vrs22_context/results/first_ranked_original_replay_projection_close_wheel_corrected_20260922.json`
(SHA-256 `650d18af3e1f317877d7da686736dcc09c97a0f164a80faf1a2ba52f53354700`).
On the same copied 15,630-experience native state, under 4 GiB memory, zero
swap and 625 MB/s SSD cgroup limits on the state device, first calls took
0.695 ms with one match, 19.159 ms with 100 matches, and 118.131 ms with
1,008 matches. The all-size
1 ms gate remains false; model evaluation remains disabled.

The new installed wheel's whole-path selftest passed. Its private receipt is
`/var/tmp/vrs22-whole-path-selftest-projection-close-20260922/receipt.json`
(SHA-256 `c3815b86baee507a9995e853951cf8377edca45b11ef9a6f1c183baee154fdf4`).
An isolated local MCP exact-address query against the repaired shadow also
returned its original with all four stages, no authority and zero internal
LLM calls. Live Codex MCP and the active legacy state have not yet switched.
The one-shot post-SessionEnd handoff watcher now runs this new installed wheel
as `swegca-vrs22-sessionend-handoff-0cd027d-20260922.service`; the older
`d6e89bb` watcher is inactive. No SessionEnd marker exists, so no live main
merge, hook switch or MCP switch has occurred.

## Active native shadow continuation and remaining read cost

An isolated native shadow tailer now follows this active session under a 4 GiB,
zero-swap cgroup as
`swegca-vrs22-native-shadow-tail-0cd027d-r1-20260922.service`. It does not
change the installed Codex hook, app MCP or live main. A stopped-writer
integrity audit of its captured prefix passed: 38,184 transcript lines,
32,624 captured lines, 5,560 explicitly excluded private/control lines,
33,244 expected and actual original observations across five native stores,
zero missing, unexpected or changed originals and no SQLite module loaded.
The receipt is `/var/tmp/vrs22-native-shadow-paused-audit-20260922.json`
(SHA-256 `ade78ee261ca8d40caa38dc570b535a187ec73b9214da7052b59f1e6a83298c2`).
The shadow tailer was restarted after the audit. A concurrent-writer audit
can see journal rows newer than its frozen cursor; its first attempt reported
seven such rows, while the stopped-writer audit showed exact equality.

An instrumented broad natural Recall with 1,008 matching originals executed
about 1.45 million Python calls in 0.304 s under profiling. Before the first
Replay, it opens all candidate exact capsules and current VRS projections to
derive complete ranking and region/portal navigation. The uninstrumented
first-ranked Replay is 118.131 ms on that query. This identifies work to
remove from the critical path without deleting the complete VRS stages or
changing the ranking contract; it is not a latency repair yet. The app MCP
`memory_status` still returned `tool_request_failed` on the old live process.

## Long-lived MCP connection to a restarted session resident

The installed Codex MCP process caches a loopback client for each session.
In an isolated reproduction, a session daemon restarted on a new port while
the stdio MCP process stayed alive. A later `memory_status` failed with
`resident_request_failed`; a new direct `LayeredMCP` instance reached the same
session state. `_Remote.call` now reattaches to the state directory and retries
only the idempotent status request when that exact transport error occurs.
In-flight context, page and Replay requests still fail closed because their
view ownership cannot safely be retried after a daemon restart.

The 23 session-VRS source tests passed in 176.99 s. A real stopped-and-restarted
daemon changed port and the revised source and new installed wheel both
reattached successfully. This is a reproduced product defect and repair; it
does not establish that the old live app MCP `tool_request_failed` has this
same cause or that the app connection is already repaired.

Commit `c0de7f1686fd8bad476282d901b3c81accc5e54f` was built as an isolated
wheel with SHA-256 `b94975c8f003d846c352f1d71e2810c22d3fda1091955370f2a75d0daafadd82`.
All 48 product files matched source, wheel and installed runtime; the whole
installed-path selftest passed with receipt
`/var/tmp/vrs22-whole-path-selftest-mcp-reconnect-20260922/receipt.json`
(SHA-256 `7f39fcf27d910ed28b4b398ec7692a4ab1a9002b844a5c77921a0eeba1d1fd25`).
The new installed wheel's natural first-ranked Replay receipt is
`evals/vrs22_context/results/first_ranked_original_replay_mcp_reconnect_wheel_20260922.json`
(SHA-256 `b38453aba238405ecca27e8946fa90449fd5ae4a51e7a28f4255bc5b170f1bd7`).
Under the same 4 GiB, no-swap and 625 MB/s state-device limits, first calls
took 0.775 ms for one match, 19.425 ms for 100 and 120.410 ms for 1,008.
The latency gate remains false. The one-shot post-SessionEnd watcher now uses
this new wheel as `swegca-vrs22-sessionend-handoff-c0de7f1-20260922.service`.
No SessionEnd marker exists and the active Codex hook/MCP has not switched.
