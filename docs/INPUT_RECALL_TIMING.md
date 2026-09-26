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

## SHA-instruction optimization

The same CPU and benchmark, after adding runtime-dispatched SHA256RNDS2 and
SHA256MSG1/MSG2 compression, produced the following 1MiB samples. All input bytes,
SHA framing, stored addresses and the measurement boundary remain unchanged.

| Route | Prior median ns | New median ns | New p95 ns | New maximum ns | >=1ms |
|---|---:|---:|---:|---:|---:|
| Temporary | 2688247 | 454404 | 524906 | 562476 | 0/30 |
| Main | 2746287 | 452084 | 590526 | 606386 | 0/30 |
| Missing | 2759328 | 453364 | 500385 | 503405 | 0/30 |

The tested 0, 128, 4096 and 65536-byte cases also had zero >=1ms samples.
This fixes the measured 1MiB case on this machine, not arbitrary input sizes,
unsupported CPUs, large graph scale, live client hooks or worst-case scheduling.
An intermediate rounds-only acceleration still took approximately 1.4ms for
1MiB; moving the unchanged message schedule to SHA instructions removed that
remaining measured cost. No external cryptographic library/wheel was introduced.

The x86-64 implementation checks CPUID SHA and SSSE3 support once and dispatches
only where available. Other targets retain the scalar implementation. A build
with SWEGCA_SHA256_SCALAR_ONLY forces the scalar path for differential checking.
The native function's target attribute confines required CPU instructions;
the overall binary does not require global -march=native or -msha.

Independent verification compares 1052 binary inputs (0..257 and larger block/
padding boundaries, four alignment offsets, up to 1MiB) against Python hashlib.
Every input also uses six different streaming partitions. Both native and
forced-scalar executables must agree with the independent digest and each other.
The same vectors run with undefined-behavior sanitization. Python is used only
as the test oracle, not in the C++ runtime.

After the final compression change, core tests passed 3961 checks with zero
hot-path allocations; runtime lifecycle passed 101 checks. Core benchmark
block-average medians remained in ns: 1/4/8-axis judgment 28.405/51.0668/85.584,
observation admission 2.79183, head publication 5.77316, Replay comparison
3.56143, strength update 1.37061, four-axis connection verification 56.318.
These are batch-average timing medians, not individually sampled tail latency.
