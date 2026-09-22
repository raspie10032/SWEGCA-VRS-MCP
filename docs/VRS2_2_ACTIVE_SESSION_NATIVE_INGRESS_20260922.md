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
this new wheel as `swegca-vrs22-sessionend-handoff-c0de7f1-r1-20260922.service`.
After the actual SessionEnd marker, it also stops only the isolated native
shadow tailer so that copy does not keep polling after the live handoff.
No SessionEnd marker exists and the active Codex hook/MCP has not switched.

The installed wheel's stdio MCP handshake was also tested in an isolated
native state. `initialize` negotiated protocol `2025-06-18`; `memory_status`
with an explicit session ID returned `ready`, while the same request without
that ID returned `tool_request_failed` by design. The current app-exposed
`memory_status` declaration lists an empty input object, and its
`memory_context` declaration lacks the session ID field present in the
current product tool definition. This is evidence of stale app tool discovery
or a missing hook injection on this connection, but it does not identify which
one; the app call still fails even when this agent supplies an ID. A fresh
app MCP connection after the authorized SessionEnd handoff must be checked
before claiming live integration.

## Concurrent active-session integrity audit

The native-capture auditor now authenticates journal rows newer than its
frozen cursor against complete transcript lines after that cursor. It still
fails if any cursor-covered original is missing or changed, or if an extra
native original has no matching transcript line. A targeted regression passed
both the valid in-flight writer case and an unrelated injected record case.
The active native shadow tailer stayed running during the audit, which passed:
39,093 cursor-covered transcript lines, 34,023 expected original observations,
34,025 native originals, two authenticated post-cursor originals, zero
missing, unexpected or changed records, and zero post-cursor pending originals.
The private counts-only receipt is
`/var/tmp/vrs22-native-shadow-live-tail-audit-r1-20260922.json`
(SHA-256 `7d372c3198888efe8146874bba6c398068c6becd276d86bc2142a2a9a5e502da`).

## Durable one-shot SessionEnd handoff owner

The prior handoff process was a transient `/run/user` unit and would disappear
on a user-manager restart. Its replacement is the enabled user unit
`swegca-vrs22-sessionend-handoff.service`, backed by the persistent unit file
`/home/raspie/.config/systemd/user/swegca-vrs22-sessionend-handoff.service`
and a task-owned deployment wrapper under the installed runtime's `deploy/`
directory. `systemd-analyze --user verify` and Python syntax checks passed.
The unit is active with a live wrapper and child handoff process, a 4 GiB
memory limit, zero swap and an enforced 625 MB/s read/write cgroup limit on
the state device (`io.max` reports `259:3 rbps=625000000 wbps=625000000`).
`ConditionPathExists` binds startup to the armed
handoff marker. The child still requires the durable SessionEnd marker before
it can touch live main; there is currently no such marker, the old MCP config
is unchanged and no native live main exists. After a successful handoff and
receipt, the child removes its deployment script and marker, and the wrapper
disables and removes its own unit and script. An actual reboot or SessionEnd
has not yet occurred, so those later transitions remain unverified.

## App-server connection boundary

The documented `config/mcpServer/reload` method was reached over the local
Codex CLI app-server control socket and returned successfully. Its status
listed `swegca-vrs` with the current product's optional `session_id` input
field. However, `mcpServer/tool/call` on this active desktop task ID returned
`thread not found`: that socket belongs to a separate CLI app-server, not the
desktop app-server that owns this task. The desktop process runs its own
app-server child over private parent pipes. Thus the reload did not prove a
refresh of the desktop tool binding, and the current app MCP call still
returns `tool_request_failed`. The isolated stdio MCP and new wheel remain
verified independently; desktop integration still needs a fresh connection
after the post-SessionEnd config switch.

## Portal metadata read candidate

The source candidate now reads each portal key's original revision and outcome
from the same generation's compact index columns. It keeps the original
experience address, portal weights, strength, shared count and complete
projection. It no longer decodes an unrelated original blob merely to form a
portal key. `LazyCues` also validates decoded cue strings with a C-level
`map`/`all` loop while retaining the same fail-closed error. The standalone
source suite passed 135/135 tests, including a portal regression that rejects
an original-blob decode and verifies the complete key.

On the copied 15,630-original native state under 4 GiB, zero swap and a
625 MB/s SSD cgroup cap, source first-ranked original Replay took 0.750 ms
for one match, 15.432 ms for 100 matches and 98.692 ms for 1,008 matches.
The measurement is
`evals/vrs22_context/results/first_ranked_original_replay_portal_index_source_20260922.json`.
The 1 ms all-size gate still fails. At the time of this source measurement,
the changed files were not yet in the installed wheel or active Codex MCP,
and no VRS model evaluation was started.

## Natural Recall latency dissection

On that same copied native state, a source `cProfile` run with 1,008 natural
matches took 215 ms for the full four-stage call under instrumentation.
Its cumulative hot paths were 1,008 exact capsule reads (43 ms), 1,008
current VRS projections (42 ms), local region navigation (52 ms) and 3,024
JSON decodes (59 ms). Cumulative figures overlap and are not additive. The
normal uninstrumented first-ranked Replay measurement remains 98.692 ms.

