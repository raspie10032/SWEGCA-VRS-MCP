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

## SWEGCA evidence layer (same day, last) — the numeric core rebuilt on the architecture

Everything above this heading built the numeric core on the v0.2 MCP adapter's star-graph rule
(binary direct = tanh(1) for any resolved record, constant base .75, refinement run to a fixed
point). That produced exactly two strength values (floor and cap) on the live store — not a
property of VRS but of those inputs and of running to convergence. The user's objection was
right, and the core now follows the SWEGCA architecture (`SWEGCA-Architecture/paper/swegca/
ARCHITECTURE_SPEC.md` §3-4, `main.tex` §"Evidence accumulation and admission", and
`src/swegca/mosaic_evidence_accumulator.py`, vendored unchanged as
`engine/mosaic_evidence_accumulator.py`, MPL-2.0):

* **hypotheses are declared propositions only.** "An experience record becomes evidence only when
  it is addressable, relevant to a declared hypothesis, and admitted under the evidence policy."
  A cue is association, not a hypothesis: direct 0, never unresolved, sign +1; its state is numerical
  dependency on the records touching it (the engine's own reading of an edge).
* **evidence observations** per (hypothesis, address = record source, source_family = source file,
  context = project, axis, support/refute from the record's polarity, producer, confidence). The
  producer declares the axes — `swegca-verdict.py` now takes `axes` from the four
  (observational / counterfactual / intervention / cross_context) and the importer carries them as
  `metadata.axes`; undeclared = observational. Evidence from a different context than the
  hypothesis's first is additionally cross-context (a structural fact). Pending and superseded
  records are not evidence.
* **the accumulator's decision is the decision**: Wilson 90% bounds per axis, accept when the minimum
  lower bound exceeds .45, reject when the overall upper bound stays below, abstain on fewer than
  four effective samples per axis, fewer than two source families, fewer than four contexts, or a
  suspected regime change. Groups sharing (source, context) count as fractions. With one producer
  every hypothesis abstains on source diversity — reported as such, not relaxed.
* **kernel inputs are continuous**: hypothesis direct = tanh(ratio x sufficiency), ratio = (effective
  support - refute) / samples, sufficiency = min(1, samples/4); hypothesis unresolved = opposing
  evidence from different source families (the arbiter's directional conflict) or a suspected
  regime change; record weight w = confidence x (1 - contradiction) x (1 - uncertainty) — the
  arbiter's proposal weight — with confidence 1 for an explicit claim and .5 for an outcome-only
  record, contradiction the opposing share on the record's proposition, uncertainty 1 -
  sufficiency; record direct = tanh(w); **w is the base strength of the record's edges**, so the
  clamp base x [.25, 4] means only well-evidenced records can reach the promotion threshold.
* **one refinement per generation** (16 shuffle cycles per consolidation, as the original organizer),
  strengths carried over — a connection's strength is how many generations it stayed stable, not a
  fixed point. `consolidation_stale` = the graph grew since the last consolidation.
* regions are computed on unit weights (navigation topology; trust does not reshape them).
* migration: `vrs2-rebuild-graph.py --drop-consolidations` drops the consolidation rows written
  under the previous rule (derived certificates, not observations) and re-derives the chain.

Two tiers are reported and kept apart: `promoted` (kernel: strength >= 1.0, the engine's
re-evidence `retained`) and the accumulator's `accept` (the architecture's verified promotion).

Live store after migration (`--drop-consolidations`, 5,158 rows, 1,146 s; daemon consolidates one
16-cycle generation per idle spell, 3.6-5.5 s): 29 hypotheses (declared propositions), 31
observations, every decision `abstain: minimum_effective_samples` (one observation each, one producer,
one axis); record weights w in {.25, .5} (one or two members on the proposition); pending edges at
base .05 decaying (.0426 after three generations), verdict edges climbing (.6875 for w = .5 after
three generations; the cap is 4w and promotion needs ~139 stable cycles, nine generations). The
distribution has three distinct values because the store has three distinct evidence states, not
because of the rule: it spreads as records age differently and as evidence gains sources, contexts
and axes. Recall without any promotion: tuned 15 MRR .834 (auto scope .838), fresh 14 MRR .813,
hook 10/14 — carried by BM25 with the asks and description gates.


## Producers (same day, last) — evidence from more than one source

The accumulator abstains on one producer by design, so the next step was not a rule but more
producers writing observations on the **same hypothesis strings** the verdicts declare. Each is a
string proxy for a producer id (`metadata.producer`), with its own source family and context:

| producer | what it observes | where |
| --- | --- | --- |
| `asm-agent` | verdicts (signed v0.2 + vrs2 mirror), axes re-declared 2026-09-15 (`obs:<slug>@axes`) | `swegca-verdict.py`, `swegca-verdict-axes.py` |
| `probe-runner` | re-runs a checkable claim (AF_UNIX present? does one producer leave abstain?) | `vrs2-probe.py` |
| `pytest-runner` | the standalone suite passing on this branch | `vrs2-produce.py` from the shell |
| `recall-bench` | fresh-14 ranks with the promotion gate on vs off, auto scope vs full recall | `vrs2-recall-bench.py` (`PROMOTION_GATE` is a module constant so it can be toggled) |
| `sonnet-subagent` | a delegation's returned yaml: status + tests, and whether `state_corrections` is empty | `vrs2-delegate-record.py` |
| `settlement-batch` | the daily audit batch's exit (0/1; exit 2 "no input" is not an outcome) | `SETTLEMENT/jobs/run_daily.py` finally block |

The common entry is `vrs2-produce.py` (`produce(producer, hypothesis, outcome, axes, context,
source, text, evidence, confidence)` → daemon ingest with `proposition`/`polarity`, `kind:
evidence`). A producer measures a comparison the claim states; it does not pick thresholds.

First live effect (37 observations, 31 hypotheses): the two probed verdicts and the two benched
ones now have source diversity 2 and context diversity 2, still `abstain: minimum_effective_samples`
(fewer than four effective samples per axis — the architecture's answer, not a defect). One
hypothesis became **unresolved**: the 2026-09-15 verdict "region-wise consolidation … gives the
promotion gate a real recall signal" (support, family `verdict:`) against recall-bench's refute
(family `bench:` — under the evidence rule nothing is promoted yet, so the gate moves no rank).
That is the arbiter's directional conflict doing what it should: the verdict was made under the
discarded kernel rule. Record weights now take three values (.25 / .5 / .75).

### Current-vs-past collision (2026-09-17)

The design runs memory activation as Déjà vu → Recall → Replay → Re-evidence, and re-evidence is
meant to collide the *current* request's evidence with past experience and send conflicts to
revalidation. Until today the only collision the hook could show was past-vs-past (the store's own
verdicts disagreeing). Two small pieces close the loop:

* `hook_recall` now returns `conflicts`: for each proposition the re-evidence stage found in
  conflict among the activated candidates, both sides (source, producer, date, polarity) and the
  accumulator's standing decision. The hook renders it as 「재검증 필요 — 〈proposition〉: 지지 … 대
  반박 … · accumulator abstain/…」 followed by the exact call that answers it.
* `vrs2-confirm.py` admits the session's current evidence as an observation on that proposition
  (producer `session-main`, source = the file[:line] just examined so the source family is that
  file, context = project-date, confidence .5 by default because it is a self-report). `--holds` /
  `--fails` are about the proposition, not about the verdict's label. Only a real opposing
  observation on the same proposition counts — labels and shared keywords are not conflicts.

First live use: the 2026-09-15 verdict "region-wise consolidation … gives the promotion gate a
real recall signal" (made under the discarded kernel rule) now stands against recall-bench's
refute and the session's refute (live stable: promoted 0): three source families, abstain,
unresolved — exactly what revalidation should look like until a new verdict supersedes it.

### Usage re-evidence (2026-09-18)

The missing link between Replay and Re-evidence: whether a recalled record was actually opened never
fed back into anything, so VRS strengths could not grow from use. Now:

* the recall hook logs, per injected record, where it can be opened (`opens`: path/offset/limit of the
  「열기」 line); the PostToolUse logger records each `Read` of a memory file with its offset; the Stop
  hook's `usage_ledger.py` matches the two per session (opened = a Read of that file overlapping the
  record's lines after the injection) and keeps `{source: [injected, opened]}`;
* changed counts reach the daemon as a journal row (`kind: usage`, `usage:<digest>`), chained into the
  graph snapshot id (`Graph.with_usage`) so replay and rebuild reproduce the same pair ids;
  `consolidation_stale` also fires when usage changed;
* `build_inputs` gives a *pending* record (no declared proposition) that was opened an association
  base `min(USAGE_CAP .2, .05·(1+log2(1+opened)))` and direct `tanh(.1·log2(1+opened))`, unresolved
  False. Records with a proposition are untouched (the evidence layer decides). The cap keeps
  4 × base = .8 below PROMOTION 1.0: **use makes a record reachable, never verified**.
* receipts show `[열림 opened/injected]`; `vrs_of(..., source)` carries the counts.

Found while testing: the kernel never rewrites a record's state (records are only ever sources), so a
record whose direct changed after its first consolidation kept its pending-time state forever;
`consolidate` now re-seeds record states from their direct every generation.

Backfill over the hooks' history (2026-09-11 → 18): 255 injected sources, 10 opened (doc-level match
for receipts written before `opens` existed; session-log entries excluded there), 201 live sources
journaled. After three generations an opened doc's edges moved .0125 → .0669 while unopened ones
stayed at the floor.

### Hypothesis registry (2026-09-18)

The accumulator keys hypotheses by the exact proposition string, so two producers stating the same
claim in different words never share evidence. A binding `{canonical, aliases}` is journaled
(`kind: alias`, chained into the graph snapshot id, replayed and rebuilt like usage rows);
`vrs_evidence.build` folds an alias's observations into the canonical hypothesis and `build_inputs`
raises one virtual node per canonical proposition with the merged members. A binding is a
declaration, not evidence: no record is superseded or removed, and the accumulator decides as
before over the merged sources, contexts and axes. Tool: `mcp/vrs2-alias.py --canonical … --alias …`
(`--list` shows live propositions with observation counts and producers). Unknown propositions are
refused. Live registry on 2026-09-18: empty — no two live propositions are the same claim yet; the
first candidates will come from `settlement-batch` versus a verdict about the same batch.

### Batch generations (2026-09-18, later) — ingest cost was O(E) per record

The first scale run (`mcp/vrs2-scale-curve.py`, synthetic records mixed from live text) showed
ingest cost rising linearly with the store: 150 ms/record at 2k, 271 at 4k, 385 at 6k, 498 at 8k,
651 at 10k — before memory was any concern (RSS 553 MB at 10k). A cProfile of 40 ingests on a
copy of the live store (5,621 records, ~1.1 M edges) put all of it inside `Graph.append`: every
record rebuilt the whole generation — `build_regions` 294 ms (level-1 `_csr`, `_memberships`
and the frozen edge copies walk every edge even with the warm start), `FlatGraph.__init__`
129 ms (`flat.extend` concatenates seven arrays and argsorts twice), the component map loop
~30 ms (every node), `csr_cache.extended` 20 ms. Checkpoints were not the cause.

Fix: K observations enter as **one generation**. `Graph.append_many` extends the flat arrays once
and builds the regions once (`append` is the K = 1 case, byte-for-byte the old generation);
`Main.ingest_many(rows)` validates all rows first, appends the memory rows in order, and writes
the K journal rows in one transaction **sharing the batch's pair id**. That shared id is the
batch marker: the replay after a checkpoint regroups consecutive observation rows with one pair
id into one batch and calls `append_many` once, so the region labels — which the consolidation
certificate digests through `labels_digest` — come out identical (crash-replay test with
batches of 20/5/1/4 plus a consolidation: pair id, count and `version_id` reproduced).
Batches are all-or-nothing; a row already journaled with the same content is an idempotent
replay inside the batch, a reused request id with other content refuses the whole batch.
`rebuild_from_journal` stays sequential (it re-derives the chain anyway; a rebuild of 5.6k rows
is ~47 min and grows quadratically — a batched rebuild is the next step if that ever matters).

Measured on the live copy: K = 1 770 ms/record, K = 10 47, K = 100 43 (includes a checkpoint),
K = 500 **11.5 ms/record** (includes a checkpoint). Callers: the daemon command `ingest_many`
(`rows=[…]`), the Stop-hook reindex (one batch per changed file, per-row fallback on any
refusal or an older daemon), `vrs2-import.py --batch 200` (0 = per row), `vrs2-scale-curve.py
--batch 500`. Single-record producers (`vrs2-produce.py`, sheet index) are unchanged.

Found on the way and fixed in the same change: `Graph.append` constructed the successor without
`usage` and `aliases`, so every ingest since the morning silently emptied both journaled maps
(the journal rows were intact; only the live generation lost them). `append_many` and
`rebuild_regions` now carry both; `tests/standalone/test_batch.py` pins it.

### Recall columns (2026-09-18, later) — recall cost was one episode build per candidate row

The same scale run put recall at 2.2 s (auto scope) / 11.4 s (all) per query at 10k records. A
profile on that store: 83% of an auto-scope recall was the proposition closure at
`store.recall` — `memory.episode(i)` for every record in every informative cue's posting list,
just to read its proposition id — and the rest was the engine building every candidate
(`recall_memory`, ~330 vocabulary lookups per record for cue strings the stages never use) and
replaying every candidate (`replay_memory`, full builds again), plus the asks gate building each
candidate to read its text.

Fix, all in the compact index (the engine is untouched):

* per-row columns `props`, `asks` (casefolded asks/description pair), `revs`, `outs`, appended
  with the row and backfilled once from the blobs for a checkpoint written before them;
  `rows_for_cue`, `proposition_of(_row)`, `asks_of` read them (the masked view keeps its mask);
* `recall_candidates(signal)`: the engine's `recall_memory` on cue ids — the same candidate set,
  the same `matched_cues` in record order, the same Jaccard and the same order (checked equal
  on 8 live queries and in `test_recall_columns.py`), decoding only the matched cue strings;
* `episode_light` / `LightView`: a record without its cue strings for the replay and re-evidence
  stages and for the opponents check (steps, sources, revision are all they read), cached apart
  from full episodes.

Measured at 10k records (the old-code lab store): auto 3.4 s → **0.33 s**, all 12.7 s → **0.73 s**
per query. Live store (5.6k, warm daemon): hook recall 186 → 69 ms. Ranking unchanged: the
14 fresh questions give MRR .778 before and after (and, as of this store, the promotion gate
moves no rank at all — `promotion-signal` now has a refuting observation of its own).

### Re-measurement supersedes, contradiction stays (2026-09-18, later)

The first collision to close itself: the morning's `promotion-signal` bench row (gate moved ranks
at MRR .747/.759) sat in support against three refuting sources, and the recall hook printed
「※ 재검증 필요」 on every prompt. After the ask-gate fix the same bench measures "the gate moves
no rank" (.778 = .778). That is not new opposing evidence — it is the same measurement taken
again — so the bench's re-run **supersedes** its own earlier row instead of adding a contradicting
one: `vrs2-produce.py` takes `supersede_same_source=True`, asks the daemon's new lock-free
`evidence_of(proposition, source)` for this producer's live row at that source, and ingests with
`supersedes`. A bench label carries the date, so one live point per bench per day; a run on
another day is a new point. Rows from *other* producers are never touched — a genuine
contradiction still has to be closed by a verdict. After the re-run: four live rows, all refuting,
conflicts 0.

### Live candidates fill the hook packet (2026-09-19)

Found while checking the compaction chain, not by a failing test. `hook_recall` cut the candidate
list at `limit` *before* marking `superseded_by`, and the hook drops superseded rows — so a document
revised many times spent the packet's slots on revisions the hook would never show. Measured on this
session's last 8 prompts (limit 10): 63/80 rows live; on 「압축 지나왔는데 영속성 체크」 2/10 (one
memory document had 7 revisions in the top 8; 26 superseded candidates sat above the tenth live one).
The store still recalls old revisions (the contract: superseded records stay recallable, only the
current one grounds a judgment); the packet now walks the ranked candidates and takes the first
`limit` live ones, reporting `superseded_skipped`. After the fix the same 8 prompts: 80/80 live,
21–176 ms. The recall bench was never affected — it ranked among non-superseded candidates from
the start — so its MRR does not move; only the hook and the delegation packet see more.

