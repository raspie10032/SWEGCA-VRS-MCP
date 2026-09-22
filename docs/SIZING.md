# VRS 2.2 resource and read-index boundaries (2026-09-21)

This document describes the repaired `vrs-regions` lineage. Earlier 60,000-record guidance treated
manual project bundles and lexical warm views as the scaling mechanism. The current resident instead
creates storage shards automatically and keeps every shard as a complete VRS main.

## Hard limits

| resource | enforced limit |
| --- | ---: |
| resident process RSS | 4 GiB |
| SSD transfer assumption | at most 5 Gbit/s (625 MB/s) |
| physically allocated state storage | 500 GB (500,000,000,000 bytes) |
| consolidation workers | 16 |
| default records before opening the next automatic shard | 8,192 |

The record count is a storage boundary, not an experience-quality boundary. An original episode is
never divided. Its revisions stay with the shard that owns its source lineage. The 500 GB admission
check counts allocated filesystem blocks because this is an SSD-capacity limit. Logical sparse-file
sizes are reported separately. The one-second storage scan cache reserves the full 625 MB that a
5 Gbit/s device could add during that interval and forces a fresh scan near the boundary.
If a directory cannot be walked or a file cannot be statted during that scan,
admission fails instead of treating the unknown allocation as zero.

## Automatic shards and connections

`Resident.main_for_ingest` uses the current automatic shard until its configured record boundary and
then creates the next shard. Explicit revisions and `supersedes` follow the existing experience or
source route. Each shard retains its full graph, stable VRS version, evidence accumulator decisions,
regions, portals, original episodes and checkpoint.

Cross-shard reads use three disk structures owned by main:

- an exact address and source-lineage directory;
- a cue to exact-experience posting directory;
- an immutable read projection of each complete checkpoint, including strengths, states, evidence
  decisions, regions, memberships, cue strengths and portals.

The projection is a read form of the same VRS generation. It is neither a lexical-only replacement
nor another authority. Natural recall starts with cue postings, ranks one global candidate set, then
uses exact Replay and current projected Re-evidence. Cold checkpoints do not have to be loaded into
Python objects for that path.

## Disk directory growth

Exact address/source segments begin at 2^16 slots per hash prefix and can grow through powers
18, 20, 22 and 23. Cue segments begin at 2^12 slots and can grow through powers 14, 16, 18, 20 and
22. Each level seals at 70 percent occupancy. A durable per-level header records the published count
and whether absent keys must continue to the next level.

Sealing below full occupancy is required for lookup latency. It leaves an empty slot after a small
expected number of probes, while the header distinguishes "absent here, continue" from "absent from
the directory." The last exact level provides more than one billion usable address slots across 256
prefixes even at the 70 percent seal threshold. This is address capacity; it is not a definition of
the user's "one billion VRS parameters."

Replay capsules preserve the original observation, provenance, revision, outcome, evidence
references and source addresses. The complete original observation and derived cue block are stored
in the same checksummed capsule. Replay verifies and binds the exact immutable payload; observation
JSON and cue strings are decoded when their fields are consumed. This avoids a second full-text copy
inside the named Replay boundary.

## Current-experience-copy measurement

Measured with wall-clock time on a consistent checkpoint-covered copy of the
active experience on 2026-09-21. The copy contains 15,637 journal rows and
15,630 unique original experiences; the active tail was excluded at its existing
verified checkpoint rather than sampled while it changed.

| quantity | result |
| --- | ---: |
| unique experiences | 15,630 |
| cue occurrences | 1,796,514 |
| load existing VRS generation | 2.781 s |
| first exact/source/cue directory build | 22.877 s |
| process peak RSS | 1,055,240,192 bytes |
| complete state logical bytes | 2,178,256,214 bytes |
| complete state allocated bytes | 691,716,096 bytes |

The first 50,000-random run used the fresh derived directory:

