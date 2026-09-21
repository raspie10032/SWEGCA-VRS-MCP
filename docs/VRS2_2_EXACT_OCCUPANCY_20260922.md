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

| Fresh read process | Random exact Replay calls | Median | p99 | Maximum | ≥1 ms |
| --- | ---: | ---: | ---: | ---: | ---: |
| 1 | 10,000 | 0.00725 ms | 0.01082 ms | 0.138851 ms | 0 |
| 2 | 10,000 | 0.00728 ms | 0.01020 ms | 0.114282 ms | 0 |
| 3 | 10,000 | 0.00748 ms | 0.01288 ms | 0.330823 ms | 0 |

The exact results are in
`evals/vrs22_context/results/exact_occupancy_p16/`. Their SHA-256 values are:

- `prepare.json`: `ae7adcd49194e17ff9a07f338c05d2deed906db9aa897c870f3b18ae9fc3d265`
- `read.json`: `b6f20b33b704e7043e981764ac639e8ab30801f9da0bc669ed6c0f02672db08b`
- `read_repeat2.json`: `b4779ea15e042851dc69bf565311f4d433ae59dc7653967674e83ca45556b0c9`
- `read_repeat3.json`: `e48d2953efc3eff4b6607d992a9c1ae84401cf1a004478074963510466f22a73`

The tool SHA-256 is
`1f827895c18025f34b87d1ec9ed662bd2f362748ec517bc204bf0daf1556ff99`.
The installed runtime was the `f1ea47a` product wheel,
`ade989360646b009d40031e45091d2043b3af8ee4ca0854cdd08817fba5ee285`.

This measurement covers one high-occupancy prefix and a sample of exact
addresses. It does not establish the requested unconditional <1 ms bound for
every main size or natural query, a billion distinct experiences, or processing
of one billion VRS parameters. The VRS parameter unit remains undefined by the
available product state and must not be replaced with an address count.
