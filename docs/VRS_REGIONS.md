# vrs-regions — VRS rebuilt as region-wise consolidation (2026-09-15)

Branch `vrs-regions` (on top of `local-windows-scale`). This replaces the numerical core of the
native v2.1 store; journal, memory index, regions, checkpoint, daemon and hooks stay.

## Why

The native v2.1 store never runs VRS refinement. `Graph.append` settled the event *signal*
(`settle_event_signal`: scores move, strengths fixed) and a strength moved only through explicit
same-proposition re-evidence (x1.01 / x0.995 on ingress). The v0.2 kernel (`refine_vrs`) is called
only by the legacy package. Measured on the live store (102,308 nodes, 1,044,150 edges): strengths
0.500 x 1,043,825 and 0.505 x 325, every node `unresolved`, **0 edges at or above the promotion
threshold 1.0** — the trusted/untrusted split the design rests on did not exist.

The private runtime shows the same picture: regions (G5) are navigation only ("VRS strength
itself is never changed"), the event-local kernel was "not judged fit for operation" (2026-09-11
ledger), and the whole-graph shuffle lives in `tools/`. The design's G6 (portals between regions)
and G9 (incremental consolidation separate from ingress) were written down but not built.

## What changed

| piece | before (v2.1) | now |
|---|---|---|
| ingress (`Graph.append`) | edges at .5, sign +1, direct .1 presence, every node unresolved, then `settle_event_signal` | edges at .75, sign = record polarity, `direct = tanh(1)` when resolved (outcome success/failure or explicit polarity; not superseded) else 0 + unresolved; a supersede marks the old record unresolved; **no settling** |
| strength dynamics | ingress-time counter | consolidation at idle: `vrs_refine.consolidate` — the v0.2 kernel (x1.01 where endpoint states agree, x0.995 otherwise, clamp base x [.25, 4]) per connectivity region, then the cross-region edges as connectors judged against the settled region states |
| hypotheses | none in the graph | cue nodes (support/refute counts over the resolved records touching them; both -> unresolved) and explicit propositions as virtual nodes inside the consolidation |
| generation | `Graph(snapshot_id, flat, nodes, components, regions, receipt)` | + `stable: VRSVersion` (refined strengths mirrored into the flat arrays, node states/stabilities, proposition edges, per-region deltas, `converged`); edges appended after the last consolidation are *pending* at base |
| certificate chain | pair id digests the settled scores | append id digests (parent, record, edge count, stable version); a consolidation is a **journal row** `{kind: consolidation, seed, cycles, edge_count, node_count, labels_digest, version_id, parent_id}` whose version id digests the refined arrays; replay and `rebuild_from_journal` re-derive it (a rebuild rewrites the row body like the pair ids) |
| promotion | never | `Graph.strength(id) >= 1.0` (max over the record's cue and proposition edges) -> the engine's re-evidence verdict `retained`; recall order BM25 x (1 + .25 promoted) |
| receipts | — | `hook_recall` rows: `vrs {strength, promoted, state, stability, pending}`; packet `vrs_stable`; `status`: `vrs_stable`, `vrs_pending_edges`, `consolidation_stale` |
| daemon | idle checkpoint | + idle consolidation (`consolidate_prepare` under the lock, `consolidate_run` outside, `consolidate_commit` under; 512 cycles per idle pass, repeated until `converged`); `consolidate` command |

Connector pass moves strengths only: letting it also move node states pulled each node between
two fixed points (region pass vs connector pass) and the delta never went below 0.07 — measured.
A region that converged and gained no edge and no changed node input is skipped, so the
steady-state cost is the regions a new record touched plus the connector pass.

## Measurements (live store copy, 5,110 records)

* consolidation chunk (32 cycles, all 16 regions + 216k connectors): 2.4-2.8 s; convergence from
  base in 10 chunks / 30 s; after convergence a chunk that touches nothing: 0.9 s (input build).
* promoted after the second chunk: 2,022 edges = every edge of the 30 resolved records
  (26 failure verdicts, 4 success); pending records decay uniformly to the floor .1875.
* recall order, 15 known-answer queries (fs listings and test records excluded):

| order | top-3 | MRR |
|---|---|---|
| BM25 (baseline) | 9/15 | .550 |
| BM25 x mean strength | 10/15 | .670 after 2 chunks, **.384 after convergence** |
| BM25 x (1 + .25 promoted) | 10/15 | **.673**, identical after 2 and 30 chunks |
| promoted first, then BM25 | 5/15 | .373 |

The raw strength value is not a ranking weight (its magnitude depends on how far the decay has
run); the promotion gate is, which is how v0.2 used strengths (binary `>= 1.0`).

## Migration

A store written under v2.1 rules opens only when its checkpoint is current (no journal rows to
replay); `vrs2-rebuild-graph.py --state <dir>` then re-derives every pair id under the new rules
(5,110 rows: see the session log for the time) and the daemon consolidates at the next idle.

## G6 portals (same day, later)

`vrs_refine.build_portals` runs inside every consolidation: one object per region pair that shares
connector edges — `bridges`, `promoted` (bridges at or above 1.0), `score` = promoted share,
`mean/max_strength`, `status` (`candidate` when at least one bridge is promoted, else `weak`),
`provenance` (the three strongest bridges as (record node, cue node, strength)). Decay and
withdrawal come from the strengths themselves (a bridge whose endpoints stop being resolved decays
at the next consolidation; the portal is withdrawn when its last promoted bridge falls below 1.0);
`portal_events` records opened/withdrawn against the previous version.

Recall (G6 order, kept inside the engine's déjà vu -> recall -> replay -> re-evidence): after déjà
vu the matched cues' regions are *active*; every candidate is classified `local` (its record sits in
an active region), `portal` (reached through a candidate portal from an active region; the pair and
score are reported), `unbridged` (no candidate portal; the reason is reported) or `pending` (not yet
consolidated). Nothing is dropped and access is not restricted (`restricts_memory_access: False`);
`UNBRIDGED_FACTOR` is an optional order factor, 1.0 by default.

Measured on the migrated copy: 77 region pairs share connectors, 9 candidate portals (all through
the hub region 1; scores .011-.020 — only 30 records are resolved, so promoted bridges are rare).
Over the 15 known-answer queries the top-20 candidates were 90% local, 8% via portal, 2%
unbridged; the unbridged factor (1.0 / .9 / .8 / .6) did not move any known answer (MRR .673
throughout), so it stays a receipt. `hook_recall` rows carry `region` {region, path, portal, score};
packets carry `region_navigation` (active regions, portals touching them) and `rejected_paths`.

## Not done here

Region-restricted recall (candidates generated only inside active regions plus portal hops) — the
receipts now say what such a restriction would have excluded; measure that before restricting.
