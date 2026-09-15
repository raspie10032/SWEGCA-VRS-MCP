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

## Region-scoped recall (same day, later)

`Main.recall(..., region_scope='all' | 'regions' | 'auto')`. `regions`: the rows outside the active
regions and their candidate-portal partners are masked out of candidate generation (a row-mask view
of the index, like the kind exclusion); below `REGION_SCOPE_FLOOR` candidates the whole store is
used; exact address access ignores scope. `auto` applies `regions` only when the whole-store
candidate set has at least `REGION_SCOPE_AUTO_CANDIDATES` (5,000) rows. The receipt
(`region_navigation.scope`) names the allowed regions and the excluded row count. The hook and the
delegation tool request `auto`.

Measured on the migrated copy (15 known-answer queries, kinds fs_listing/test excluded):

| scope | candidates (mean) | recall ms (median) | excluded rows | known answers |
|---|---|---|---|---|
| all | 1,633 | 126 | 0 | 10/15 top-3, MRR .673 |
| regions, hits>=1, portal floor 0 | 1,564 | 115 | 68 | same ranks |
| regions, hits>=1, portal floor .05 | 1,119 | 74 | 513 | MRR .668, one answer 7 -> 251 |
| regions, hits>=2, portal floor .05 | 860 | 49 | 773 | one answer lost |
| regions, hits>=3, portal floor .05 | 528 | 31 | 1,104 | two lost |

Why the safe setting removes so little: 14 populated regions, queries activate 4-9 of them and the
candidate portals all reach the hub region, so the allowed set is nearly the store. The strict
settings buy 2-4x speed at the price of known answers — at 5k records recall is ~120 ms and no
restriction is worth an answer, hence `auto` with a threshold the store does not reach yet.
Making scope pay at scale needs finer regions (the modularity resolution of `fast_regions`) or
activation by cue-hit mass; both are measurements to run when candidate sets grow.

## Fine regions and row-activated scope (same day, later)

`vrs_refine.fine_regions` splits every connectivity region again with the same modularity rule on
its induced subgraph (regions under `FINE_MIN_NODES` = 400 stay whole). Consolidation runs on the
fine regions and stores their labels in the stable version (`coarse_labels` kept); `Graph.labels()`
serves them, padded with -1 (pending) for nodes appended since. Live copy: 16 coarse regions ->
**213 fine** (nodes max 3,174 / median 307; records per region max 922 / median 6); the split costs
3.7 s inside a consolidation; strengths and the whole-store ranking are unchanged (MRR .673).

Activation had to change with it. By cue-node labels the finest safe setting lost 2 of 15 known
answers (a record links to cues in many regions; its own region is not where its cues sit). By
summed cue mass per region (`REGION_ACTIVATION='mass'`) nothing was lost but ranks moved (MRR
.617): a region with one strong record loses to a region with fifty weak ones. The setting kept is
**`'rows'`**: déjà vu as a cheap per-row lexical score (sum of idf over the informative cues a row
carries); the regions of the strongest `REGION_SCOPE_TOP_ROWS` rows are active, plus candidate
portals at or above `PORTAL_SCORE_FLOOR`. The strongest rows are in scope by construction, so the
top ranks are those of the whole store and only the tail is cut.

| setting (15 queries, fs/test excluded) | candidates | ms median / max | known answers |
|---|---|---|---|
| all | 1,633 | 158 / 835 | 10/15, MRR .673 |
| rows 50, portal >= .05 | 700 | 78 / 136 | 10/15, **.676** |
| rows 30 | 521 | 55 / 88 | one lost (the rank-24 answer) |
| rows 20 | 436 | 59 / 85 | one lost |
| rows 10 | 274 | 33 / 106 | four lost |
| with fs listings, rows 30 | 2,161 -> 571 | 167 / 1,589 -> 86 / 146 | one lost |

Defaults: rows 50, portal floor .05, `auto` threshold 2,000 whole-store candidates (location prompts
that admit the folder listings reach 4,600 and take up to 1.6 s unscoped). The tail answer at rank
24 is the price of any scope; below the threshold nothing is cut.

Warm start (later the same day): the fine split starts each coarse region's local moves from the
previous fine membership and gives every resulting sub-region the previous fine id it overlaps
most (unclaimed ids only), so ids stay put for regions that did not change — labels agree 100%
between consecutive consolidations on the live copy — and the per-region convergence skip and
per-region seeds carry across. Connectors are refined in bundles by the source record's region
(with fine regions most edges cross, so one global connector pass would have run every time) and
skipped like regions. Cost after one new record: 3.4 s + 3.6 s to converge (was 25 s + 25 s), of
which the warm fine split is 1.8 s and the input build 0.8 s.

## Fresh questions (same day, last): asks gate, description gate, and what the tuned set hid

The 15 known-answer queries had been used to tune ordering rules and, today, to write asks for
four of their targets, so they no longer measure generalization. 14 fresh questions (user phrasing,
targets = memory docs never touched) gave the honest picture:

| order | tuned 15 (MRR) | fresh 14 (MRR, top-3) | hook injects the answer |
|---|---|---|---|
| BM25 x promotion gate (before today's asks) | .673 | — | — |
| + asks gate x1.5 per rare word in the record's own 「찾을 때 묻는 말」 | .956 | .416, 6/14 | 5/14 |
| + description gate x2 per rare word in a doc's front-matter description | .843 | **.813, 13/14** | **10/14** |

The asks gate exists because a doc that gains an asks section gets longer and BM25's length term
pushed it below short log entries that merely mention the words (rank 7 -> 17). Only 23 of 295
memory docs have an asks section; every doc has a description written in task words, so the
description is the phrasing the rest of the store was missing. `asks_and_description` recognizes
a verdict's tail line, a doc's section heading and a log entry's parenthesized tail — not an
inline mention of the phrase, which let a doc *about* asks claim every example word it quoted.

Region scope re-checked on the fresh set: 50 activating rows dropped one fresh answer entirely;
100 rows keep every answer (fresh .849 scoped vs .813 unscoped, 120 vs 180 ms per query).

