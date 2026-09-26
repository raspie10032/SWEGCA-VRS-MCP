# One block reader per recovery task

PersistentConnection history recovery now uses a private SessionStore read cursor.
The cursor retains at most one ExperienceBlock reader. Consecutive history and
original reads in that block reuse its file descriptor; switching blocks closes
the old descriptor before opening and validating the next one. Each recovery task
owns its cursor, so parallel workers neither share a mutable reader cache nor
retain every block's file descriptor.

This caches an open handle, not record bytes, decoded observations, tallies or
verdicts. Every requested record still goes through ExperienceBlock::read:
address/block/bounds validation, the per-read allocation limit, full payload hash
and trailer verification remain. Original decoding, event lineage and the full
ordered SWEGCA refinement replay are unchanged. Block headers are verified when
a handle opens, rather than reopening the same immutable block for every record.
The store/its immutable inventory must outlive the cursor. External mutation of
owned block files during recovery is outside the existing single-owner contract.

No permanent cache, new public host method, on-disk format or authority decision
is introduced. Ordinary selected SessionStore::read / Replay remains unchanged.
Readers close on success and exception. The existing shared transfer limiter
charges actual pread requests, including block header reads on each open.

A real 48-observation single-block history test intercepts open() and verifies
exactly one reader open for its entire backward/forward history traversal and
original reads. Existing multi-block, forged-refinement, historical-head,
partial-write and recovery tests continue to cover the original checks. Parallel
recovery fault injection covers worker I/O errors and allocation failure cleanup.
This optimization does not remove the quadratic sum of historical refinement
prefixes for a long single connection, or solve asynchronous input/merge scheduling.


Native validation: persistent connection tests 69 checks, parallel recovery 1575
checks (1468 allocation-failure points), and real MCP subprocess tests 918 checks
passed. The single-block open count is asserted, rather than inferred from timing.

With the same 8192-event benchmark and no compiler running concurrently:

| Workload | Workers / CPUs | Prior full reopen | With reader reuse | Peak RSS |
| --- | --- | ---: | ---: | ---: |
| Distinct connections | 2 / 6,7 | 360.813 ms | 245.883 ms | 72480 KiB |
| One connection | 1 / 6 | 3626.110 ms | 3614.046 ms | 10700 KiB |

These single-run observations are not a stable speedup guarantee. The second
result shows that long-history computation remains dominant; its small change
must not be described as solving that bottleneck. Raw results are stored in
benchmarks/results/recall-reader-{distinct,repeated}-8192.jsonl. Original-address
Replay and final memory accounting checks passed. All 300 retained input-to-Recall
samples remained below 1 ms (maximum 790 ns) for these 128-byte payloads; live
client and arbitrary-scale latency are not established by this fixture.