| sample | median | p95 | p99 | maximum | at least 1 ms |
| --- | ---: | ---: | ---: | ---: | ---: |
| exact capsule Replay | 0.0115 ms | 0.0297 ms | 0.0575 ms | 0.6230 ms | 0 |
| Déjà vu through Replay | 0.0205 ms | 0.0427 ms | 0.0728 ms | 0.3684 ms | 0 |
| through Re-evidence | 0.0750 ms | 0.3975 ms | 0.7877 ms | 2.6953 ms | outside gate |

Focused follow-up repeated the largest 107,773-byte observation and the largest
8,378-cue experience 5,000 times each. Their through-Replay maxima were
0.8391 ms and 0.7135 ms; same-boundary thread CPU maxima were 0.6092 ms and
0.6183 ms. Re-evidence maxima were 3.1935 ms and 3.7271 ms and remain reported
outside the requested Replay boundary.

The same 15,630 experiences were then attached as one complete linked VRS shard
to an empty primary. A completely fresh main-owned read directory took 80.813 s
to build exact Replay and 4.246 s to build the complete VRS read projection.
It used 2,146,222,080 peak RSS bytes and 716,795,904 allocated disk bytes. This
run kept the original linked store in place and did not export or re-ingest it.

| linked-shard sample | median | p95 | p99 | maximum | at least 1 ms |
| --- | ---: | ---: | ---: | ---: | ---: |
| exact capsule Replay, 50,000 | 0.0109 ms | 0.0315 ms | 0.0617 ms | 0.8327 ms | 0 |
| Déjà vu through Replay, 1,000 | 0.0167 ms | 0.0394 ms | 0.0672 ms | 0.3657 ms | 0 |
| through Re-evidence, 1,000 | 0.0470 ms | 0.1208 ms | 0.2267 ms | 166.9352 ms | outside gate |

The linked largest-observation and largest-cue 5,000-sample runs had
through-Replay maxima of 0.5938 ms and 0.6358 ms, with zero 1 ms violations.
Their through-Re-evidence maxima were 1.5398 ms and 2.5573 ms.

One earlier 5,000-sample focused run observed one 1.3532 ms wall-clock outlier
while its other 4,999 samples passed. The subsequent 20,000-sample run had zero
violations, but the observed outlier means an unconditional hard real-time claim
is still open on this general-purpose Linux host.

An initial cgroup-limited run exposed a restart-cold defect: one of 256 first
prefixes took 12.0775 ms and one later low-level exact call took 1.5181 ms. The
four-stage through-Replay sample itself had no violation, but the run was kept as
a failure. The runtime now reads only the allocated exact-address extents and
Replay capsule bytes during bounded startup preparation. It does not read sparse
holes and retains no whole-file Python copy.

After dropping the benchmark copy's file-cache pages, a second run applied
`rbps=625000000`, `wbps=625000000`, `memory.max=4294967296`, and no swap to the
exact benchmark process. It measured 50,000 random exact calls, 50,000 complete
Déjà vu-through-Replay calls, and 5,000 calls for each largest-record case:

| cgroup-limited post-repair sample | median | p99 | maximum | at least 1 ms |
| --- | ---: | ---: | ---: | ---: |
| first address per 256 prefixes | 0.0124 ms | 0.0586 ms | 0.1258 ms | 0 |
| exact capsule Replay, 50,000 | 0.0089 ms | 0.0472 ms | 0.0819 ms | 0 |
| Déjà vu through Replay, 50,000 | 0.0145 ms | 0.0533 ms | 0.1463 ms | 0 |

The largest-observation and largest-cue through-Replay maxima were 0.1473 ms
and 0.1944 ms. Process RSS was 1,064,554,496 bytes; the transient service's
cgroup memory peak, which also accounts for charged file cache, was 1.6 GB.
Re-evidence remained outside the named boundary and reached 23.4708 ms.

