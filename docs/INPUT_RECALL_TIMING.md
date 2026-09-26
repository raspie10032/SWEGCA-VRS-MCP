# Input-to-Recall boundary measurement

The benchmark calls the real Runtime::input route with natural bytes and records
a steady-clock timestamp at the first statement of ExperienceRouter::recall_cue.
It includes the Runtime session lookup, full content cue hash, temporary lookup,
Main generation check and Main lookup as applicable. It excludes candidate
materialization after Recall entry, Replay, recording and transport framing.
This is the C++ input API boundary, not a live client/keyboard-hook measurement.
The client hook remains uninstalled.

The entry callback exists only when SWEGCA_RECALL_ENTRY_PROBE is defined for the
benchmark build. Normal production builds contain no callback/clock operation.
Every sample checks exactly one Recall entry, the expected temporary/Main/miss
result, and zero pread/pwrite calls. The graph has five prepared input identities,
not a billion-experience corpus. Sessions and Main use normal persistence,
shuffle/core refinement and explicit-end merge during setup outside timing.

Command: `taskset -c 6 make -j1 build/input-recall-bench`, then
`taskset -c 6 ./build/input-recall-bench`. GCC -O3, C++20, ff-contract=off;
AMD Ryzen 7 9800X3D. Palworld was not stopped. Five warmups followed by 30 samples
per case, with other desktop scheduling still possible. Baseline is e788899 plus
only this benchmark/probe instrumentation.

## Observed baseline (nanoseconds)

| Route | Input bytes | Median | p95 | Maximum | Samples >=1ms |
|---|---:|---:|---:|---:|---:|
| Temporary | 0 | 200 | 200 | 200 | 0/30 |
| Temporary | 128 | 510 | 520 | 520 | 0/30 |
| Temporary | 4096 | 10000 | 12410 | 12520 | 0/30 |
| Temporary | 65536 | 156602 | 180542 | 254612 | 0/30 |
| Temporary | 1048576 | 2688247 | 3142241 | 3183082 | 30/30 |
| Main | 0 | 200 | 200 | 200 | 0/30 |
| Main | 128 | 510 | 511 | 530 | 0/30 |
| Main | 4096 | 9990 | 13380 | 13800 | 0/30 |
| Main | 65536 | 157031 | 181992 | 232252 | 0/30 |
| Main | 1048576 | 2746287 | 3022831 | 3055031 | 30/30 |
| Missing | 0 | 200 | 200 | 201 | 0/30 |
| Missing | 128 | 510 | 510 | 510 | 0/30 |
| Missing | 4096 | 11380 | 12230 | 14160 | 0/30 |
| Missing | 65536 | 163671 | 222852 | 356753 | 0/30 |
| Missing | 1048576 | 2759328 | 3327433 | 3392104 | 30/30 |

The 1MiB cases fail the user's <1ms requirement. These numbers do not establish
success at unmeasured sizes, graph scales, client boundaries or tail percentiles.
The full-input SHA-256 loop is a common linear cost in all three paths and is
the next optimization candidate; direct component profiling is still needed to
quantify its exact share. Do not shorten/drop input bytes, change the digest,
move measurement after hashing or relabel MCP/Replay latency as this boundary.

## Region specification provenance

The permitted public SWEGCA architecture/paper files inspected in this turn do
not specify concrete region/portal composition rules. Local region_graph and
portal_activation Python files are untracked in the current research checkout
(9ec73bdda9); their user-original lineage is unverified. Their implementation is
not adopted as the new C++ graph design. The source design path has been asked
for while independent latency work proceeds. Discarded C++ candidates were not
opened or reused.
