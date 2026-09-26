# Whole-process VRS memory profile on Linux

The stdio executable supports `limited-create ROOT CONFIG.json` and
`limited-open ROOT CONFIG.json`. These modes read the bounded bootstrap config,
then exec the operating system's systemd-run user launcher with a transient,
collected service. No resident unit is installed and no existing service is
stopped or reconfigured. The service runs the same native C++ executable.

Properties:

- MemoryMax = `memoryBytes` (example 4,294,967,296 bytes, 4GiB).
- MemorySwapMax = 0.
- CPUAffinity = the explicit `cpuAffinity` config string (example `"6 7"`
  on this user's 16-thread PC, preserving other CPUs for Palworld).
- OOMPolicy = kill; the group is terminated on exhaustion.
- --pipe/--wait/--quiet preserve protocol stdin/stdout and return failure.
- --collect removes the transient unit after completion.

The service uses bounded-create/open internally and verifies cgroup v2
memory.max, memory.swap.max and effective sched_getaffinity before constructing
Runtime or opening its storage root. Missing user-manager/controller support or
mismatched controls fail startup; there is no retry without limits. Plain
create/open remain direct developer entrypoints and do not impose an OS limit.
Use the limited modes for the stated constrained runtime profile.

The kernel accounts the group's charged memory, including allocations outside
PMR, thread stacks and charged file cache. This is stronger than checking only
MemoryBudget. MemoryBudget still bounds its tracked allocations, but other
memory overhead means the kernel may terminate the process before that budget
is exhausted. Journal durability/recovery remains necessary; the limit is not
an assurance that every workload fits. The bootstrap/systemd management process
is not inside the final VRS service group.

The profile currently supplies Linux memory and CPU isolation. Logical block
transfer pacing and storage quotas still use the C++ implementation. It does
not configure physical block-device io.max or a filesystem-wide disk quota.
No client hook or live VRS service was installed by this change.

## Verified locally

- Actual limited MCP process passed 363 protocol/lifecycle checks with 64MiB
  memory.max, zero swap and CPUs 6/7.
- Native probe checked those kernel controls directly.
- A separate 64MiB probe attempted to touch 128MiB. It was killed; the exact
  transient unit's journal confirmed oom-kill. Only the isolated test process
  was targeted, without allocating a 4GiB pressure workload.
- systemd-run reports exit 1 for this OOM on this host, so a SIGKILL-like launcher
  code alone is not used as proof. The test reads the exact unit's journal.

Opt-in Linux check: `make check-resource-profile`. It requires a reachable
systemd user manager and CPUs 6/7 for this local test profile; it is not part of
portable unit checks. Normal completions produce only JSON-RPC on stdout.

## Actual VRS OOM/recovery scenario

`make check-oom-recovery` is a separate opt-in local integration check. It starts
one explicitly named transient VRS test service on CPUs 6/7 with 64MiB kernel
memory and no swap. For fault injection only, that service's PMR budget is set
to 256MiB and its frame allowance to 128MiB, so the kernel limit is reached
before the allocator's own guard. Normal recovery then uses limited-open with
matching 64MiB limits. This deliberate mismatch is not a production profile.

The test first receives successful publication responses for two complete
experiences. It then streams an incomplete oversized JSON request until the
kernel kills that exact service. The journal confirms oom-kill, not merely an
unexplained nonzero exit. In the observed run about 60MiB reached the pipe before
termination; the unfinished frame was never admitted into Runtime.

After restarting under the normal limited mode, the test verifies:

- No implicit session end and no Main merge: work reports zero merged sources.
- The original session resumes and recalls exactly the two published originals.
- Selected Replay returns the identical original address and exact UTF-8/NUL
  payload published before the kill.
- Explicit end then produces exactly one Main merge.
- A second restart and a fresh session recall those experiences from Main,
  again replaying the same original address.

This proves recovery of acknowledged originals after an OOM while buffering an
unadmitted frame. It does not prove every interruption point during record write,
catalog rename, merge publication or a live client's delivery/retry behavior.
The initial 128MiB-PMR injection exited through the allocator guard instead of
kernel OOM; that was not accepted as OOM evidence, and the injection was adjusted.
Only the uniquely named test service is stopped if the fault-injection check
fails; no existing service is targeted.
