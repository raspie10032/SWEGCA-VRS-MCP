# Agent memory integration validation — 2026-09-07

## Result and acceptance boundary

Implemented and verified the Hermes native MemoryProvider adapter plus a
single-owner local service. Automatic recall/capture does not require the LLM
to select a tool. This is host integration validation, NOT a live generated
conversation, a cognition-growth experiment, or OpenClaw compatibility.

- Full regression: **195 passed in 8.27 s** (181 existing + 14 integration tests).
- Upstream integrity: **34 files PASS**, no numerical kernel changes.
- Real host lifecycle: **3 fresh Hermes loader/MemoryManager processes**, **2
  service processes**, **1 synthetic stored turn**; cross-session and service
  restart recall passed, same owner/hot snapshot retained, World unchanged.
- No live LLM request, no voluntary model tool call, no new training data claim.
- Repeated native integration run: **1.759 s**, passed. The first run (1.708 s)
  is retained separately; later socket/reliability fixes were retested.
- Wheel and source distribution build passed. Installed service executable and
  actual prepared-profile discovery/recall/status were also checked.

## Tested cases

1. Sixteen concurrent duplicate deliveries yield one admitted episode and
   identical receipts; a second core owner is rejected.
2. Automatic dirty-only VRS and all four hot activation stages; unsigned
   conversational text remains insufficient to authorize World writes.
3. Success/failure/negative/uncertain/conflict/pending are all active inputs;
   pagination does not hide any related episode from addressability.
4. Service restart preserves owner, hot snapshot and stored text.
5. Native provider automatic capture, duplicate callback and session switch.
6. Delayed background sync retains the original session/turn provenance.
7. Offline outbox survives provider restart and is delivered after recovery.
8. Private socket permissions, owned socket refusal and malformed requests.
9. Non-primary sync cannot write; gateway initialization is rejected.
10. Stored prompt delimiters are quoted, not promoted to instructions.
11. Dead same-owner socket recovery, without deleting regular files/live sockets.
12. Lost commit acknowledgement retries the exact envelope without duplication.
13. Hot context activation does not invoke core canonical JSON/digest helpers.
14. Profile installer refuses an existing directory and preserves its bytes.

## Reproducibility

Run the commands in [HERMES_MEMORY.md](HERMES_MEMORY.md). Native test artifacts
are deliberately ignored under `local-data/hermes-integration-20260907-a/` and
`local-data/hermes-integration-20260907-b/`; each contains three host stdout
JSON reports, separate stderr and a summary. Fresh temporary stores are used
and removed after the run; the reports preserve the synthetic observations.
No existing user/Rozephine experience store is read, cleared or re-experienced.

Hermes was read from a pre-existing dirty 0.19.0 checkout with HEAD
`eb52760564dbba2e5971fa54bd67384e281cd3b8`. HEAD alone does not identify its
working bytes. The exact exercised source hashes are:

| Host source | SHA-256 |
|---|---|
| `agent/memory_provider.py` | `96dab095cbb81ed361a0a08bf501840bfd51121fe760b7296595be39d40a411f` |
| `agent/memory_manager.py` | `c6b8841babd901a900234d77eab095e53f99010c095f919d7f5fb745a17179d2` |
| `plugins/memory/__init__.py` | `1b155797bfaea72b5ea5d2e35fde3452534cda4600fd49293130015096cb25a8` |
| `agent/agent_init.py` | `6a5d55a7a1474d665aea35afdfb79ce7bebcaa99cba73e62bc9acee2996df677` |
| `agent/turn_context.py` | `a478aa94f57d43d57a4427a17b47c773f742c6408cf239ba582eb75536d52ed0` |
| `run_agent.py` | `466de8e77ba0e9be3f0933d31a4450ef9bb553d6071501e3827e4132e4d28cc3` |

The first three are imported/exercised in the native test. The final three are
read-only inspected lifecycle wiring, not execution evidence for a whole AIAgent.
No files in that checkout were edited.

