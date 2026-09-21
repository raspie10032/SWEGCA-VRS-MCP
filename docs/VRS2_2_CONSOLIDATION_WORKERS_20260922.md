# VRS 2.2 region consolidation: 16-worker audit

This is a **source-candidate benchmark** on a copy of 15,630 existing native
experience records. It does not measure one billion VRS parameters, and it does
not prove a seconds-scale bound at that size. The current resident deployment
is still waiting for the active Codex task's SessionEnd handoff. A rebuilt
wheel from source commit `67cae88` is installed separately and passed archive,
package-file, session-capture, and original Replay checks. Its SHA-256 is
`2b8b1754f8d8efa443f7c6e2837417f18f3c98e42d637e51105e833ae434f051`.
The one-shot post-SessionEnd watcher now points at this candidate, but the
current session's old live runtime and main have not been switched.

The copied shard contained 71 fine regions, 147,356 nodes, and 3,312,964 flat
edges. The comparison ran the identical frozen generation for four cycles with
one and sixteen requested VRS region workers. `OPENBLAS_NUM_THREADS`,
`OMP_NUM_THREADS`, and `MKL_NUM_THREADS` were each set to 1 to keep library
threads from obscuring the worker count. Both worker counts produced the same
VRS version ID in every run, and the new candidate matched the pre-change
version ID. The full standalone suite passed **125 tests** in 182.49 seconds.

| Source | One worker wall | 16 workers wall | Within-run speedup | Fine-region time at 16 workers | Peak threads, 1 / 16 |
|---|---:|---:|---:|---:|---:|
| Before this change | 4.227 s | 3.872 s | 1.092× | 2.185 s | 2 / 18 |
| Candidate repeat 1 | 4.372 s | 3.446 s | 1.269× | 1.747 s | 2 / 18 |
| Candidate repeat 2 | 4.688 s | 3.613 s | 1.298× | 1.860 s | 2 / 18 |
| Candidate repeat 3 | 4.436 s | 3.607 s | 1.230× | 1.821 s | 2 / 18 |

All four cgroup-limited runs passed with `memory.max=4,294,967,296`, swap 0,
and `io.max` read/write at **625,000,000 bytes/s** on disk `259:3`
(`/dev/nvme2n1`). The state was mounted from its partition
`/dev/nvme2n1p3`; the benchmark verifies that relationship instead of
comparing the partition's virtual filesystem device number with the disk's
I/O-controller number. The candidate's highest observed cgroup memory peak
was 983,465,984 bytes. Its state occupied 85,905,408 allocated bytes, below
the exact 500,000,000,000-byte storage ceiling. No SQLite module was loaded.

The code keeps the same recursive region algorithm, portal/connection logic,
warm-start labels, and ordered final ID assignment. It computes independent
coarse-region splits in parallel and uses sorted member indices instead of
allocating a full-graph mapping array for each region. A one-worker profile
identified `fine_regions` and `build_inputs` as the largest cumulative Python
paths. Profiling changes wall times, so its numbers are diagnostic rather than
the speedup estimate. The measured improvement remains modest; more work is
required before any large-scale consolidation promise can be made.

Reproduction script: `local/bench/vrs2-consolidation-workers.py`. It reads a
separate copy of an existing native experience shard; the original is not
modified. One bounded run used:

```sh
systemd-run --user --pipe --wait --collect \
  --unit=vrs22-consolidation-fineparallel-20260922 \
  -p MemoryMax=4294967296 -p MemorySwapMax=0 \
  -p 'IOReadBandwidthMax=/var/tmp 625000000' \
  -p 'IOWriteBandwidthMax=/var/tmp 625000000' \
  /usr/bin/env OPENBLAS_NUM_THREADS=1 OMP_NUM_THREADS=1 MKL_NUM_THREADS=1 \
  /home/raspie/.local/share/swegca-vrs2-runtime-2.2-candidate/venv/bin/python \
  /var/home/raspie/Documents/Codex/SWEGCA-VRS-MCP-vrs22-repair-20260921/local/bench/vrs2-consolidation-workers.py \
  --state-dir /var/tmp/vrs22-existing-experience-merge16-20260922 \
  --io-device /dev/nvme2n1 --cycles 4
```

Raw results and the diagnostic profile are in
`evals/vrs22_context/results/consolidation16/`; the baseline SHA-256 is
`76de22543c957f4aefc24e3ea7d9ca614bf90650761d5690d97551283fd6925c`.
Candidate repeat hashes are
`ad2241082c099fb0378925b26867167ca052f87f4cba7ca006298300ac1510e1`,
`6f6bc48502720a255ac69271560775a38b26a365f1953e527284826a4a62e0f7`,
and `d7e1d628b54e8d60bd6682a0eba846d769c0ee9a9d31575a1047636605b2bbd2`.