A separate diagnostic read the first posting for that same broad cue and then
called the existing exact-address four-stage path. It did **not** perform
natural Recall ranking and is not an acceptance measurement: first call
1.728 ms, warm median 0.978 ms, warm p99 1.424 ms, and 47 of 101 calls at or
above 1 ms under the same 4 GiB, zero-swap, 625 MB/s cgroup limits. Simply
choosing an arbitrary first posting would therefore still fail the strict
latency condition in this copy. The meaning of the first original relative
to final Recall rank is pending explicit clarification; no ranking semantics
were changed.

## Storage-scan admission and installed candidate

The 500 GB allocated-storage scan now rejects a directory walk or file stat
error instead of counting an unknown allocation as zero. Two targeted source
resource tests passed. This preserves the existing 500 GB ceiling, automatic
shards and original episodes; it does not change Recall ranking.

Product source commit `24b723a` was built into a separate offline wheel with
SHA-256 `931e6b8aa25af3e26005b33415d6d53054219d82655eafb36e3dc219809f3ee0`.
All 48 source, wheel and installed Python files matched byte for byte, with
no extra installed product files or SQLite, Hermes or external filelock
imports/dependencies. The installed wheel's natural first-ranked Replay
receipt is
`evals/vrs22_context/results/first_ranked_original_replay_storage_audit_wheel_20260922.json`:
0.715 ms for one match, 15.736 ms for 100 and 95.699 ms for 1,008 under
4 GiB, zero swap and 625 MB/s I/O limits. The all-size 1 ms gate remains
false.

The installed-wheel whole-path selftest passed at
`/var/tmp/vrs22-whole-path-selftest-storage-audit-r2-20260922/receipt.json`
(SHA-256 `fbf5a2ff734bd4ba1a6059e4a5780e223c359ac8aedb0232cfae242744d7d231`),
and 20 grader regressions passed. The first selftest attempt inherited a
relative `PYTHONPATH=src` into its isolated fixture workspace, shadowing the
installed package; the clean-environment rerun passed. The evaluation runner
now pins the new wheel and its failing performance receipt, so its preflight
still does not allow a VRS model call.

The persistent `swegca-vrs22-sessionend-handoff.service` now runs this new
installed wheel. Its supervisor and child are live under the 4 GiB, zero-swap,
625 MB/s cgroup. The armed SessionEnd marker is still absent; the active
Codex state, hooks and MCP were not switched. The separate native shadow
tailer remains active. A concurrent live audit of its current prefix passed:
40,302 transcript lines, 35,064 expected original observations, two valid
post-cursor originals, zero missing, unexpected or changed originals, and no
SQLite module loaded (`/var/tmp/vrs22-native-shadow-live-audit-20260922-r2.json`).

## Readable reasoning-summary ingress

The Codex transcript also carries readable `summary_text` parts inside
`response_item: reasoning`. The earlier extractor dropped the whole item,
including those parts. [OpenAI's App Server documentation](https://learn.chatgpt.com/docs/app-server)
distinguishes streamed readable summaries from raw reasoning text. The
extractor now admits only the ordered `summary_text` parts as an assistant
`reasoning_summary` original experience. It excludes encrypted content and
raw reasoning. The session VRS test checks both the admitted original and
the absence of private fields; 45 session and grader tests passed.

A one-time task-owned backfill added 1,299 previously skipped readable
summary originals to the active native shadow's session VRS, with their
stable transcript addresses. The script was removed after use. The new
tailer continues from the same cursor using the new installed wheel. A
subsequent concurrent original-content audit passed with 40,942 prefix
lines, 36,914 expected originals, two authenticated post-cursor originals,
zero missing/unexpected/changed records, and no SQLite module loaded:
`/var/tmp/vrs22-native-shadow-summary-live-audit-20260922.json`. The active
desktop VRS remains the old connection; this is the isolated native shadow.

Product source commit `d5dea59` was built into an offline wheel with SHA-256
`c9ef791cd21ea71abfb280a51b4dcbedda55dfc76e1cb95ab66f3da96787c80e`.
All 48 source, wheel and installed Python files were byte-equal, with no
SQLite, Hermes or external filelock imports/dependencies. Its complete
installed-wheel selftest passed at
`/var/tmp/vrs22-whole-path-selftest-summary-ingress-20260922/receipt.json`
(SHA-256 `191f520a6419abf8242f19dd39f1c010d1622ee2b59e5338971dc04ed1bfcd9a`).
The persistent post-SessionEnd handoff unit and shadow tailer now use this
wheel; both are active under 4 GiB, zero swap and 625 MB/s I/O limits. No
SessionEnd marker exists, so main has not merged and the live desktop MCP
has not switched.

The wheel's 15,630-original natural Replay receipt is
`evals/vrs22_context/results/first_ranked_original_replay_summary_ingress_wheel_20260922.json`.
During concurrent system load, the first calls measured 3.037 ms for one
match, 26.778 ms for 100 and 167.931 ms for 1,008. These samples fail the
strict 1 ms gate. The evaluation grader now accepts additional observed
violations instead of assuming only the broad two cases fail; all 20 grader
regressions pass and no VRS model cell ran.
