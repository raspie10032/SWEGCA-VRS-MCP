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
