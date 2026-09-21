# VRS 2.2 resource and read-index boundaries (2026-09-21)

This document describes the repaired `vrs-regions` lineage. Earlier 60,000-record guidance treated
manual project bundles and lexical warm views as the scaling mechanism. The current resident instead
creates storage shards automatically and keeps every shard as a complete VRS main.

## Hard limits

| resource | enforced limit |
| --- | ---: |
| resident process RSS | 4 GiB |
| SSD transfer assumption | at most 5 Gbit/s (625 MB/s) |
| logical state storage | 500 GB |
| consolidation workers | 16 |
| default records before opening the next automatic shard | 8,192 |

The record count is a storage boundary, not an experience-quality boundary. An original episode is
never divided. Its revisions stay with the shard that owns its source lineage. The 500 GB admission
check counts logical file sizes, including sparse index capacity, so sparse allocation cannot hide a
limit violation. The one-second storage scan cache reserves the full 625 MB that a 5 Gbit/s device
could add during that interval and forces a fresh scan near the boundary.

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
references and source addresses. Their derived cue block is stored in the same checksummed capsule
but decoded when Re-evidence consumes it. This keeps the named Replay boundary from materializing
thousands of derived strings that the next stage owns.

## Current-experience-copy measurement

Measured on a consistent read-only copy of the active session experience on 2026-09-21:

| quantity | result |
| --- | ---: |
| unique experiences | 12,537 |
| cue occurrences | 1,324,260 |
| load existing VRS generation | 1.075 s |
| build exact, source and cue read directories | 7.173 s |
| load plus complete backfill | 8.247 s |
| process peak RSS | 1.007 GB |
| read-directory logical bytes | 2.004 GB |
| read-directory allocated bytes | 494 MB |

Replay timing used CPU time so scheduler descheduling was not hidden inside the component result:

| sample | median | p95 | p99 | maximum | at least 1 ms |
| --- | ---: | ---: | ---: | ---: | ---: |
| every experience once, first pass (12,537) | 0.024 ms | 0.091 ms | 0.285 ms | 0.530 ms | 0 |
| random Replay (50,000) | 0.022 ms | 0.081 ms | 0.277 ms | 0.502 ms | 0 |
| largest capsule Replay (5,000; 60,000 characters, 8,362 cues) | 0.253 ms | 0.311 ms | 0.369 ms | 0.496 ms | 0 |

Consuming all 8,362 cues for the largest capsule belongs to Re-evidence preparation and measured
0.885 ms median, 1.061 ms p99 and 1.523 ms maximum together with Replay. It is reported separately;
it is not deleted from the system or folded into the Replay number.

These results establish the `<1 ms through Replay` boundary for this current real experience copy
and these samples. They do not prove a hard real-time bound for every device, cold page-cache state,
future corpus or one-billion-parameter VRS. The disk directory keeps lookup work independent of the
total record count by using a small number of sealed levels, but larger-scale and 5 Gbit/s constrained
measurements remain required.

## SessionEnd linked-shard measurement

An active session is itself a complete sharded VRS. SessionEnd never exports its
observations into another SQLite database. Main atomically records each complete
session component in `linked-shards.json` and owns those original databases in
place. A 15,630-experience copy measured as follows:

| operation | result |
| --- | ---: |
| validate complete session VRS and attach registry | 1.059 s |
| open empty primary plus linked registry | 0.009 s |
| build exact, source and cue directories | 8.832 s |
| build complete VRS read projection | 2.170 s |
| end-to-end benchmark wall time including 50,000 Replay samples | 17.53 s |
| process peak RSS | 1.848 GB |

The attached main primary contained zero copied experiences and reported 15,630
logical experiences from the linked VRS. Fifty thousand random exact-address
lookups through Replay measured 0.032 ms median, 0.358 ms p99 and 0.609 ms
maximum, with zero samples at or above 1 ms. The previous observation re-ingest
path took 31.189 s for merge alone on the same 15,630-experience source; that
path has been removed.

Session components are registered in one atomic update, including every
automatic child shard. Linked source lineages remain main-owned: later revisions
stay with their original shard, update the registry generation atomically, and
recover from a committed journal after an unclean process exit by replaying that
VRS journal. This recovery does not export or re-ingest observations.

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
