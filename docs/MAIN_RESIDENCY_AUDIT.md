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

## Baseline structure at f34fb08

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

## Follow-up: shared singleton pages

Implemented physical page sharing for up to 256 eligible singleton Main
connections per bounded maintenance scan. The core `metadata_release` and
`metadata_page_beneficial` rules still decide whether placement can proceed.
One authenticated `ExperiencePage` is shared, while each connection retains its
own slice, original address and core Replay reduction. No connection, original,
outcome, source, context, strength or graph generation is combined or discarded.
The page body checksum covers all encoded entries; a selected read decodes only
its own entry, and a resident restore allocates only its own slice.

Writing runs in the existing background maintenance worker. Main validates all
prepared entries before publication, then attaches them without allocation or
I/O. A snapshot/pin acquired during preparation prevents that chunk's release.
Failed writes leave resident data intact. Last-owner cleanup retains the existing
inode, hard-link and storage-accounting rules. This is metadata placement after
an already authorized merge; it does not merge active sessions into Main.

Cold metadata now uses the core range-receipt decision instead of speculative
individual pin reads during candidate listing. The resident cost comparison,
candidate membership/order and numeric SWEGCA formulas remain unchanged.

Measured with the same commands and 2,048-input fixtures:

| Tracked bytes after maintenance | Baseline | Shared-page implementation |
|---|---:|---:|
| Distinct singleton connections | 2,881,280 | 2,670,912 |
| Repeated cue, one connection | 102,544 | 103,216 |

Distinct connections save 210,368 tracked bytes, approximately 7.3%. Shared
ownership adds 672 bytes to the repeated fixture. These are requested PMR bytes,
not a claim of lower total process RSS. Each of the 420 measured input/receipt
samples remained below 5ms and made no `pread`/`pwrite`; the maximum entry latency
in this run was 1,780ns. This remains a small in-process fixture, not desktop
delivery or large-dataset acceptance.

Validation:

* Experience-page tests: 6,416 checks, including independent slice restoration,
  all recorded outcome variants, write failure, late pin protection, no-I/O
  reduced selection, bounded resident restoration and last snapshot cleanup.
* Runtime tests: 2,481 checks, including shared-page maintenance, exact/context
  Recall without I/O, selected originals, later real merge and reopen.
* Stdio subprocess tests: 7,443 checks.
* Existing core microbenchmark: four-axis judgment median 43.9414ns and
  four-axis connection verification median 50.088ns. This checks fixed-size
  numerical operations only; no before/after speedup is claimed. The run shared
  CPU7 briefly with the stdio regression process.

Raw results are `benchmarks/results/shared-singleton-pages-{repeated,distinct}-2048-20260927.jsonl`
and `benchmarks/results/core-shared-singleton-pages-20260927.txt`.

The verified VRS executable was atomically installed in the existing desktop
prefix. SHA-256: `50da6c140ea66043e1941997169ac5f935171a4fbdd44142828a160756d5d2bb`.
Other executable hashes and persistent configuration were preserved. Installed
backend inventory verified the observer/query tools, shared owned socket,
aggregate memory limit 3,999,997,952 bytes, swap 0, CPU6–7 and shared I/O owner.
It made zero model calls and cleaned up its processes/socket. This is installation
verification, not proof that the currently open Codex desktop uses VRS: no
installed VRS processes were active before this check, and the desktop was not
restarted. The opt-in `Codex · SWEGCA VRS` launcher is still required.

Remaining: resident portal maps and connection descriptors, grouping of other
small prefixes, initial scratch headroom under severe pressure, and residency
after reopen. Singleton sharing is one completed component, not the complete
4GB/500GB scaling solution or completion of natural-language purpose binding.
