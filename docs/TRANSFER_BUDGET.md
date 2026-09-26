# Shared VRS block-transfer allowance

RuntimeConfig.io_bytes_per_second and the required stdio config field
`ioBytesPerSecond` select an aggregate read+write allowance. The default is
625,000,000 bytes/second, corresponding to decimal 5Gbps. Larger positive
settings are allowed. Zero is rejected.

One TransferBudget is owned by the runtime's shared StorageBudget. All
ExperienceBlock pread/pwrite requests use it, including creation headers,
selected Replay, catalog pointer reads, session recovery and Main journal
recovery. A request is at most 1MiB. The same allowance follows block moves and
is passed when opening readers and writers. Standalone low-level blocks without
a resource owner retain their explicitly unbudgeted behavior.

The limiter allows one 1MiB chunk of initial/idle credit (rounded up to a whole
nanosecond). Service times round upward, so very small operations or extremely
high configured rates may be more conservative. Idle time cannot accumulate
more credit. Threads synchronize admission arithmetic, release the lock before
sleeping, and recheck on wake; late sleepers cannot redeem stale reservations
as an unbounded catch-up burst. Failed/short/interrupted syscalls still charge
the requested transfer, conservatively accounting retries as additional work.

This is admission pacing of logical syscalls, not kernel block-device shaping.
Page-cache hits are also charged. Filesystem metadata, fsync implementation I/O,
writeback batching and unrelated processes are not rate-controlled by it. The
1MiB burst means this is not an instantaneous byte-by-byte physical-device cap.
Hardware/cgroup-level confirmation under the specified SSD profile remains a
separate requirement; do not claim it from these tests.

No transfer budget operation occurs on the in-memory input-to-Recall path.
Actual Replay/record/recovery I/O is paced. Tests confirm unchanged transfer
counters for input/Recall and increasing counters for selected Replay.

Verification covers deterministic admission/retry times, no consumption while
waiting, bounded idle credit, late wakes, concurrent admission, integer/rate
boundaries, and a real 10ms pacing case. Runtime lifecycle and actual MCP
subprocess regressions exercise the connected storage paths.
