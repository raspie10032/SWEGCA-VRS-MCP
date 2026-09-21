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

### Resident layer — the minimal G7 (2026-09-19)

The 60k bundle rule needs a way to keep more than one bundle without more than one process. Now one
daemon answers for several bundles (`resident.py`):

* **Bundles** are registered in `~/.claude/vrs2.json` (`bundles: {id: dir}`, `bundle_of: {project slug:
  id}`, `hot_bundles`; `vrs2-install.py --bundle/--bundle-of/--hot-bundles`). The daemon's own store is
  the primary — HOT: index + VRS graph, the engine's recall with regions, promotion and re-evidence.
* **Warm** = the index alone. A checkpoint now writes a second blob, `checkpoint_warm` (the memory index,
  16.4 MB on the live store against 25.8 MB for the full checkpoint); a `WarmView` loads it read-only
  from the bundle's own sqlite and follows the bundle's journal (rows after the checkpoint; a newer
  checkpoint by the owner reloads). Measured on a copy of the live store, 5,702 records: warm 189 MB /
  0.39 s against hot 293 MB / 1.17 s. The index dominates at this size; the graph's share grows with N
  (~300 edges per record).
* **Hot by use**: an ingest that names another bundle (`ingest`/`ingest_many` with `bundle=<id>`, the Stop
  hook passes its project's) opens it owned; beyond `hot_bundles` the least recently used one is evicted —
  checkpoint + close in a background thread (a close is seconds; the hook client times out at 5 s, and a
  retried eviction ran twice before this) — and is warm again at the next read. Hot secondary bundles
  checkpoint after the same quiet spell as the primary; consolidation of a secondary bundle is not run
  here (it refines when it is someone's primary).
* **Recall across bundles**: `hook_recall` returns the primary's rows first (VRS, regions, verdicts), then
  every other bundle's rows in its own index order — BM25 over the informative cues, every matched cue
  admitting a candidate as the engine does, no strengths — each tagged `bundle`/`bundle_state` and carrying
  its bundle's fanout and size so the hook's evidence gate judges it against the right store. The hook
  prints 「뭉치 <id>·warm」. Measured with one warm copy of the live store beside the primary, 8 prompts:
  156 ms median with the second bundle against 160 ms without (max 224 ms) — the warm candidate pass is a
  few milliseconds at 5.7k records.
* **Directory**: `lookup <episode_id>` names the bundle that holds a record (primary first, then each
  bundle's index); `bundles` and `status.bundles` list every bundle with its state (hot / warm / closing /
  cold), records and fill. `evict <id>` returns a bundle to warm.

Residency is a rank, not a strength: where a bundle sits says nothing about its VRS, and a record is
addressable in every state (G7: `residency rank != VRS strength`). Not built: prefetch, hysteresis beyond
LRU, cross-bundle re-evidence (a proposition's evidence in two bundles is two separate judgments), and the
cache-miss scheduler (G8) — a warm view is loaded inside the request that first needs it (0.4 s once).

Found while wiring it, live: the origin binding was the first nested map in a record's metadata; the
store freezes metadata (`mappingproxy`), the daemon's JSON reply could not encode it, and the connection
closed without a reply — the hook printed nothing for any prompt whose packet held a fresh row, from
10:58 to about 11:15, with no receipt (the shim swallows exceptions). Every packet row now carries `plain()`
metadata (regression test), and the hook writes an `error` receipt for its own failures.

### Prepare outside the judgment — the minimal G8 (2026-09-19)

G8 says: storage access, decoding and loading belong to a preparer, not to the live judgment; a judgment
runs on a pinned immutable view; what it could not reach is a **named miss**, answered on the next
judgment; hit and miss latencies are measured apart, never folded into one number. Where the misses
were, and what changed:

* **Blob decodes.** A record's body is a zlib+json blob; the judgment reads columns, but replay,
  re-evidence and the packet rows read episodes, and after a checkpoint load the caches are empty.
  Now the index counts `hits` (answered from a resident episode) and `decodes`; every packet reports
  its delta (`blob_hits`, `blob_decodes`, a `blob_decodes` miss) and the hook's receipt keeps it.
  The packet rows use light episodes (no cue strings). The **preparer** thread prefetches the light
  cache newest-first at start-up (live: 2,130 rows in 84 ms after a load that already carried 3.6k;
  a copy with empty caches: 5,743 rows in 302 ms) so the first judgments run from RAM.
* **Warm bundles.** A judgment never loads or refreshes a bundle any more: `Resident.ready()` pins what
  the preparer has, and a bundle that is not loaded (or is closing) is a `bundle_not_ready` miss —
  the packet says so, the hook prints one line 「뭉치 … 는 아직 준비 중 — 다음 프롬프트부터」, the
  preparer loads it (`wanted`) and refreshes every warm bundle whose journal moved, every 5 s. Measured
  on a live copy: the first judgment with a 5.7k bundle not ready answered in 124 ms with the miss
  named (before: +378 ms of loading inside the request); after the preparer's 378 ms the next judgment
  carried the bundle's rows in 6 ms.
* **A starting daemon.** The first prompt after a cold start used to block until the store was loaded
  (1.2 s at 5.7k, tens of seconds at 60k), and two prompts arriving together spawned two daemons (the
  second died on the owner lock after loading). Now the spawner leaves a start marker
  (`loopback.starting`, cleared once the daemon serves, ignored after 5 min), a second caller waits
  instead of spawning, and the hook waits 2.5 s at most: past it the prompt goes without memory and
  says so (`DaemonStarting`, receipt `miss: daemon_starting`). Measured live: 1st prompt 416 ms with
  the notice, 2nd 402 ms (no second spawn), 3rd after 3 s 151 ms with memory.
* **Timing apart.** Every packet carries `timing = {judgment_ms, rows_ms, bundles_ms}`; the receipt
  line of every prompt has it beside the end-to-end `ms`. On the live store the judgment is ~130 ms
  of the ~150 ms hook (rows 2 ms, other bundles 0–6 ms).

Not built: a residency budget (the preparer prefetches up to the light-cache size, 16k rows, newest
first — beyond that the cache is LRU and the miss counter shows what that costs), thrashing detection,
and full-result equivalence checks between a judgment with and without a warm bundle.

### Machine results close the loop — the minimal G11 (2026-09-19)

G11 asks for the whole loop: intent → action → *actual outcome* → experience → VRS → later cognition.
Before this step the store's evidence came from claims (verdicts, confirmations, bench notes, the gates'
repeat counts); the only thing that ran and wrote back was a guard. A survey of the live store (5,746
records, 42 hypotheses, 66 observations) showed every hypothesis abstaining at the accumulator's first
check, `minimum_effective_samples`, and **not one observation on the `intervention` or
`counterfactual` axis** — nothing a claim can supply; only a run on the changed tree and the same run on
the tree without the change can. The diversity checks that the paper is about were never reached.

* **One contract for a machine result** — `harness/results.py`, CLI `vrs2-run.py`. A command is run as
  the experiment for a hypothesis: `--axis intervention` (on the changed tree) or `counterfactual`
  (on the unchanged tree, usually `--expect failure`: without the fix the test must fail) or
  `observational`. The row's `outcome` is the experiment's (the prediction held or not); the raw exit
  code, command, duration, cwd and a **tree digest** (git HEAD + a digest of the dirty state, or the
  digest of `--watch` files for a copy under test) are bound in `metadata.run`. The producer is the
  runner (`pytest-runner`, `<script>-runner`, `stop-hook`) — a distinct source from the agent's own
  claims; the source family is the command digest, so re-running the same command is one group for the
  accumulator (repetition is not new evidence — a settled verdict). A counterfactual on the same tree
  digest as its intervention run is refused. Every run is a line of `vrs2_run.log` before anything is
  sent; a line the daemon could not take waits there (`pending`) and `flush()` — the Stop hook calls
  it — sends it later, so a result is never lost to a daemon that was down.
* **The hook's own run is a result.** `reindex.py` records its failure every time and its success once
  a day per project (producer `stop-hook`, claim "the stop hook indexes the changed memory files of the
  current project"), and flushes pending run-ledger lines on the same client.
* **The standing named.** `Hypothesis.summary()` now carries per-axis effective samples and the counts
  of producers / contexts / source families; `vrs_evidence.gaps()` turns that into what the accumulator
  still needs before its next check. Packet rows with a proposition carry `decision` (status, reason,
  gaps, one tag), `evidence_of` returns it with the rows, and the hook prints it on every verdict line:
  `[증거 abstain: 반사실 0/4·개입 1/4·프로듀서 2/4]`. An abstain with its gaps is the loop's state, not a
  verdict on the claim — and it tells the agent which run would move it.

**A defect the new test exposed** (not found by suspicion: the contract test failed on `intervention 0`):
`vrs_evidence._axes_of` tested `isinstance(metadata, dict)`, but the store hands metadata back frozen
(`mappingproxy`), so **every declared axis since 2026-09-15 was read as observational** — the verdict
axes tool, the bench, the probe and the delegate returns all declared `intervention` / `counterfactual`
and none of it reached the accumulator. With the `Mapping` check the live survey shows those axes
populated (e.g. the consolidation-threshold claim: intervention 3, counterfactual 3); all 43
hypotheses still abstain on `minimum_effective_samples`, but now for a gap that is real and named. The
record weights (`sufficiency` counts samples over the required axes) shift with it at the next
consolidation; ranking never used them.

Measured live: the contract test on the pre-fix module (a copy tree, `--watch` digest) failed as
predicted → counterfactual row, exit 1, experiment success; the same test on the fixed tree →
intervention row, exit 0; the full suite (65 tests then, 17.9 s) → an intervention row for the suite claim.
After one consolidation the hook shows `[증거 abstain: 관측 0/4·반사실 1/4·개입 1/4·교차맥락 0/4·프로듀서
1/4·맥락 1/4]` on the new claim. `test_g11_results.py` proves the escape is reachable through the
runner alone: 96 real subprocess runs — 2 commands × 4 contexts × 4 producers × 3 declared axes —
take a hypothesis to `accept` with the accumulator unchanged (its bar is the paper's: four groups per
axis, four producers, four contexts; nothing here relaxes it).

Not built: automatic pairing of before/after runs (the runner records tree digests and refuses the
same-tree pair; it does not run both sides for you), a pytest plugin (the wrapper runs the command),
batch schedulers (the contract is there; registering a task is the user's), and any change to the
accumulator's minimums.

### Shared experiences are the keys — the minimal G5 (2026-09-19, R3/R4)

The goal's G5 asks for soft multi-membership and overlap; the user's correction R4 says what the
overlap is *for*: "여러 그룹을 잇는 경험은 다중 소속 — 이게 바로 그룹과 그룹을 이어주는 키다". One
original experience that belongs to two regions is the key between them — no copies per region, no
link that no experience carries, and a crossing must be replayable: which shared experience, which
revision, against which version. Until now the regions were exclusive labels and the G6 portals were
aggregates of connector *edges* (record → cue across the pair) scored by promoted share; a crossing named
a pair and a score, never an experience.

* **Membership** (`vrs_refine.build_memberships`, at consolidation): a record's membership in a fine
  region is the share of its forward-edge strength mass whose cue endpoint sits in that region — the
  engine's own membership rule (`_memberships` in `mosaic_vrs_connectivity_regions`) applied to the
  navigation regions. Kept at or above `MEMBER_FLOOR = .05` as CSR arrays in the stable version
  (`member_ptr/region/weight`, ~20k entries live, 233 KB), deterministic from (edges, strengths,
  labels) — `memberships_digest`. A record is *a member* of a region at `SHARED_FLOOR = .2` and
  a member of two regions is a **shared experience**. Cues and propositions have no memberships.
  Measured on the live store (5,757 records, 347 fine regions): a record's mass spreads over 19 regions
  on average, its own label is the strongest 94.5 % of the time; shared records at .2 are 25 % (mostly
  listings), at .3 13 %, at .4 4 %; .2 keeps 1.25 regions per record.
* **Keys** (`key_portals`): every portal whose pair has shared experiences carries them —
  `keys = [{node, weights (w_a, w_b), strength}]`, strongest `min(weight) × strength` first, at most 8,
  and `shared` = their count. No pair is created: a membership *is* connector-edge mass, so a keyed
  pair always already has a portal (live: 117 keyed pairs, all among the 6,028 edge pairs — but only
  6 of them among the 228 pairs a promoted edge had made 'candidate'; the two rules see different
  bridges).
* **Navigation** (`Main.recall`): a candidate outside the active regions is `member` when it is
  itself a member of one (`in_region`, `weight`); otherwise a crossing prefers a keyed pair over an
  edge-only candidate pair and names the key — `via = {episode_id, revision, outcome, weights,
  strength, shared}`; an edge-only crossing says `bridge: edges only`. Region scope admits the members
  of the active regions (`member_rows`) and, with `KEYED_PARTNERS_IN_SCOPE`, keyed pairs as partners
  (`keyed_partners`). The receipt counts paths by kind and `crossings_keyed`; the hook's receipt line
  keeps both per prompt.

Measured on a live copy (scope forced, 20 recent prompts): member rows admitted median 114 per prompt;
excluded rows median 436 → 145 once keyed pairs are partners; top-10 against whole-store recall unchanged
(9.8/10 before and after). The wider scope is not free: an A/B of `KEYED_PARTNERS_IN_SCOPE` on four
scoped prompts (5 runs each, two rounds) costs +15–30 ms per judgment (165 → 186, 139 → 176, 158 → 201,
145 → 170 ms; allowed regions 38–79 → 73–95) — the extra rows scored. At 5.7k that is a fifth of the hook;
at 60k+ it is to be re-measured against the 1 s line before the constant stays on. Paths over the 20
prompts: local 19,766 · member 1,464
(4 %) · portal 6,651, of which **2,988 (45 %) cross through a named shared experience** · unbridged
7,534. Nothing is dropped and nothing certified: membership and keys are association (the goal's
"membership_is_truth=False"); a listing record that is a member of many regions is a poor key by its
strength, not by rule.

Not built: cross-bundle shared experiences (a joint graph would be needed; R4's key stays within one
bundle for now), the association components R4 lists beyond connectivity (co-activation, entity, time,
action-outcome, usefulness, agenda — usage re-evidence exists but does not enter membership), and the
scale re-measurement of the wider scope at 60k+ (the 5.7k numbers cost nothing).

### Signed producers and users — the sixth step toward 3.0 (2026-09-19)

3.0 is the same contract beyond one bundle, one producer, one user. The accumulator counts producers as the
paper's "distinct source" proxy, and in vrs2 a producer was a string in `metadata.producer` — anything could
claim `asm-agent` or `pytest-runner`. That is harmless while one person's tools feed one store; it is worth
nothing once several users or machines do.

* **Identity** — `harness/identity.py`, CLI `vrs2-identity.py`. A producer id is bound to an Ed25519 key pair
  (`cryptography`, optional): the private seed stays with the producer (`~/.claude/vrs2-keys/<id>.key`),
  the registry holds public keys only (`~/.claude/vrs2-producers.json`: `key_id`, `user`, `since`) and can
  be shared between machines. `produce()` and the importer sign a row when the machine holds its producer's
  key — over (producer, hypothesis, outcome, axes, context, source, revision, text digest), canonical JSON —
  and the daemon verifies on ingest (`ingest`, `ingest_many`): a registered producer's row gets
  `metadata.verified = True | False` (+ `key_id`); an unregistered producer's row is left as it is.
* **What verification changes.** A `verified: False` row (a registered id without its key, a borrowed key, a
  tampered field) is folded into no hypothesis — recallable, weight 0 — because it says nothing about *who*
  observed; it is not counted as a producer of its own either. Rows from before this step carry no
  `verified` key and count as they always did; unregistered ids stay proxies. Signing is identity binding,
  not truth: a verified row is that producer's claim and the accumulator decides as before.
* **User** — `~/.claude/vrs2.json` `user` (default the OS login) rides on every produced row
  (`metadata.user`), in the registry entry of each producer, and in every hook receipt. It is provenance:
  no per-user scope, no authority.
* **Surfaces.** The hook prints `[서명 확인]` on a verified row and `[⚠ 서명 불일치 — <id> 사칭 가능]` on a
  failed one; `origins` pages evidence rows too (`kinds=["evidence","verdict"]`, with producer / verified /
  key_id) and `vrs2-identity.py audit` counts them per producer: verified · unverified · registered-legacy ·
  proxy.

Live: nine producers of this machine registered (`asm-agent`, `session-main`, `pytest-runner`, `stop-hook`,
`gate`, `sonnet-subagent`, `origin-check`, `recall-bench`, `probe-runner`); the first signed row — the suite run
through `vrs2-run.py` — verified True with its key id; the 65 earlier rows of registered producers count as
legacy. Tests: `test_signed_producers.py` (3): keygen / sign / verify with tampering, an impostor without the
key and one with another producer's key, the evidence fold counting three producers and not the impostor,
the daemon stamping rows on ingest and the audit page reading them.

Not built: key rotation and revocation (`keygen --replace` re-registers, old rows stay verified by their
stored `key_id` only nominally — a revoked key is not re-checked), a registry carried inside the store (it is
a file beside it), signatures on imported file-backed records (origin binding covers those), and any per-user
recall or write policy — several users share one store as several producers, nothing more.

### Every turn is an experience — real-time transcript ingestion (2026-09-21)

The premise, in the user's words: every experience enters through the VRS as an experience, and what comes
back is not a log line but an experience that names its exact place. Until this step the store did not hold
the conversation: the Stop hook indexed what the main *wrote about* a turn (the session-log entry), memory
docs and verdicts; the transcript itself was read once, by PreCompact, for one snapshot entry. A question
after compaction could find only what the main had chosen to log. Continuity that is not real time is not
continuity — it is absence — so this closes it in the code, not in a test plan.

`harness/transcripts.py` (hook shim `transcript_tail.py`, tool `vrs2-tail.py`) tails a conversation log and
sends each **turn** — one user message and everything until the next — to the daemon as one row through
the same `ingest_many` every other producer uses:

* `kind = transcript`, text = the turn as said: the user's words, the assistant's text, one line per tool
  call (`도구: Read proj/chunk_7.csv · Bash …`), one event line per compaction boundary
  (`[압축 경계 auto · 전 967,352 → 후 11,761 토큰]`). Tool results are never copied: they are files and
  outputs the origin holds. Text is bounded (6,000 chars) — the store is for recall, the log for the rest.
* bound to its place (G3): `metadata.origin = {bytes, lines, sha256 of the raw span, span: true}`. Logs are
  append-only, so `origin.verify_span` checks the row against the log by reading the span alone (a 60 MB
  transcript is never scanned by the hook). The 「열기」 line is the exact call:
  `원문 위치: Read file_path="…jsonl" offset=45 limit=4  (대화 턴 23 의 로그 45-48행)`, plus
  `memory_read` for the stored text when the snippet is cut; a rotated log says `원본 자리 바뀜`.
* producer `transcript-tail` (registered, signed when the machine holds its key), `project` = the slug of
  the transcript's cwd (the hook's `choose` admits a row only for its own project), `agent`, `session`,
  `turn`, `part` = `whole` / `partial` (cut by PreCompact or by the read budget) / `tail` (what followed a cut,
  or a late assistant record after a Stop that saw an unfinished turn).
* the hook packet has a slot for one conversation turn beside the one record slot (`MAX_TRANSCRIPTS`), so a
  turn never displaces a memory doc and is never displaced by one; rendered as `N. 대화 — claude-code 세션
  5e9f04f8 턴 295 (2026-09-21)`.

**Real time** is every moment the host offers: Stop (turn complete), SubagentStop (the delegate's own
transcript — its sidechain records are its turns), PreCompact (the unfinished turn *before* the context is
lost — the cut is forced), SessionStart (what a crash or /clear left behind, plus a sweep of the project's
quiet sibling logs that were tailed before and have grown since). An agent without hooks:
`vrs2-tail.py --watch "<glob>"` polls its log directory; its open turn is taken only when the file has been
quiet for `QUIET_S` (8 s), so a turn is never cut mid-write. Formats: `claude-code`, `messages-jsonl`
(role/content per line, also under `message` / `payload` — OpenAI/Codex style), `messages-json` (a document
with a `messages` list — positions are message indices), `text` (role-prefixed lines); detected from the
first bytes or named with `--format`.

Budget and state: a run sends at most 40 rows (one generation) and parses at most 8 MB from its offset; a
never-tailed log drains over several runs (`backlog` in the receipt) — `--backfill "<glob>"` loops until
empty. State per log is its own file (`~/.claude/hooks/vrs2_tail/<sha12(path)>.json`: offset, line, turn),
so Stop and SubagentStop cannot lose each other's update; positions only move forward and request ids are
`transcript:<sha12(path)>:<l0>-<l1>`, so a replay after a lost update is idempotent on the daemon.
Receipts: `~/.claude/hooks/vrs2_tail.log`.

Measured (live store, 2026-09-21): this session's 63 MB transcript backfilled in 10 runs / 51 s → 297 rows
(turns 1–297, 4–25,278 lines); parse 150 ms per 8 MB window; `hook_recall` 136 ms at 6,110 records with a
transcript turn at rank 2 for a question about the morning's work. The first backfill exposed a defect:
`ensure_daemon`'s client allows 5 s, a 38-row batch took 10 s, timed out and replayed idempotently
(no duplicates, wasted time) — the tail now widens its client timeout with the batch size.

Tests (`test_transcripts.py`, 4): turns enter through the daemon and a `hook_recall` for a question brings
the turn back with its lines, span-verified and rendered as the exact `Read` call — never by reading the
log; PreCompact cuts the open turn and Stop brings its tail with contiguous spans; the read budget leaves a
backlog and a replay sends nothing; a Codex-style `messages-jsonl` log; the hook routing of Stop /
SubagentStop / SessionStart with the dead-session sweep. Not built: retention or cold bundles for transcript
rows (they go to the project's bundle like everything else; the sizing rule warns at 90 %), redaction of
secrets a user pasted into a turn (the text is stored as said — the log already holds it), and a service
wrapper for `--watch` (a foreground loop the user starts; system settings are the user's).

#### The premise as a gate, and what a turn carries (2026-09-21, later the same day)

The user's audit question — "is one thing missing, or thirty?" — was thirty-two (session log 12:4x). The
ones that broke the premise outright were fixed first:

* **A test that ignores the premise is discarded, not scored.** `local/bench/compaction/GRADING.md` §0: a run
  is scorable for continuity only with `vrs2_tail.log` receipts that reach every compaction boundary before
  it, a recall receipt after the boundary, and transcript rows of that session in the store; anything else
  is labelled 「VRS 없음」 and its ② is not computed. The 09-16/09-18 A/B runs (A2·B2) were such runs — a
  subagent restricted to Read/Write/Edit, no hooks, 0 compactions — and are marked so wherever their
  numbers were cited (§10, the purpose memory). Their only remaining use is as anchors for the ① scorer.
* **The absence is no longer written as a principle.** The MCP server's instructions, `VRS2_STANDALONE_VALIDATION`,
  `VRS2_VALIDATION`, `AGENT_MEMORY_VALIDATION` and the importer's docstring said "no automatic transcript
  capture"; each now says where capture happens (the hooks and `vrs2-tail.py`, not the server).
* **After a compaction the cut turns come from the store.** Daemon command `turns` (session → the last K
  transcript rows, optionally only those starting before a log line); the SessionStart hook on
  `source == compact` appends a 「압축 직전 대화 — 스토어의 마지막 3턴」 block under the session-log tail, each turn
  with its part (`[압축 전 미완 — 여기서 잘렸다]`) and its exact `Read` call. The log is not re-read; a daemon
  that is down is a named miss in the receipt, never a blocked start.
* **A turn carries more of what happened.** Tool errors (`[도구 오류: Bash · Exit code 1 …]`), the user's
  rejection of a tool call (`[사용자 거부: Bash]` — the strongest correction there is), a command's first line
  of output (`결과: Bash → …`), slash commands (`[명령 /compact]`), the host's own compaction summary
  (`[압축 요약(호스트)] …` head and tail, bounded) and the turn's token usage (`토큰: 문맥 최대 512,345 · 출력 1,200`;
  `metadata.tokens`, `errors`, `rejected`). Tool payloads themselves are still never copied.
* The first machine-wide backfill (`~/.claude/projects/**/*.jsonl`, 1.3 GB) exposed two defects: a 4 KB
  format probe truncated a long first record and misread workflow subagent transcripts as generic logs (now
  whole first lines are read, and `agentId`/`parentUuid` count); workflow `journal.jsonl` files are event
  logs, not conversations, and are skipped. 115 rows ingested before the fix carry the agent label `wf_…`
  instead of `claude-code`; they are real turns at real positions and were left as they are.

Measured (40 real prompts of this session, self-hits — the turn the prompt itself opened — excluded): a
transcript row is in the top 10 of every packet (first one at rank 1 in 18/40, ≤3 in 30/40); `choose` admits
one in 32/40 under the big-row bar and 35/40 with a lenient bar — the bar is not the problem, so it stays.
`hook_recall` ran at a median 508 ms while the backfill was ingesting (130–150 ms before it at 6.1k rows);
re-measured after the backfill in the session log.

#### Real time without a hook, and what must not multiply (2026-09-21, third batch)

* **The live daemon sweeps.** A `vrs2-tail-sweeper` thread (every 60 s, `--tail-sweep 0` turns it off) tails
  every log this machine has tailed and every file of a registered glob (`vrs2-tail.py --register "<glob>"
  --agent NAME [--project SLUG] [--format F]`; `--registered`, `--sweep` for one pass now) that grew since its
  state and has been quiet 10 minutes — a session that died mid-turn, an agent without hooks. It goes through
  the daemon's own port like a hook, opens a client only when a log has something to send (an idle daemon
  still idles out), and only the daemon that owns the configured live store sweeps — a test daemon on a
  temporary store never touches this machine's tail states. So "all agents, real time" no longer depends on
  a foreground `--watch` loop the user must keep running.
* **A failed ingest is said out loud.** When a Stop / PreCompact tail run cannot send its rows (daemon down,
  refused batch), the hook prints a `systemMessage` — `[기억] 대화 유입 실패 1건 — … (다음 정지에 다시 보냄)` — beside
  the receipt; the log position does not move, so the rows go on the next run.
* **Secrets are masked before storage** (`redact`): private-key blocks, Anthropic / OpenAI / AWS / GitHub /
  Slack / Google keys, JWTs and `password= / token= / api_key=` assignments become `[가림:<kind>]`;
  `metadata.redacted` counts them. The log keeps the original; the store must not multiply it. Hex digests
  are not masked.
* **More of the turn**: the user's mid-turn message (`[사용자 끼어듦 12:43: …]`, from the host's `queued_command`
  attachment — the message itself arrives later as a user record and opens its turn), a background task's
  status (`[배경 작업 completed: …]`), and the digest of each file the turn read (`읽음: proj/chunk_7.csv@3f2a9c1b7e5d`
  — the file as it is at the tail's run; the version at read time stays in the log's tool result).
* **One key per machine per producer** (`vrs2-identity.py keygen --producer P --add`): a second machine adds
  its public key under the producer's `keys` instead of replacing the first machine's; the daemon verifies
  against any of them and stamps the matching `key_id`. A borrowed key still fails.
* **The SubagentStop payload** is written down the first time it fires (`payload_keys` in the receipt): the
  field names `agent_transcript_path` / `agent_id` were assumed from memory; the receipt settles it.
* `vrs2-tail.py --show LOG FIRST LAST` prints the turn(s) at those lines as the row was made — the human way to
  open a 「원문 위치」 without reading raw jsonl. The delegate packet (`vrs2-delegate.py`) already renders
  transcript rows through `choose`/`render`, conversation slot and 「원문 위치」 included; `memory_context`
  returns the replayed episode with its metadata (position inside), but its snapshot precondition fails
  whenever a write lands between `memory_status` and the call — under continuous ingestion (the backfill,
  the sweeper) that is often; the hook path has no such precondition. Left as a named gap.

#### After the backfill (2026-09-21 13:2x)

The machine-wide backfill ended: 1,145 runs, **5,313 turns** from 13 projects (T2M 2,278 · SQLITE 1,587 · mcp
481 · …), 0 errors, parts 5,285 whole / 19 tail / 9 partial; with this session's 297 the store holds ~5,600
conversation turns beside its 5.7k records — 11,441 rows. Restore after restart 2.9 s (checkpoint seq 11,994).
`hook_recall` at 11.4k rows: 118–488 ms over five prompts (judgment 241 ms), against 130–150 ms at 6.1k — the
growth cost, on the measured curve, and the reason the transcript rows will want a bundle of their own before
the year is out (the sizing rule warns at 90 %).

Two lessons from the restart, both defects of procedure rather than code: (1) `shutdown` answers in 5 ms but
the old process goes on checkpointing; a new daemon spawned two seconds later finds the state owned, exits,
and `ensure_daemon` waits its whole timeout on a start marker nobody clears — wait for the old process to be
gone (no port file, no process) before starting the next; (2) **the MCP bridge did not survive the restart**:
it kept one client for the whole session, so every `memory_status` / `memory_context` of the session failed
with `tool_request_failed` afterwards. `server.ReconnectingClient` now rebuilds the client through
`ensure_daemon` on `resident_request_failed` / `resident_daemon_starting` and retries once; a refusal (a
contract error) is not retried. The bridge's ingress text no longer says "no automatic conversation capture".

Still open from the thirty-two: thinking blocks (a decision, not a patch — they are the model's reasoning;
their length could be counted, their text should not be stored by default), batch / scheduler results and
the user's own actions outside any agent (the company programs have to call `vrs2-run.py` / `produce()`
themselves), the project wall in `choose`, the snapshot ↔ partial-row link, the host's 30-day transcript
cleanup (a `cleanupPeriodDays` the user sets), hook contention under load (the 6 s batch was measured only
while the backfill ran), transcript bundles, the backfill-vs-live state race (idempotent, cosmetic), Linux,
and `memory_context`'s snapshot precondition under continuous ingestion (a bridge-side retry with a fresh
snapshot is the likely answer; not built).

