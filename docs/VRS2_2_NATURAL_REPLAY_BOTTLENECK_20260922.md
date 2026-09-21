# VRS 2.2 natural Recall through Replay: measured bottleneck

This measures the product's `ShardedMain.recall` on a **copy** of 15,630
existing native experiences. It reaches the complete VRS region/navigation
path and original Replay, then runs Re-evidence. The active resident state was
not read or changed, and no model evaluation was started.

`tools/benchmark_vrs22_natural_replay.py` wraps the imported `ReplayResult`
constructor only to timestamp the point at which Replay is complete; it
returns the same product object. The tool separately times the full four-stage
return. Three deterministic lexical cues are selected by posting-list size;
their SHA-256 values and actual fanouts are in the raw results, while the
private cue text is omitted. Each result checks that all candidates have
original Replay episodes and Re-evidence.

Both runs used a systemd cgroup with `memory.max=4,294,967,296`, swap disabled,
and read/write limits of 625,000,000 B/s on SSD `259:3`. The pre-existing
experience copy had complete exact and read-projection indexes. No SQLite
module was loaded.

| Matches | Installed `f1ea47a` median warm Replay | Source change median warm Replay | Source change first Replay | Source change 4-stage median |
| ---: | ---: | ---: | ---: | ---: |
| 1 | 0.544 ms | 0.196 ms | 9.112 ms | 0.253 ms |
| 100 | 46.694 ms | 15.152 ms | 31.894 ms | 26.014 ms |
| 1,008 | 263.584 ms | 93.248 ms | 207.173 ms | 136.672 ms |

The original source expanded per-cue graph strengths for **every** natural
candidate even when there was only one shard. That array is used to construct
cross-shard portal strength, not for local ranking, Replay or Re-evidence. The
source change defers only that expansion on natural Recall. When candidates
span shards, it reads the complete current VRS facts before building the same
portal connections. The default `current_vrs` API still returns full cue
strengths. Tests compare full and deferred current facts for hot main and cold
projections, and the cross-shard portal/conflict suite passes.

The source change is an optimization, **not** satisfaction of the hard bound.
First calls and queries matching 100 or more records exceed 1 ms through
Replay. The existing API materializes every matched original Replay before
returning; its work and output therefore grow with matched-candidate count.
The next repair must retain full access to originals, graph, regions, shared
experience, portals and Re-evidence while addressing that unbounded read work.
The user-defined one-billion VRS-parameter unit remains unresolved and is not
identified with records, cues or bytes here.

Raw cgroup receipts:

- `evals/vrs22_context/results/natural_replay_15630_20260922.json` — SHA-256
  `6d5491be37b2e531eec544c61ab4db8b767fd9dc7041c20f343866dfa2f22a71`
- `evals/vrs22_context/results/natural_replay_source_15630_20260922.json` — SHA-256
  `c1678fe3eb7c1a59eff3d94d620926266a846957bcd1f2a9acb0eeda0309db9c`

The benchmark tool's SHA-256 is
`834071aed9a61c5e6337dd70adb0f708c13de9e7c6386c9ce8daa4afcb85014d`.

## First-query cue-directory page fault

A separate six-process check measured the *first* single-match natural query
after opening the same native copy. Each process requested
`POSIX_FADV_DONTNEED` for the 83,890,176-byte first-level cue table before
the read. Three processes queried immediately; three read that table into the
page cache first. All six ran under the same 4 GiB and 625 MB/s cgroup limits.

| Process | First Replay without table read | Cue slot probe | First Replay after table read | Cue slot probe |
| ---: | ---: | ---: | ---: | ---: |
| 1 | 8.707 ms | 5.959 ms | 1.720 ms | 0.0085 ms |
| 2 | 3.231 ms | 1.274 ms | 1.800 ms | 0.0077 ms |
| 3 | 3.396 ms | 1.567 ms | 1.758 ms | 0.0081 ms |

The selected cue required one hash-table slot probe. Cold processes recorded
1–3 major page faults during the query; table-read processes recorded zero.
This identifies page faults in the cue directory as a first-query bottleneck.
Reading the table took 0.021–0.048 seconds outside the timed query, but even
then first Replay remained above 1 ms. Table warming alone is insufficient and
its page-cache residency is not a hard guarantee under a 4 GiB cap. The
previously measured 100- and 1,008-match costs remain a separate unbounded
candidate-materialization problem.

The reproducible probe is `tools/profile_vrs22_natural_first.py` (SHA-256
`b6bd3d083ac9bcf909552444bb716d65ecedc7d1a238bf23d74eb4ae64cdc3d4`).
Raw files are `evals/vrs22_context/results/natural_first_{cold,warm}{1,2,3}_20260922.json`.
Their SHA-256 values, in cold 1–3 then warm 1–3 order, are
`d6dc2f99501143055a9956b97bd2d5c92488e344133b921dd4c8f0d4972d85a7`,
`d446fa3ecf5c6cdb38bb8a0626686060558480d8c8fd2a5cbccef9999924f584`,
`f542d17e56914843614e0e991466ef9a26d21dcc67f79c79cd981b6818762b70`,
`1ac08b8fbb95a8b8a3dacf599adf0a09fbc19edebc41bb8d327fb8d0c64fac60`,
`8d58d0c4ab75c12b1245cccf190af511346e932334752e81dbf6294ecdc7607f`,
and `04654700bcc765eeeb61ffad9cf678d9e609fdab2c1940da65deefc22ed117a1`.

