# Reusing a verified ended session for Main merge

MainSources now owns the stable SessionStore and SessionRuntime allocation from
session creation/resumption. Runtime's active route borrows that cache. The lease
keeps normal Main cache cleanup from destroying a still-active session. Stores,
caches and registry nodes all use the shared VRS MemoryBudget.

Explicit end still runs the existing core-controlled end and original publication
steps. Only afterward does Runtime destroy the input route and return its lease.
No graph merge runs in end or input. The latest ended decoded cache can remain
available for the separate work() call. That call resolves the already published
source to the same verified object, rather than reconstructing every historical
refinement again. Main merge still traverses/shuffles all actual incoming and
previous observations through the core; no stored verdict or tally replaces it.

At most one unleased decoded source cache is retained; ending another session or
resolving another source may release it. An active leased cache remains protected.
After merge success or failure, unleased caches are released while original stores
remain available for selected Replay. Thus this is a same-process handoff, not an
unbounded cache of all pending session graphs. If the cache was released, normal
source reconstruction still applies. A restart still reconstructs history from
originals and recorded rules; its verification has not been bypassed.

Failed route construction returns its lease. Failed merge allocation leaves its
durable published source available to retry and does not release another active
session. A poisoned Main journal still requires existing restart recovery. Store
pointers held by published Main entries do not move during handoff or cache release.
Opaque receipt issuer identities still expire with the input route.

Regression coverage includes:

- a 32-observation ended source merged with zero pread while a new active session
  remains usable, including further observation and selected Replay;
- deferred generation publication (end alone does not merge);
- source cache release without original-store invalidation;
- write failure and restart recovery via the existing lifecycle tests;
- allocation failure during queued work, preservation of the active lease, and
  retry through normal disk reconstruction after the ended cache is discarded;
- all owner/budget allocations returning to zero on destruction.

This does not implement simultaneous input and merge execution. Runtime remains
serialized. Cold restart cost, checkpoint design, asynchronous scheduling and
large disk-backed graph query work remain separate unfinished requirements.


## Measured same-process handoff

Same native workload/CPU/flags as RECALL_SCALE_MEASUREMENTS.md, 8192 events:

| Workload | Previous merge phase | Handoff merge phase | Restart after handoff change |
| --- | ---: | ---: | ---: |
| One connection | 4068.590 ms | 2.320 ms | 3626.110 ms |
| Distinct connections | 396.909 ms | 36.928 ms | 384.245 ms |

Final runs were performed after compiler processes finished. Raw results are
`benchmarks/results/recall-scale-handoff-*-8192.jsonl`. This timing starts at
work(), after explicit end/publication. It does not measure total session time
or eliminate the original per-input core work. Historical restart verification
is unchanged; the numerical restart variation is not a claimed algorithmic gain.
All 300 retained input-to-Recall samples in these two runs were below 1 ms;
maximum 660 ns for these 128-byte inputs. Both selected Replay checks and final
zero-allocation checks passed. Native lifecycle checks: 128; source discovery:
32; real stdio subprocess protocol: 918.
The UBSan trap lifecycle build also passed all 128 checks.
