# VRS 2.2 MCP region-path audit — 2026-09-22

This is a source and live-transport audit, not a performance result or a
claim that the repaired source is installed. The active repair branch is
`codex/vrs22-regions-repair`.

## Verified execution path

- The memory MCP entry calls `Main.recall(query, expected_pair)` without a
  region argument (`src/swegca_vrs2/server.py:55`). `Main.recall` defaults to
  `region_scope='all'` (`src/swegca_vrs2/store.py:1293`). The alternate hook
  recall also defaults to `all` when no explicit scope is supplied
  (`src/swegca_vrs2/loopback.py:345-350`). Thus the normal MCP call does not
  execute the region-scoped candidate preparation at `store.py:1340-1430`.
- `detect_deja_vu` and `recall_candidates` execute at `store.py:1431-1433`.
  The following navigation block at `store.py:1434-1488` labels already
  recalled candidates as local, member, portal, unbridged or pending. It does
  not supply those portal keys to that Recall. Its receipt can be useful, but
  it cannot establish that the high-speed navigation preceded Recall.
- `Graph.rebuild_regions` builds each component from a temporary
  `SimpleNamespace` (`store.py:482-493`). The resulting topology retains that
  temporary object as `source`. `ConnectivityRegions.require_pair` demands
  that the exact source object be reachable from `pair.memory` and that its
  VRS snapshot ID match (`engine/mosaic_vrs_connectivity_regions.py:228-239`).
  `Main` currently constructs `FullCurrentMemoryVrsSnapshot(memory,
  graph.snapshot_id)` with only the compact original-memory index
  (`store.py:777,796`). The component source is absent from that pair.

An isolated `Main` instance confirmed both faults after one original record
and one consolidation: it had one component, `pair.memory` was `CompactIndex`,
the component topology's `vrs_snapshot_id` differed from the pair's, and
`region.require_pair(pair)` raised `region topology belongs to a different VRS
generation`. The source identity would still be absent after fixing only the
generation ID. No live state was changed by this probe.

## Evidence semantics that an integration must preserve

The product graph connects original records to literal cues for numerical
navigation (`store.py:288-294,365-428`). The author's separate
`VRSHotMemorySource` turns its own term-edge signs into support/refutation
episodes (`tinylm-slicer-sanabi-bazzite/src/tinylm_slicer/mosaic_vrs_memory_bridge.py:318-362`)
and treating product record-to-cue edges that way would create false
original evidence. A region binding must keep those edges structural and must
leave Recall, Replay and Re-evidence on the existing original episodes with
their source, revision, uncertainty and opposing evidence intact. It must also
retain the product's component split and shared-original portal keys.

## Live transport boundary

In the current Codex task, the exposed `mcp__swegca_vrs__memory_status` tool
still returns `tool_request_failed`. The installed `PreToolUse` command was
run with an exact synthetic event and returned an `updatedInput` containing
the exact session ID. A separate JSON-line MCP stdio call using the actual
task session ID completed `memory_status` -> `memory_context` ->
`memory_release` on the session layer, with 13,411 candidates for one broad
diagnostic query. This proves the session resident transport, but does not
prove the Codex app ran its hook for the nested call, that the candidate order
is correct, or that the four-stage region path is repaired.

The source test `tests/standalone/test_session_vrs.py` passed its first 13
tests and timed out once while starting a detached finalizer's daemon. That
specific test passed on an isolated rerun (32.53 s). The full file therefore
has no clean pass from this audit, and the first timeout is not yet assigned a
code cause. No VRS model-performance evaluation should use this state.

## Required next proof

1. Bind each component's real structural source to the main-owned, pinned
   full-current pair without turning numerical associations into evidence or
   dropping an original episode. Preserve pair/journal integrity across
   restart and exact source identity across a generation.
2. Show the Déjà vu key activates region/portal/shared-original navigation
   before Recall, while the original four stages and complete fallback remain
   accessible. A receipt computed only after Recall is insufficient.
3. Prove the Codex app's nested MCP call receives the exact session ID through
   the trusted hook, then verify a session-first recall and release in that
   same task. Direct stdio success is a separate, narrower result.
4. Only after those proofs, measure the user's actual Déjà vu -> Recall
   transition and continue the resource and compression tests.
