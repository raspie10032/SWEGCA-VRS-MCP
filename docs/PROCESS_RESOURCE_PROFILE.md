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
