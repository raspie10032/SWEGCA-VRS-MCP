# Main metadata residency audit — 2026-09-27

This diagnostic does not change SWEGCA decisions, source history, merge rules,
or production cache policy. It identifies which existing structures cannot yet
release memory. It is not a 4GB/500GB acceptance test.

## Reproduction

```sh
taskset -c 6,7 make -j2 build/recall-scale-bench build/runtime-tests
ulimit -c 0
taskset -c 7 build/runtime-tests
taskset -c 6,7 build/recall-scale-bench 2048 repeated 2
taskset -c 6,7 build/recall-scale-bench 2048 distinct 2
```

The benchmark uses 128-byte synthetic inputs, a 256MiB tracked allocation
budget and two merge workers. Its cache target is deliberately one byte to
attempt one complete maintenance traversal; that is not a production setting
or an achievable target. Each route has five warmups and 30 measured samples.
The timing starts at `Runtime::input`, not at desktop input delivery. No LLM,
natural-language semantics or compaction evaluation is involved.

The former distinct-input assertion incorrectly expected no Recall candidates.
The current session-context path legitimately recalls earlier dialogue when
there is no exact cue. The benchmark now checks both cardinality and key kind:
first input missing; subsequent repeated inputs exact; distinct inputs context.
The existing `*-continuation` route labels cover follow-up input after Replay;
with multiple linked connections the selected route is shared context.

## Results

| 2,048 originals | Repeated cue, one connection | Distinct cues/connections |
|---|---:|---:|
| Tracked bytes before maintenance | 734,064 | 2,881,280 |
| Tracked bytes after maintenance | 102,544 | 2,881,280 |
| Cue keys / cue ranges | 1 / 1 | 2,048 / 2,048 |
| Context keys / context ranges | 1 / 1 | 1 / 2,048 |
| Tracked bytes after reopen | 734,064 | 2,881,280 |
| Largest measured input-to-Recall entry | 170ns | 1,930ns |

Raw output: `benchmarks/results/main-residency-{repeated,distinct}-2048-20260927.jsonl`.
All 420 measured route samples were below the latest 5ms entry limit. Each
input/receipt construction checked no `pread`/`pwrite` and no surviving temporary
allocation. This does not measure Replay completion or prove desktop latency
under large-scale/concurrent load. Runtime regressions passed 2,141 checks.

## Confirmed remaining structure

* `MainGraph` cue/context maps and their range nodes remain resident. The new
  serialized diagnostic counts these without allocation, payload reads or
  evidence evaluation; the benchmark verifies its no-allocation/no-I/O contract
  and expected topology. It is outside the input/Recall path.
* `ExperienceSequence::page_candidate` uses the core metadata placement rules.
  A singleton is retained because its individual backing structure would cost
  more than the released metadata. The distinct fixture therefore saves nothing
  even after traversing the full graph. Removing this guard would waste RAM.
* Reopen reconstructs resident metadata; the repeated fixture's prior cache
  reduction is not retained across reopen.
* Distinct and repeated fixtures differ in connection state as well as indexes.
  Their total memory difference cannot be attributed solely to portal nodes.

The next residency change must group small connections' metadata into shared
pages and connect portal paging to Main, retaining original addresses, core
selection rules, source boundaries and the no-allocation publication contract.
Simply dropping context ranges or removing the small-page guard is not a fix.
General natural-input binding and agent-level purpose verification remain open
as specified in `NATURAL_INPUT_PLAN.md`.