### Origin binding — G3 (2026-09-19)

The goal document's G3 asks that a record be bound to its source's address and digest and that the store
be able to verify byte-exact preservation. Until today a record carried `metadata.path`, a `revision`
(12 hex of its text) and a head line the hook re-found at open time — and when the head was not found
the 「열기」 line silently pointed at line 1. Now:

* **Binding at ingest** (`vrs2-import.py`, `harness/origin.py`): every log entry, document and document
  section carries `metadata.origin = {bytes: [start, end), lines: [first, last], sha256, size, mtime_ns}` —
  the byte span of the raw part in the file, the line span of the stored text, the full SHA-256 of the
  stored text. `revision` is `sha256[:12]`, so no record's identity changed: checked over every memory file
  of every project, 2,990 revisions equal to the manifest, 3,676 spans slicing back to the stored text
  (CRLF files included — text is what text mode reads, spans are on the file's bytes).
* **`origin.verify`** re-reads the file and answers `intact` (bytes at the span still hash to the record),
  `moved` (same digest elsewhere in the file), `changed` (no such digest, but the record's source key —
  the head line, or `#index:title` for a section — still names a part: the record is an older revision)
  or `missing` (file gone, or the key gone: a relabeled entry, a shifted section index, a deleted document).
  Rows ingested before the binding verify by the digest of their stored text (the daemon sends
  `text_sha256` in the hook packet for them); a row's absence of an origin is not a failure.
* **The hook's 「열기」 line** carries the state: a moved record opens at its current lines and says so, a
  changed one is marked 「※ 원본 바뀜 — 토막은 색인 때 판본」 and opens the current version, a missing one
  says 「※ 원본 없음」 instead of pointing at line 1.
* **`vrs2-verify-origin.py`** runs it over the live store (daemon command `origins`, paged under the
  transport's 1 MB line; parts and files cached per stat — 3,032 records in 0.35 s). First live run:
  3,004 intact, 0 moved, 10 changed, 18 missing. The 18 were the leftovers of the 2026-09-14 label edits
  (the same body live twice under an old and a new key) plus three deleted document sections and one
  deleted document; four of the 10 "changed" were sections whose index had shifted (their key was gone, so
  the rule above now calls them missing). **Retirement** (`--retire`, `--dry-run` first): a missing row is
  superseded by one small journal row of `kind=retirement` that says where the body lives on (`continues`
  = the current source key, found by the label-stripped head) or that the source is gone. Nothing is
  deleted; the hook and the delegation packet exclude the kind. After retiring 22 rows: 3,004 intact,
  6 changed (edits in other projects' memory files that their own Stop hook supersedes at its next run),
  0 missing.

Memory ≠ truth still holds: `intact` certifies the source, not the record.
