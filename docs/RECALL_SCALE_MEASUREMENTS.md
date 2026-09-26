# Native accumulated-experience measurements

The `recall-scale-bench` target exercises Runtime receive, real original storage,
shuffle/core evaluation, explicit session end, Main merge, selected Replay and
Runtime reopening. It does not run models or install a live client hook.
Implementation baseline before the benchmark: `73d2dcb`.

Run separately, preserving other workstation services:

```sh
make -j2 build/recall-scale-bench
taskset -c 6 build/recall-scale-bench 2048 repeated
taskset -c 6 build/recall-scale-bench 8192 repeated
taskset -c 6 build/recall-scale-bench 2048 distinct
taskset -c 6 build/recall-scale-bench 8192 distinct
```

Native GCC 16.2.1, -O3, Ryzen 7 9800X3D. Measurement process on CPU 6;
compilation restricted to CPUs 6,7 and at most two jobs. No Palworld stop,
cache-drop operation, frequency locking or machine-wide isolation. JSONL results
are retained under `benchmarks/results/recall-scale-*`.

Each event has a 128-byte payload and is admitted through Runtime.receive.
`repeated` uses one exact key/connection; `distinct` gives each event a distinct
key/connection. Natural observations remain insufficient; no support/refutation
is fabricated. This is a narrow synthetic structural workload, not semantic
accuracy or conversational quality evaluation. All events use seed 7 and their
index as current step. Main consolidation uses step equal to event count.

## Timing boundaries and resource scope

Entry measurement begins immediately before Runtime.input and stops at the
first statement of Recall, via a benchmark-only probe. Completion separately
includes building the receipt, but excludes its destruction and Replay. Every
sample asserts one Recall entry, exact candidate count and route, zero pread or
pwrite during input, and return to baseline tracked allocation after receipt
release. Five warmups precede 30 retained samples per route. Original Replay
must return the last recorded address, both temporary and Main.

The VRS allocation ceiling here is 256 MiB, with normal default logical storage
and transfer limits (500 GB and 625000000 bytes/s). RSS is Linux process maximum
RSS in KiB; this run does not use the whole-process cgroup profile and does not
validate physical SSD throughput or filesystem capacity. Measurement vectors
are benchmark infrastructure outside the VRS allocation tracker. Reopening uses
normal OS caches; it is not a physically cold disk test.

## Results

All 600 retained input-to-Recall samples were below 1 ms. The largest entry
measurement was 1910 ns. This only establishes the measured 128-byte inputs,
connection counts, CPU and conditions; it is not an arbitrary-scale guarantee.

| Workload | Experiences | Main exact Recall completion median | Main continuation completion median | Merge phase | Reopen phase | Peak RSS |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| One connection | 2048 | 38.481 us | 0.190 us | 311.686 ms | 293.097 ms | 6148 KiB |
| One connection | 8192 | 199.072 us | 0.200 us | 4068.590 ms | 4111.396 ms | 10348 KiB |
| Distinct connections | 2048 | 0.250 us | 0.330 us | 99.293 ms | 93.455 ms | 18872 KiB |
| Distinct connections | 8192 | 0.270 us | 0.370 us | 396.909 ms | 396.894 ms | 60460 KiB |

For 8192 same-key candidates, the exact receipt allocated 262376 bytes;
continuation allocated 824 bytes. Distinct-key query has only one candidate,
so its low receipt cost is not an 8192-candidate comparison. Storage grew to
14542316 bytes (repeated) and 17892515 bytes (distinct) at 8192 events.

Receive includes Recall plus durable recording and core evaluation. Its maximum
was 1608576 ns in the repeated 8192 run; that is not an input-to-Recall violation.
The boundaries must not be combined into an MCP/Replay-total 1 ms claim.

## Next implementation priority from the evidence

At 4x events on one connection, merge/reopen duration grew about 13–14x;
distinct connections grew about 4x. PersistentConnection recovery currently
replays each historical refinement over its full then-current experience prefix.
MainSources reconstructs ended sources before merging. The measurements and
that loop identify repeated source reconstruction as the next path to reduce;
they do not separately attribute every nanosecond of the combined phase.

Preserving an already verified ended source for Main handoff can avoid repeating
its entire recovery in the same process. Historical verification and original
lineage must remain intact. Cold restart recovery, asynchronous publication and
much larger disk-backed graphs remain unfinished. No checks or core operations
were removed to obtain these measurements.
