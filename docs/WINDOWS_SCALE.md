# Windows-scale local adapter (branch `local-windows-scale`, 2026-09-14)

v2.1.0 bundles the standalone main and passes its 21 tests on Windows, but with real
records it does not scale: each record carries hundreds of cue nodes (every token plus
every 2–4-character Hangul substring), those cues are shared with most other records,
and the ported composition (a) settles the event signal one node at a time in Python
over trie-backed sparse successors, (b) re-clusters the whole connected component on
every ingress, and (c) persists only the observation journal, so a restart replays
every ingress. Measured on session-log chunks (median ~1,000 chars): ingress cost grew
about 70 ms per already-stored record (record 1: 175 ms, record 30: 2.2 s; over stdio
at 137 records 10 s/record), and reopening a 40-record state took 69 s.

This branch keeps the engine's rules and changes the representation, the arithmetic
vehicle and the persistence. Ported engine files under `engine/` are untouched.

## What changed

| Piece | Rule kept | Change |
|---|---|---|
| `flat_vrs.py` | `next = f32(.8·old + .2·tanh(direct + .2·signal/max(1,Σ\|strength\|)))`, synchronous rounds, a node whose float32 bits changed revisits itself and its outgoing targets, round budget stays a budget | flat immutable bytes-backed arrays; one round = one SciPy CSR matrix–vector product applied to pending nodes only; propagation through a 0/1 adjacency product |
| `store.Graph` | node/edge construction, direct .1, explicit same-proposition re-evidence (×1.01 / ×.995 / conflict abstain / duplicate preserve), settled-snapshot digest, receipt keys | built on `flat_vrs`; regions per changed component with unchanged components shared (`graph.components`, `graph.regions` Maps) |
| `fast_regions.py` | weighted-modularity local moves with the engine's gain and tolerance, multi-level aggregation, association-mass memberships, topology digest | components above 2,000 nodes: a few synchronous sweeps (even/odd halves) then the engine's sequential rule on the active set (monotone → converges); level-0 warm start from the previous generation's cores; small components use `ConnectivityRegions.build` unchanged |
| `store.Main` | journal is the source of truth; every row re-validated on restart; single owner lock | `checkpoint` table (pickled hot index + flat generation + request table) written every 8 ingests and on close; restart loads it, re-validates all journal fingerprints, replays only rows after it |
| `store.Main.recall` | all matching lexical keys generate candidates (no top-k); explicit proposition closure; four stages; hot path without I/O/JSON/hash | closure opens only through *informative* cues (fanout below half the store — Korean one-character particles matched nearly everything); candidate order = BM25 over the matched informative *words* (a matched Hangul n-gram cue that is a substring of a longer matched cue is the same word and is scored once, by the longest form; tf is 1 in a cue set, idf from postings fanout, length norm from the record's cue count, k1 1.2 / b 0.75), then the engine's Jaccard (order is declared non-semantic by the engine; the whole set stays addressable); selection rules are reported in `memory_selection`. Measured on 8 delegation questions against the 2,620-record store: this rule puts the sealed answer in the top 5 for 6/8; a rare-word "anchor" tier (rank first by matched words with idf >= 4, or by the stem's idf) was tried and is worse (3/8) because Korean inflected instruction words (설명해, 건드리면, 이유를) are as rare as topic identifiers |

## Verification

* `tools/compare_flat_vrs.py N` drives the tagged v2.1.0 `store.py` (via `git show`) and
  the flat path on the same real records and compares settled scores bit-for-bit:
  0 differing nodes over 20 generations.
* `tests/standalone`: 21 passed (restart, identity, snapshot pinning, conflicts,
  revisions, corruption rejection, hot-cognition I/O ban, shared unrelated components).
* Regions on a 6.6k-node component: engine sequential build modularity 0.5231 in 691 ms;
  vectorized cold 0.5233 in 135 ms; warm 0.5494 in 92 ms.

## Measurements (this machine)

| records | nodes | edges | ingest | restart | query |
|---:|---:|---:|---:|---:|---:|
| 40 | 6.6k | 25k | 50–120 ms | 16–30 ms | 2 ms |
| 500 | — | — | ~470 ms (stdio) | 0.9 s incl. process start | 20–36 ms |
| 805 | 60k | 500k | 0.6–0.7 s | 0.4 s (45 MB checkpoint) | 22 ms |

## Not changed, worth knowing

* Cue extraction (`keys`) still turns every Hangul word into all 2–4-character
  substrings; this is what makes the graph large. Left as designed.
* Checkpoint size grows with the store (~55 KB per record here). Loading it is the
  restart cost now.
* One process owns a state directory. Hooks or a second client cannot open the store
  while a stdio server holds it; see the integration notes in the project memory.

## 2026-09-14 (later): node set, incremental regions CSR, idle checkpoint

Measured on the real 2,696-record store (Korean session-log entries, memory-doc sections, verdicts):

| change | before | after |
|---|---|---|
| generated Hangul n-gram cues as VRS nodes (`store.graph_cue_ids`; `VRS2_GRAPH_SUBSTRING_CUES=1` restores) | 169,718 nodes / 1,693,046 edges | 89,773 / 800,426 |
| ingest, one record (settle + regions + extend) | 2.2 s | 0.97 s |
| + incremental level-0 regions CSR (`csr_cache.py`, bit-identical to engine `_csr`) | 0.97 s | 0.72 s |
| checkpoint cadence | every 8 ingests, 2.5–3.7 s inside the ingest | after 5 s idle in the daemon (in-ingest bound 64) |
| daemon RSS / checkpoint / disk after compact | 331 MB / 22–28 MB / 30.7 MB | 233 MB / 16 MB / 19 MB |
| restart | 1.6–2.2 s | 1.7 s |
| hook_recall warm | 72–130 ms | ~110 ms |

* A node-rule change is a new certificate chain: VRS snapshot ids digest the settled values, so
  `Main.rebuild_from_journal` (tool: `vrs2-rebuild-graph.py`) re-derives and rewrites the per-row
  pair ids in the live journal and the archive segments; row bodies and fingerprints are untouched.
  2,696 rows took 1,176 s.
* The rebuild also applies the current `keys()` to old records (mixed-script splits), which moved the
  15 known-answer queries from MRR 0.694 to 0.711.
* VRS strengths as a ranking signal: measured no effect — top-50 candidate strengths are 0.500 with
  two distinct values in this single-producer store (verdict
  `vrs-strengths-carry-no-ranking-signal-in-a-single-producer-store`).
* `hook_recall` now reports up to 64 matched cues (the 12 cap undercounted evidence in 9% of rows).