#### The four stages, measured on the live session (2026-09-21 13:3x → 14:0x)

Asked whether the four stages of memory actually work, measured rather than described, on this session at
11.4k rows: **Déjà vu** — region paths are present in every judgment (`paths: local/member/portal…` in each
receipt). **Recall** — 9 of 12 prompts of the day injected something, median 506 ms. **Replay** — 25 of 328
injected records were opened, 7.6 %; and the matcher only knew `Read`, while in bypass mode the main reads
with `sed -n` / `cat` / `head` through Bash, so shell reads of an offered path were counted as *not opened*.
**Re-evidence** — 45 hypotheses, 78 observations, 0 accepted, 0 rejected, 45 abstain (one producer, as the
accumulator's rule says), 24 usage records.

What changed for Replay: (1) `read_log` now handles `Bash` / `PowerShell` (matcher widened by the installer
and live): a `sed -n A,Bp` of an offered path (a memory doc, a transcript's 「원문 위치」, anything a 「열기」
line named — `offered_paths` from the session's recent receipts) is a `use: read` line with offset/limit,
`head -N` gives 1..N, a whole-file read has no offset, a write (`>`, `>>`, `tee`, `Set-Content`) is not a
read, and simple `VAR=value` assignments are substituted so `M=…; sed -n 1,60p "$M/x.md"` is seen (the shape
the main actually uses; the first cut missed it). (2) `opens` entries carry `whole` — a record whose snippet
is its whole text needs no opening and is not counted as a miss (`usage.last_turn`). (3) The next prompt's
packet begins with one line when the last turn left injected records unopened:
`(지난 턴: 주입 n 중 연 것 m — 안 연 것 …. 열지 않은 기록은 쓰지 않는다.)` — silent when nothing was
injected or everything was opened, never blocks a prompt (`recall.replay_line`). Tests:
`tests/standalone/test_replay.py` (parser shapes, the hook on a Bash read, `last_turn` with synthetic
receipts, the line). Not done here: making the company batches call `produce()` — the user's programs.

#### The remaining items, in order (2026-09-21 14:0x → 14:3x)

「순서대로 고치자」 — the items left after the four batches, taken by number:

* **4 thinking** — a turn keeps a bounded excerpt of the model's reasoning: `사고: …` after the assistant line
  (`THINKING_CHARS` 600 of the joined thinking blocks; half the blocks in a live log carry only a signature and
  are skipped), `metadata.thinking` = the full length. The why of a decision was the one thing the turn text
  lacked; the whole reasoning is not copied.
* **10 · 11 batch results and work outside any agent** — nothing to build on the VRS side: `vrs2-run.py
  --hypothesis "the settlement daily audit completes" --context SETTLEMENT --producer settlement-daily-audit
  -- <the batch>` records a batch run as a machine result (ledger first, `--flush` at the next Stop when the
  daemon was down), and `vrs2-produce.py` / a session-log line take what a person did by hand. The wiring is the
  user's: the scheduler task `SETTLEMENT-DailyAudit` is deliberately not registered, and the company programs
  are not in this repository.
* **14 project wall** — `choose` kept every other project's rows out. A prompt that *names* a project (a slug
  word of three letters or more: T2M, SQLITE, COGN, mcp, gemini — a Korean-named project has no such word)
  now lets that project's rows through, after this project's own, with a conversation-turn slot of its own
  (`MAX_TRANSCRIPTS` each), rendered `· 프로젝트 t2m`. Naming T2M no longer costs this project its turn.
* **16 snapshot ↔ cut row** — the PreCompact snapshot entry ends with `로그 위치: <log>#l0-l1 (턴 k)` (the
  partial row's key, from `transcripts.plan` on the tail's state, or from the tail's own cut marker when the
  tail ran first — the host runs an event's hooks in parallel), and the cut row carries
  `metadata.snapshot = {log, line, stamp}` plus a `[압축 스냅샷: session-log.md n행]` line (marker
  `vrs2_tail/<sha12>.snapshot.json`, waited for up to 4 s, consumed). `turns` rows and the post-compaction
  block show it (`· 압축 스냅샷 session-log.md n행`).
* **18 host cleanup** — `cleanupPeriodDays` was 30 (the default): a turn's 「원문 위치」 would go missing after
  a month (the row's text stays; the hint already says 「원본 로그 없음」). Set to 365 in the user's
  `~/.claude/settings.json` (copy kept). The logs of the last month are 1.4 GB (T2M 704 MB), so a year is
  ~17 GB — the user's number to raise or lower.
* **22 contention** — re-measured after the backfill: Stop-time `vrs2_tail` median 2.9 s (max 5.6 s),
  `stop_reindex_v2` median 5.6 s (max 8.2 s) — and the reindex receipts showed *why*: three session-log rows
  refused at every Stop with `request_id_reused_with_different_content`, which left the file unstamped and
  re-parsed (349 rows) each time. The id is `kind:source@revision`, the text carries the tick's code ledger,
  and a hook killed after the daemon accepted (the 30 s limit under the backfill) never wrote the manifest —
  so the same id came back with other content forever. `reindex.reissue` gives that content an id and revision
  of its own (`+sha8(text)`) and supersedes the episode the old id stands for (new daemon command
  `operation`: request id → episode); an older daemon without it still takes the row, without the link.
  Found by reading the receipts, not by a failing test.
* **24 turn bundle** — `bundle_of: {"kind:transcript": "<id>"}` routes every conversation turn to a bundle of
  its own (before the project's `bundle_of`); the cue to create it is the sizing warning at 90 % of the limit
  (the store is at 19 % now). The split itself is a config line and a daemon restart, not a code change.
* **27 state race** — one run per log at a time: `acquire_lock` (an exclusive lock file beside the state; a
  lock older than 120 s belongs to a dead run and is taken over); a second runner — the hook, the daemon's
  sweeper, a backfill — steps aside with receipt `skip: locked` and the next trigger takes the same lines.
* **31 · 32 the premise, checkable** — `vrs2-tail.py --status` begins with `premise: wired — Stop:ok
  SubagentStop:ok PreCompact:ok SessionStart:ok use_log:ok last_stop:<ts>` read from the host's settings file
  (`transcripts.premise`), and `test_item31` pins the installer's SHIMS to the four events and the widened
  matcher. On Linux: `vrs2-install.py`, `vrs2-identity.py keygen --producer transcript-tail --add`, then
  `vrs2-tail.py --status` must say `wired` and, after one Stop, show a `last_stop` — not verified from here.
* **33 stale snapshot** — the bridge (`LoopbackMCP.call_tool`) catches `snapshot_mismatch` on a
  `memory_context` / `memory_recall` start, pins the current `pair_snapshot_id` once and says so
  (`snapshot_refreshed = {expected, used, reason}`); nothing to refresh → silent; no handle exists at that
  point, so the request id stays usable. Measured before: 2 of 2 calls failed with 8 rows entering between
  `memory_status` and the call.

Tests: `tests/standalone/test_audit_items.py` (8); standalone suite 92 passed. The daemon needs a restart for
`operation` and the `turns` snapshot field; hooks and the bridge pick the new code up on their next start.

#### A third-party agent, live: Antigravity (2026-09-21 14:3x → 14:5x)

The premise check had one honest hole: nothing but Claude Code was flowing. The user installed Google
Antigravity to test the third-party path. Its conversations are **SQLite databases**, one per conversation
(`~/.gemini/antigravity/conversations/<cascade>.db`; two older `.pb` dumps are not read), whose `steps`
rows carry protobuf payloads: the user's words in step type 14 (field 19.2), the model's answer and reasoning
in type 15 (20.8 / 20.1, 20.3), tool calls in the other types (5.4.2 name, 5.4.3 JSON args), a created
timestamp at 5.1.1, the workspace in `trajectory_metadata_blob`. `harness/transcripts.py` decodes the
protobuf wire format without a schema (`pb_decode`, `pb_get`) and turns each step into the role/content
message the generic adapter already reads (now with `thinking` / `reasoning`); the format is `antigravity`,
positions are step indices (`INDEXED`, like messages-json), the span digest is over the steps' canonical JSON
and `origin.verify_span` re-reads them from the database (`origin.format`). The database is read in place,
read-only; a locked moment is retried. The hint opens a turn with `vrs2-tail.py --show <db> FIRST LAST` (a
`Read` cannot), and `read_log` counts that as an open with the step range.

Real time without a hook, for real: `vrs2-tail.py --register "<glob>" --agent antigravity --format
antigravity --idle 15` — a registration carries its own idle, and the daemon's sweep period follows the
shortest registered idle (15 s, never below 5, never above 60). Live test: the user's message at 14:46:37, the
answer at 14:46:41, the row in the store at 14:46:50 with `verify_span intact` and the reasoning — **9 s**
after the answer (with a foreground `--watch --interval 5`; the sweep alone is bounded by idle + period).
Two things the live test exposed: a WAL-mode database takes its writes in `<db>-wal` while the `.db` mtime
stays at the last checkpoint (the answer sat in the WAL with the .db 4 s older) — `last_write` now takes the
newer of the two for `quiet` and the sweep's idle; and the sweep at a fixed 60 s would have missed the
30 s line. Backfill of the nine existing conversations: 102 turns, 0 errors; recall put four Antigravity
turns in the top 10 for a question about the staging database, verified and signed.

#### Driving Antigravity from the terminal, and what the continuity test showed (2026-09-21 15:0x → 15:3x)

The language server has an `agentapi` (`language_server.exe agentapi new-conversation | send-message |
get-conversation-metadata`) that takes the address, CSRF token and project id from the environment
(`ANTIGRAVITY_LS_ADDRESS`, `ANTIGRAVITY_CSRF_TOKEN` — read from the running process, never printed —
`ANTIGRAVITY_PROJECT_ID`); projects are plain JSON in `~/.gemini/config/projects/<id>.json` (name, folder, policies).
`local/tools/agy.py` wraps it, `ag_run.py` sends a prompt and waits for the database to go quiet. The sealed card
(`local/bench/continuity/antigravity-card-1.md`) ran through it: conversation A (goal + four constraints, two
decisions, stop before running), then two new conversations asking the same four questions, B1 without VRS and
B2 told to use the swegca-vrs2 MCP.

Three things the run exposed (none found by suspicion):

- `send-message` arrives as step type **101** — "Message from System", text at field 114.2.10.1, sender at
  114.4.3 — not the type-14 user step the chat box writes. The adapter dropped it, so the second and third turns
  entered as wordless answers. Type 101 now opens a turn (`[메시지·system] …`); its `task_notification` sub-kind
  (114.3), a background command's result, stays a tool result.
- After that fix the state was reset and every re-cut turn met `request_id_reused_with_different_content`; the
  tail stayed at line 1 (the reindex's old trap). `reissue_row` gives the re-cut row an id and revision of its own
  (`+sha8(text)`), re-signs it and supersedes the row the old id stands for; the receipt says `reissued`.
- B1 was not "no memory": Antigravity writes `brain/<cid>/.system_generated/logs/transcript.jsonl` (one line
  per step, plain `content`) and the agent — reciting constraint ④ in its answer — went outside the workspace
  seven times to find and read the previous conversation's file. B2's condition was off too: `mcp_config.json`
  is not hot-loaded (the tools were not registered in the session), so the agent found the config, spawned
  the server itself through a client script, got conversation A's turns on the first page for three of four
  queries, and still parsed `transcript.jsonl` with seven scripts (≈25 reads outside the workspace). Both scored
  10/10 on the card; the scope column is where they differ, and the VRS-with-registered-tools condition (B2′)
  is still to be measured. `transcript.jsonl` is a candidate simpler source for the tail (JSONL, line-addressable,
  opened by Read) — not switched.

Reruns after the user restarted Antigravity (15:39 → 15:46): the config file Antigravity reads is
`~/.gemini/config/mcp_config.json`, not `~/.gemini/antigravity/mcp_config.json` (a string in the language server
binary; a 0-byte file had sat there since May). With the config in place Antigravity spawned the server for a
second at conversation start and wrote `~/.gemini/antigravity/mcp/swegca-vrs2/{instructions.md, <tool>.json ×8}`
— discovery works, `memory_store` absent as intended — yet the model read those schema files and still built
its own in-process client instead of calling `call_mcp_tool`, then followed the store rows' origin paths to the
raw `.db` and `transcript.jsonl` (≈25 reads outside the workspace; 10/10, quoting the A3 instruction). The first
rerun was void: the store rows named the card file and the agent found and read it — the card is moved away
during a run from now on. Verdict for card 1: goal and context survive the new-conversation boundary at 10/10
with or without VRS on the same machine, because Antigravity's own per-conversation log is on disk and the
agent finds it; what VRS changes is where the agent says it looked and that it goes straight to the origin.
A test that separates the conditions needs another machine, another agent, or deleted logs.


### The session producer layer — 2.2 (2026-09-21)

Composed after the SWEGCA architecture (`docs/VRS22_PLAN.md` has the role table): main stays the one
persistent Cognitive State; a session is an authority-limited producer; its temporary VRS is the producer's
**proposal journal** — `<state>/sessions/<session id>/`, one small store of its own (journal, hot index,
VRS graph), written to by that session alone, never read by main, merged into main after the session ends
by a journaled transaction with a receipt, then removed.

- **Writes.** `ingest` / `ingest_many` with `session=<id>` go to the journal (`layer: session` in the reply).
  The transcript tail passes the session for a live log (written within `SESSION_IDLE_S` = 30 min); an older
  log (a backlog, a dead session) goes to main as before. The Stop reindex passes the hook's `session_id`.
  A journal accepts a `supersedes` naming a record it cannot see (a doc section main holds); main validates
  it at the merge.
- **Reads.** `hook_recall` with `session=<id>` judges the journal first — Déjà vu → Recall → Replay →
  Re-evidence over it, with the **bounded exact top-K**: the candidate stage runs in columns (postings → a
  row × cue matrix; the substring dedupe of `words` as bit sets over the query's informative cues; the BM25
  pre-score as an array), rows are taken in pre-score order and get their full order key until no remaining
  row's pre-score × gate ceiling can beat the K-th best full score (the ceiling is per row: promotion only
  after a consolidation exists, asks/description factors only where the row has them), Replay and
  Re-evidence run for the top K, the rest stay addressable as `unjudged_ids`. Main is read only on a
  **complete miss** — no cue of the query has a posting in the journal — and the packet says so
  (`layer`, `misses: session_miss/absent|complete_miss`). `turns` and `origins` with a session read the
  journal too (after a compaction the SessionStart hook's turns come from there); `operation` answers for
  the journal's ids (the reindex's manifest recovery).
- **Merge.** `session_end` (the SessionEnd hook's shim, `session_end.py`) or the preparer's sweep (a
  journal idle > 30 min, checked every 15 s) submits the session to the merger thread. `merge.py`:
  `prepared` (journal read, `delta_digest` over the rows' fingerprints fixed) → `memory_committed` (one
  `ingest_many`: the whole session is one batch generation in main) → `state_committed` (every row verified
  present under its id) → `completed` (directory removed). The receipt binds `before_pair` / `after_pair`
  (the state hashes), the digest, `evidence_refs`, the producer, `reissued` (a request id another producer
  committed with other content: this row enters under its own id, superseding the held record when it is a
  later revision of the same source), `unlinked_supersedes`, `conflicts` (propositions now carrying both
  polarities — the accumulator's abstention, listed, never averaged). Daemon start recovers incomplete
  transactions (`prepared` re-runs, `memory_committed` verifies and completes, a vanished journal rolls back).
- **Residency.** Open journals are hot (`SESSION_HOT` = 4, LRU; beyond that checkpointed and closed on
  disk, unmerged). The merge takes the daemon lock only around main's `ingest_many`. Off with
  `VRS2_SESSION_LAYER=0` (main-only, as 2.1).

Measured on this PC (i5-13400), a session-sized journal of real session-log rows, a hit (main is not read,
so main's size does not enter): the four stages 0.37 / 0.55 / 0.82 ms p50 at 10 / 100 / 1,000 rows (385
candidates at 1,000), the whole `hook_recall` packet 0.57 / 0.89 / 1.25 ms; one socket round trip adds ~1 ms
on a kept connection and 2–15 ms when a hook connects per call (Windows TCP). Before the bounded stage the
same packet took 2.3 / 19.5 ms at 100 / 1,000 rows. `tests/standalone/test_session_layer.py` (11): isolation,
session-first and the miss, the merge receipt as one generation, re-issue and unlinking, conflicts, recovery,
idle sweep, an unseen supersedes, bounded top-K exactness against the full path, the tail end to end.

The rule's cost, stated: another active session of the same project does not see this session's new
log entries or turns until this session ends and merges (before 2.2 every Stop's rows were in main at
once). The evaluation (plan phase 6) is where that shows or does not.