| Implementation/artifact | SHA-256 |
|---|---|
| `agent_service.py` | `1659ba7aa91949efa3398aa8d22c1595fbbda0062cee33d087cb084f1e336fed` |
| `hermes_memory.py` | `f9fa2d3e396720e29ad9c242d2d1b2c140d9b901f2139b931d837aaf5e30463b` |
| `stateful.py` | `cbc94814d0676bee517ed4826a4b60e1bd647f98d364370b6ac33e98cd219bc8` |
| native run B summary | `5e90ca9a6a986cd23ecfa31f32c6b151646cacd6145f8d4013e25ea5dee55ec1` |

## Local deployment (snapshot, not portability guarantee)

A new private `hermes-swegca` profile was prepared, with both builtin memory
channels disabled. A user-level `swegca-vrs-hermes.service` was enabled and
started: active/running, zero restarts, approximately **153 MiB** resident cgroup
memory at inspection. The deployed core has **zero episodes**; synthetic test
data was not mixed into it. Its interval is 30 seconds, and clean ticks do not
run VRS. It listens only on a private user runtime Unix socket.

The prepared profile was loaded with the real Hermes loader/manager and queried
read-only against this deployed service. No production model or authentication
was configured, no old profile was changed, and no live conversational agent
was left running. Hermes was not available as a shell command or checkout venv
at inspection. Install the package into the intended Hermes runtime and finish
that profile's model/login setup before claiming live deployment acceptance.

## Limits and unresolved work

- Hermes best-effort callbacks are not a host-level fail-closed reasoning gate.
- User/assistant text only; no interrupted/tool-payload/multimodal capture claim.
- Outbox durability starts when the provider callback returns, not when the
  host merely schedules it. Raw host transcript remains a separate subsystem.
- Lexical retrieval and existing event-to-hypothesis graph, not semantic
  cross-turn learning. A pending chat turn is not a verified real-world outcome.
- Current VRS is coalesced global arithmetic, not incrementally equivalent
  convergence. Large graphs can block core access and exceed adapter timeouts.
- Explicit page and wire budgets; no claim of unbounded single-response size.
- No OpenClaw adapter, multi-user gateway ACL, or nine-tool MCP proxy into this
  resident owner yet. Original independent MCP modes remain available.
- Future acceptance: install/configure the intended host model, verify a real
  conversation across session restart, then implement/test other host adapters.

## Codex 작업 실수 및 교정

- Discovery: unbounded dirty-worktree output truncated a combined read. No
  files changed. Re-read relevant files with bounded output; fully re-read
  AGENTS before development. See the chronological ledger.
- Dependency check: the initial real-loader import lacked PyYAML. The check
  failed rather than being called a success; added PyYAML to the test extra and
  verified with a refreshed lock/install. Production adapter remains stdlib-only.
- Draft identity handling used a mutable latest turn ID, unsafe for a delayed
  Hermes background sync after the next turn/session started. Code inspection
  exposed the race; replaced it with queued session/message-bound turn IDs and
  stable retry timestamps, then added the delayed-sync and duplicate tests.
- Draft socket cleanup lacked separate bind ownership, so a startup race could
  remove another listener's pathname. Corrected before deploying: separate
  socket lease, same-owner dead-socket check, inode-bound cleanup and regressions.
- Scope, assumptions, interventions, authority, evidence boundaries and completion
  claims were audited. Above errors belong to Codex, not Hermes or Rozephine.
  Retained run A/B logs are not rewritten; no failed draft was deployed publicly.

## Rozephine의 판단

Not invoked. Existing private Rozephine runtime/experience remain untouched.

## Codex의 판단

The public component now has a tested always-invoked Hermes memory integration
and a running local memory service. This does not establish live LLM behavior,
OpenClaw compatibility, broader cognitive growth or independent semantic truth.
