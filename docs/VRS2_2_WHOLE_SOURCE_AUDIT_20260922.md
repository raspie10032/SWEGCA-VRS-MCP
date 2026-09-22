# VRS 2.2 whole-source SWEGCA audit — 2026-09-22

This is a source and deployment audit, not an acceptance certificate. The final
implementation language is C++. The Python tree is the current behavioral
reference. The four memory stages, exact originals, VRS numerical state,
regions, memberships, portals, uncertainty, refutation, and session ownership
must survive the port. No prebuilt wheel may be used as a product or dependency
shortcut.

## Scope and evidence

The static sweep covered **all 47 current product Python files**, every direct
third-party import in those files, all five declared product entry points, the
active installation instructions, Windows installer, source verifier, and the
evaluation runner/grader. The test tree has 21 standalone test files; the
session VRS suite passed 24/24 after the cleanup in this audit. Static coverage
does not prove behavior of every branch. Historical reports remain history.

| Product files | SWEGCA/VRS role and current evidence |
|---|---|
| `__init__.py`, `native_transport.py`, `server.py`, `native_memory.py`, `native_context.py` | Product interface and evidence paging. Stdio MCP uses first-party transport; the external MCP SDK is imported by tests/smoke tools only. |
| `codex_hooks.py`, `conversation_hooks.py`, `conversation_watch.py`, `conversation_finalize.py`, `conversation_merge.py`, `session_capture.py`, `layered.py`, `read_lease.py` | Session ingress, exact routing, session-first read, and SessionEnd-only attachment. The path from transcript to `VRSClient.ingest_many` to resident/Main was traced; 24 session tests passed. |
| `loopback.py`, `linked_shards.py`, `resident.py`, `sharded.py` | Resident transport, complete shard links, logical main and four-stage recall. Source tests cover session and cross-shard cases; the active desktop main has not yet passed the real handoff. |
| `store.py`, `compact_index.py`, `native_journal.py`, `native_lock.py` | Main-owned immutable experience, native journal, VRS generation and synchronization. `store.Main.ingest_many` journals one pair generation and calls graph append. `numpy` and `immutables` implement data structures; their source-build provenance remains unverified. |
| `cue_shards.py`, `exact_replay.py`, `read_projection.py`, `projected_recall.py`, `csr_cache.py` | Cue-to-address directory, original capsule, current VRS projection and cold recall. This audit found that the older projected path opened exact Replay capsules during Déjà vu. A later source correction starts Déjà vu before exact-capsule work; an isolated 401-observation run measured about 1.7 µs from Déjà vu completion to first Recall work. This does not prove scale independence or live installation. |
| `fast_regions.py`, `flat_vrs.py`, `vrs_evidence.py`, `vrs_refine.py` | Numerical regions, edges, evidence and consolidation. Direct imports are NumPy; `flat_vrs.py` optionally imports SciPy. These are implementation libraries, not a separate truth owner, but their numerical behavior must be preserved in C++. |
| `producer_identity.py`, `provenance.py` | Source and optional producer identity validation. `producer_identity.py` optionally imports `cryptography`; this is provenance checking, not VRS judgment. |
| `engine/__init__.py`, `engine/mosaic_evidence_accumulator.py`, `engine/mosaic_hot_evidence_pages.py`, `engine/mosaic_immutable_numeric.py`, `engine/mosaic_memory_activation.py`, `engine/mosaic_memory_promotion.py`, `engine/mosaic_semantic_family_directory.py`, `engine/mosaic_vrs_address_index.py`, `engine/mosaic_vrs_connectivity_regions.py`, `engine/mosaic_vrs_dependency_index.py`, `engine/mosaic_vrs_event_delta.py`, `engine/mosaic_vrs_event_kernel.py`, `engine/mosaic_vrs_event_signal.py`, `engine/mosaic_vrs_region_arrays.py`, `engine/mosaic_vrs_state_update.py` | First-party SWEGCA numerical/event, activation, evidence, region and state modules. Static imports show NumPy in the array/numeric modules; no external LLM or third-party semantic parser is imported. The 12 recorded native port hashes are checked against source. |

## Active path trace

1. Host transcript → `SessionCapture.scan_transcript` → `VRSClient.ingest_many`
   → loopback `ingest_many` → `Resident.ingest_many` → `Main.ingest_many`
   → native journal and VRS graph. Cursor files contain addresses/accounting,
   not recall content.
2. `LayeredMCP` reads the session VRS first. Only a complete zero-candidate
   session result opens the durable main. A read lease holds the snapshot and
   defers concurrent transcript admission. This path has source tests; live
   desktop handoff remains pending the actual SessionEnd.
3. `ShardedMain` uses cue directory, original capsules and current VRS read
   projection for a cold logical main. It retains regions and portal paths in
   the returned receipt. The exact-address and projected natural paths both
   access Replay data before their Déjà vu objects are constructed. Receipt
   order alone is therefore insufficient evidence of execution order.
4. `conversation_finalize` publishes end intent, captures a stable tail, then
   marks ended and attaches complete session VRS stores. No active session is
   merged due to silence. The unused product `runtime_upgrade.py` branch was
   removed; the separate one-time live handoff supervisor is still pending.

## Corrections made in this sweep

- Removed the wheel-installing Windows script and changed active README/Windows
  instructions to describe source execution and its unverified dependency
  prerequisite. The current architecture release gate now uses the actual
  Déjà vu → Recall metric, with Replay and whole MCP timing separate.
- Replaced wheel/archive release verification with source/entry-point/native-port
  verification. Removed one ignored product `.whl` artifact found in `dist/`.
- Moved the MCP SDK out of product runtime dependencies into the test extra;
  source imports confirmed it is used only by test/smoke clients. Removed the
  optional PyTorch wheel index. NumPy and immutables remain runtime dependencies.
- Exposed the existing hook generator's `module_root` parameter in its CLI so
  source-run lifecycle hooks carry `PYTHONPATH` explicitly.
- Removed dormant product runtime-upgrade code and its dedicated test. Session
  capture, native attachment, and the live one-time post-SessionEnd handoff
  remain separate and unchanged in purpose.

## Open acceptance failures

- Reorder the actual projected and exact-address read execution so Déjà vu
  leads Recall and original Replay follows Recall. Measure the real transition
  without hiding work before the timer or substituting whole MCP latency.
- Verify a complete dependency runtime built from source; the isolated active
  Python environment has no such provenance yet. C++ must eventually replace
  Python/runtime wheel dependence while preserving the SWEGCA state and tests.
- Prove live native main attachment after a real SessionEnd, source parity of
  the active services, region/portal equivalence, 16-thread consolidation,
  4 GiB/5 Gbit/s/500 GB limits, and the correctly defined billion-parameter
  target. None of these follows from the static sweep alone.
- Run Luna/Terra/Sol VRS compaction evaluation only after the repair gates
  pass. Existing plain 250k evidence is retained and should not be rerun.
