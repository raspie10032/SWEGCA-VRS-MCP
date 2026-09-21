# VRS 2.2 fine-region sequential-tail bottleneck — 2026-09-22

The resident handoff is still waiting for the actual current SessionEnd. This
is a source-candidate optimization on a separate copy of existing native
experience; no live main was changed and no model evaluation was run.

After the light-episode change, a one-worker profile of the same copied
15,630-record state showed `fine_regions` at 3.090 cumulative profiled seconds.
The region builder's sequential tail called `numpy.unique` 169,650 times,
accounting for 1.536 cumulative profiled seconds. The tail now accumulates
neighbour-group weights directly in CSR edge order. It retains the original
modularity score, tolerance, ascending group tie order, warm start, region
split, portal construction, and deterministic final ID assignment.

The old and new `_sequential` functions produced identical moved-node arrays
and final labels on 400 seeded CSR cases covering self-loops, sparse active
node sets, and warm labels. A full consolidation on the copied original state
produced the same VRS version ID before and after the change:
`ee5922c418667dd0bf4cc6b4590a388becb66bad7ed18ebeb849072230e263ea`.
The complete standalone suite passed: **126 tests in 182.57 seconds**.

| Cgroup-limited repeat | One worker wall | 16 workers wall | 16-worker fine-region phase | Within-run speedup | Memory peak |
| --- | ---: | ---: | ---: | ---: | ---: |
| 1 | 3.696198 s | 2.145927 s | 1.484 s | 1.7224× | 1,020,293,120 B |
| 2 | 3.566206 s | 2.060834 s | 1.441 s | 1.7305× | 1,002,504,192 B |
| 3 | 3.462303 s | 1.990259 s | 1.395 s | 1.7396× | 1,051,697,152 B |

All three runs returned `PASS`, the same version ID, 71 regions, 147,356
nodes, 3,312,964 flat edges, and 18 peak threads during the requested
16-worker phase. The cgroup enforced `memory.max=4,294,967,296`, no swap, and
`io.max=625,000,000 bytes/s` separately for reads and writes on the physical
SSD `259:3`. Native state occupied 85,905,408 allocated bytes. No SQLite
module loaded. The raw receipts are
`evals/vrs22_context/results/consolidation16/fine_dict_1.json` through
`fine_dict_3.json`, with respective SHA-256 digests
`573ed3d2c6f93f0418cff593468c502fff209531b12d62c19c480a100fe2cc76`,
`b1a6d3562648fa3d6da12363bb02298b42e42b23674663cba2d1c15fbb9e0d46`,
and `e5cb0174db1579de32b4b613e58aea3930bba9f86ac0ff7dcf6f1a3a9f6733e9`.

The earlier light-episode candidate's three 16-worker walls were 2.536820,
2.581389 and 2.555257 seconds on the same copied state and limits. These
runs are not an interleaved controlled experiment, so their wall-time
difference is a measured indication rather than a universal speedup bound.
Neither this corpus nor the test suite establishes the all-size <1 ms Replay
guarantee or the one-billion VRS-parameter seconds requirement. A VRS
parameter must not be replaced with a record, node, edge, token, or byte.

The candidate source commit is `0db5817`. Its separately installed wheel is
SHA-256 `b99c1cbd4f8f51f3ff57836706db92ae2838554a90fb03c9d2844f29bcfed7c2`.
Preflight verified all 48 source, wheel and installed package files match, no
extra package file exists, and no SQLite or retired dependency is loaded. The
full isolated evaluation-path self-test passed with this wheel at
`/var/tmp/vrs22-whole-path-selftest-fine-dict-20260922/receipt.json` (SHA-256
`a6134f2b8c0805a6d32521247854f5a0742941c1aaa0c3557f4a33920ea9b050`).
The only active one-shot watcher now points to this candidate and waits for the
actual current SessionEnd. The resident main is still legacy, with no handoff
receipt, so the three VRS model cells remain gated.