After the storage admission ceiling was corrected from 500 GiB to the literal
500,000,000,000-byte limit, the benchmark imported that same product constant
instead of carrying a second limit. Three fresh cgroup services repeated the
complete measurement on 2026-09-22. Together they executed 150,000 random exact
Replay calls, 150,000 complete Déjà vu-through-Replay calls, and 15,000 calls
for each largest-record case. All three reported the literal storage limit and
passed with zero 1 ms violations. The largest first-prefix, exact Replay,
Déjà vu-through-Replay, and largest-record-through-Replay values across the
three runs were 0.1372 ms, 0.0828 ms, 0.1759 ms, and 0.2413 ms respectively.
The raw JSON SHA-256 values are
`755ad985e608c8cd7104f58428c54a55a583d54284f0dab504befda8a83a3baa`,
`ac978aaf078d0973f7534b140606b9137fbedcbc9bb7fe080c07140f72d1e2c2`,
and `d76138ebb8da6db16b3e265f263a132a2fb9ab4f83401378affe0d5c1238e10f`.

A later whole-runtime import audit found that the external lock package loaded
Python's database module while initializing an unused lock class. VRS did not
open a database or create a database artifact, but the dependency was removed
instead of treating that distinction as sufficient. Main, session capture,
SessionEnd attachment, watcher exclusion, and read barriers now use the
package's small OS file lock. The complete standalone suite passed 125 tests.
The installed package then repeated the exact session hit and
main-after-complete-miss paths with all four stages while the database module
remained unloaded.

The cgroup benchmark was repeated after this change. It reported
`database_module_loaded=false`, the literal 500,000,000,000-byte ceiling,
zero 1 ms violations in 50,000 exact and 50,000 complete
Déjà vu-through-Replay samples, a 0.1572 ms through-Replay maximum, and a
704 MB cgroup memory peak. Its raw JSON SHA-256 is
`13a7a281c34a535cbde262ee40add52db0e61499bdd1333ec5e96855e7429b5a`.

The real-experience state above uses the first production address level. A
separate structural benchmark therefore forced one exact address through every
production level (powers 16, 18, 20, 22, and 23) before reaching its Replay
capsule. Under the same cgroup limits, 50,000 maximum-depth exact lookups had a
0.0101 ms median, 0.0144 ms p99, 0.0759 ms maximum, and zero 1 ms violations;
the first lookup was 0.0844 ms. The process peak was 42,479,616 bytes and the
database module remained unloaded. Raw result SHA-256:
`94b0e0582f9918aa07c97972e810f7c23c5985ff3508b7f4374d0d0cdac0ddd2`.

This structural run uses sealed empty lower levels to exercise maximum routing
depth. It is evidence for bounded level traversal only. It is not a billion
experience, high-occupancy collision, or parameter-scale measurement.

The directory keeps algorithmic work independent of total record count. The
current-experience linked-shard and actual 5 Gbit/s kernel-limited runs are
complete. A larger-scale run remains an evidence gate, and a general-purpose OS
measurement is not an unconditional hard real-time proof for every future host.

## Memory-bounded consolidation

Consolidation estimates each shard's transient graph and cue state before scheduling it. Available
memory is the 4 GiB limit minus current RSS and a 128 MiB reserve. Independent shards run in waves
that fit this amount. One shard receives all 16 workers; several smaller shards divide the same 16
workers within a wave. Completed shards commit and release their transient state before the next
wave. A shard that cannot fit by itself fails with `vrs_memory_budget_exceeded`; the implementation
does not remove regions, portals, evidence logic or original experiences to make it fit.

## Unresolved billion-parameter requirement

The repository's dormant recurrent cognitive-core defaults describe 709,560,320 trainable-style
parameters (708,902,912 block parameters, 655,360 state-embedding parameters and 2,048 final-norm
parameters). The live VRS experience store does not currently connect its records, cues, edges or
read-index slots to that parameter count. The user's "one billion VRS parameters in seconds" target
therefore remains unresolved and must not be claimed by substituting records, addresses, cues, edges,
tokens or bytes for parameters.
