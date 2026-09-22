# VRS 2.2 exact-address high-occupancy check — 2026-09-22

The product's exact-address directory uses fixed disk segments and seals a
segment for new addresses at 70% occupancy. The prior maximum-level benchmark
exercised five routing levels but used empty sealed lower levels. This check
exercises occupied slots at the production first-level size, then crosses its
real seal boundary into the next level. It reads the product Replay capsule and
checksums directly. Region, portal and main-owned Re-evidence remain covered by
the separate full-path tests, not by this index benchmark.

`tools/benchmark_vrs22_exact_occupancy.py` created 45,876 synthetic addresses
under one hash prefix. The first production `2^16`-slot level sealed with
45,875 addresses, and one address entered the `2^18`-slot level. Each exact
lookup read and verified its immutable Replay capsule. One original VRS
episode supplies the benchmark capsule content; therefore 45,876 addresses are
**not** 45,876 distinct experiences or a VRS parameter count. Preparation took
0.965306 seconds and allocated 50,974,720 bytes of SSD blocks.

Before each of three separate read processes, `POSIX_FADV_DONTNEED` requested
eviction of the four benchmark `.vrs` files. Each process used the product's
bounded startup address warmup and allocated-page Replay prefetch before the
timed lookups. The systemd cgroup set `memory.max=4294967296`,
`memory.swap.max=0`, and physical SSD `259:3` read/write rates separately to
625,000,000 bytes/s. The first process peaked at 79 MB cgroup memory.

| Fresh read process | Random exact Replay calls | Median | p99 | Maximum |
| --- | ---: | ---: | ---: | ---: |
| 1 | 10,000 | 0.00725 ms | 0.01082 ms | 0.138851 ms |
| 2 | 10,000 | 0.00728 ms | 0.01020 ms | 0.114282 ms |
| 3 | 10,000 | 0.00748 ms | 0.01288 ms | 0.330823 ms |

The exact results are in
`evals/vrs22_context/results/exact_occupancy_p16/`. Their SHA-256 values are:

- `prepare.json`: `ae7adcd49194e17ff9a07f338c05d2deed906db9aa897c870f3b18ae9fc3d265`
- `read.json`: `292036af275ec4a900b52a9233009cf67230d3f9a60d09a83e7a6ce52c3b33b5`
- `read_repeat2.json`: `d0a2c6778774c6c55899c723678e91f1bafbe10620da1452b05cd9a9d5c2a348`
- `read_repeat3.json`: `a7afab1a090f0c39b48e8e53d7e70d1f7bfd51d48765c14f3aa6154263ca6256`

The tool SHA-256 is
`22b2f3e804942dd92014e08ef55a1a793fd4ecc8bffb2cc23c8bdf8bdf31eebd`.
The installed runtime was the `f1ea47a` product wheel,
`ade989360646b009d40031e45091d2043b3af8ee4ca0854cdd08817fba5ee285`.

This measurement covers one high-occupancy prefix and a sample of exact
addresses. It does not establish the requested unconditional <1 ms user-input → first-Recall route for
every main size or natural query, a billion distinct experiences, or processing
of one billion VRS parameters. The VRS parameter unit remains undefined by the
available product state and must not be replaced with an address count.
