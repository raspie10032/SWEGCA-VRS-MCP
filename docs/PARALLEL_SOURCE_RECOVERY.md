# Independent connection recovery workers

SessionRuntime recovery now prepares private slots for every catalog connection,
then recovers independent connection histories on the configured worker slots.
Each history still runs its original ordered append/refinement chain, reproduces
all actual shuffled observation reductions, and checks recorded revision,
strength and refinement digest. No history checkpoint or saved tally bypasses
core verification. Source reads use separate readers over an immutable store
inventory and the shared thread-safe transfer budget.

Only after every task succeeds is the cue index built, in canonical connection
and original-index order. No reader receives a partially recovered runtime.
A failed worker records its exception; all started threads join before exceptions
propagate and private slots are destroyed. Failed thread creation also joins
previously started workers. Empty slots safely destruct. Recovery does not write
to the source; tests intercept pwrite to verify this.

The existing RuntimeConfig.merge_workers / host mergeWorkers setting controls
both independent recovery and independent Main preparation. Each phase joins
before the next starts. Direct SessionRuntime callers can pass recovery_workers
(default 1). Zero is rejected. A single connection remains serial. Callers with a
custom MemoryBudget upstream must provide a thread-safe resource, as for parallel
Main preparation. The core owns no thread or memory resource policy.

This does not make Runtime input/work concurrent, bypass cold history replay,
parallelize dependent history within one connection, or guarantee a speedup on
small or I/O-bound sources. Active input ownership and same-process ended cache
handoff are unchanged. Native C++ jthread is used without external dependencies.

The dedicated parallel-recovery test compares serial and parallel restored heads,
strengths and original addresses for accept/refute/insufficient histories. It
checks deterministic Recall ordering and selected Replay, partial pthread_create
failure, worker read failure, allocation failure at every recovery allocation
point, joined worker cleanup, zero writes and zero remaining tracked allocation.


Native tests passed 1575 checks, including 1468 injected allocation failure
points. Worker allocation failures were observed; their count varies with
scheduling. Lifecycle (128) and real MCP subprocess tests (918) also passed.

The scale benchmark now accepts optional WORKERS and uses atomic read/write
counters so concurrent recovery instrumentation is race-free. With CPU affinity
6,7, 8192 distinct one-observation connections and no concurrent compilation:

| Workers | Full reopening | Warm Main merge | Maximum RSS |
| --- | ---: | ---: | ---: |
| 1 | 394.667 ms | 38.925 ms | 60308 KiB |
| 2 | 360.813 ms | 36.205 ms | 72512 KiB |

Raw results: benchmarks/results/recall-recovery-workers-{1,2}.jsonl. This is one
synthetic run per setting, not a stable speedup guarantee. Reopening includes
source validation, connection recovery, Main reconstruction and query mounting;
it does not isolate just the worker loop. All 300 retained input-to-Recall
samples were below 1 ms for the measured 128-byte payloads. Extra worker/runtime
allocation raised observed maximum RSS despite equal final tracked bytes.
Single-connection history cost is unchanged and remains a separate limitation.
The UBSan trap parallel-recovery build also passed all 1575 checks.