The first full-suite attempt had 125 passes and two test-environment failures:
the separate offline test venv lacked `pytest-asyncio`. After installing that
plugin offline, the same source candidate passed the complete standalone suite
**127/127 in 182.57 seconds**. The 21 targeted resident/sharded tests also
passed separately.

## First-query preparation and remaining fanout cost

A phase probe on the same copied 15,630-record native main found that the
first single-match natural call spent about 0.55 ms in lexical-key extraction
and 0.26 ms enumerating 1,843 VRS portals even when the experience had no
incident portal pair. Fixed lexical patterns are now compiled at module load;
`projected_portals` returns an empty selection immediately. Full portal
projection, region membership, provenance keys and cross-shard paths remain
unchanged when portal pairs are selected. Seven representative address,
mixed-script and ordinary cue inputs retained the same key tuples; the full
native suite passed **127/127**.

Both receipts below use the same first single-match cue, 4 GiB memory limit,
zero swap and 625 MB/s SSD read/write limit. They run the source code on an
experience copy, not the installed resident runtime:

| First call | Replay reached | Full four-stage return | Cue probe | Major faults |
| --- | ---: | ---: | ---: | ---: |
| Table dropped from page cache | 2.500 ms | 2.621 ms | 1.599 ms | 1 |
| 83,890,176-byte table read before timing | 0.904 ms | 1.027 ms | 0.010 ms | 0 |

The source receipts are
`evals/vrs22_context/results/natural_first_source_cold_20260922.json`
(SHA-256 `bc834eb4daa8e365faf48f11990b5c38508ddd8c9331ddd0de2d057da1f478bc`)
and `evals/vrs22_context/results/natural_first_source_warm_20260922.json`
(SHA-256 `f359f733e189a0d006b8f6c393c7c6725d969abb062b636b507ad74627b20ccb`).
Table reading itself took 33.3 ms outside the second timed query. This is a
conditional sub-1 ms Replay observation, not an all-size or cold-query bound.
The cold page fault still violates 1 ms. A separate 100-match first query
after the same table read still took about 20.9 ms through Replay; exact
capsule loading, current VRS facts and portal provenance materialization
remain proportional to matched original experiences. No model evaluation is
authorized by this result, and the product-performance gate stays closed.

The same source was built into an isolated wheel, SHA-256
`4b06afd976f3b983f37ba905fdb4b99eede691940733afa142952b0749992fe7`.
All 48 installed package files match the source and wheel. Its cgroup-limited
natural-query receipt is
`evals/vrs22_context/results/natural_replay_first_query_wheel_20260922.json`
(SHA-256 `7e3bd28812082689de912181bcdf84334b77f7121b58a9ff0a203568cb16f130`).
On the copied existing main, 100 single-match calls had no Replay at or above
1 ms; all 10 calls with 100 matches and all 3 calls with 1,008 matches still
exceeded 1 ms. This wheel result is now the evaluation runner's explicit
performance-failure receipt. The cold single-match result above remains a
separate failure even though this repeated-run receipt began with cached
table pages.

## Clarified latency target: first original experience

The user clarified that the 1 ms target ends when the **first original
experience** reaches Replay, rather than after every matched original has
been replayed. The ranking and source identity still belong to main; the
measured first experience is therefore the first **ranked** Recall candidate.
`tools/benchmark_vrs22_natural_replay.py` now timestamps construction of its
first `ReplayedEpisode` and verifies that its ID equals the first ranked
Recall candidate. The earlier whole-Replay numbers remain in the same receipt
as a separate diagnostic. The benchmark wrapper leaves product return values
unchanged.

On the same copied 15,630-experience native main and installed wheel, under
the 4 GiB, zero-swap and 625 MB/s SSD cgroup, three calls per fanout gave:

| Matched originals | First ranked original, first call | First ranked original, warm median | Calls at or above 1 ms |
| ---: | ---: | ---: | ---: |
| 1 | 0.794 ms | 0.310 ms | 0/3 |
| 100 | 19.861 ms | 15.483 ms | 3/3 |
| 1,008 | 120.954 ms | 92.945 ms | 3/3 |

The raw receipt is
`evals/vrs22_context/results/first_ranked_original_replay_wheel_20260922.json`
(SHA-256 `8a683577d333d43e40bd599df517c7394407d37f99683f3f642908beeb03fee2`).
This is a milestone inside `ShardedMain.recall`, not the time at which the
caller receives a usable row: current main still computes all candidates and
Re-evidence before returning. The first ranked candidate itself is selected
only after full candidate scoring and VRS navigation, which explains why
the clarified target still fails for wider matches. Cold page faults also
remain a measured failure. The model-evaluation gate stays closed.
