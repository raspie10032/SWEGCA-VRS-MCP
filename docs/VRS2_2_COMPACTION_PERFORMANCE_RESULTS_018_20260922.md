# VRS 2.2 compaction performance results 018

## Decision

All three 100k plus VRS cells are valid. Each completed 12 actual source-thread
compactions, exact session-first VRS recovery at every boundary, a recovery
turn, a coding turn, SessionEnd attachment, and external executable grading.
The frozen v005 100k and 250k plain cells are reused as the controls.

The headline comparison uses the first ten boundaries in every arm because the
frozen plain controls contain ten. Full 12-boundary VRS scores are reported
separately.

## Test-surface correction

The v018 VRS runtime, live capture, recall, compaction, and model conversations
used the native repaired package. A later whole-surface audit found that the
separate coding-task fixture still came from commit
`7c67f30954d259083223310ada32e0d599e7bd51`; sibling storage and regression
test files in that fixture used the retired database implementation. The three
models only changed `src/swegca_vrs2/checkpoint.py`, so the purpose-retention,
dialogue, token, compaction, and VRS-protocol results do not depend on that
storage implementation. The original coding regression result is retained as
historical provenance rather than described as a clean native test surface.

The same audit found a second provenance issue: the native VRS code never
called a database, and every v018 state contained zero database artifacts, but
its external file-lock package imported Python's database module while
initializing an unused lock class. This did not participate in capture, recall,
compaction, scoring, or the coding change. It means v018 is valid behavioral
evidence, but it is not evidence for the stricter claim that the process never
loaded that module.

The exact final target file from each model was replayed in an in-memory,
data-only checkpoint fixture with no storage backend or agent adapter. The
fixture verified all three required behaviors: duplicate `metadata.json`
rejection before member reads, duplicate array rejection before member reads,
and acceptance of a valid archive with an unrelated extra member. The
unmodified baseline failed both duplicate checks; Luna, Terra, and Sol passed
all checks.

- validator SHA-256:
  `c6a9ca68720869685913e7f723a97454883dc7c4dce0bee6f8bedd3d0f1733f4`
- result SHA-256:
  `b433128278ce1faad72026c785839189022d7295b1184289fe0264772152acc6`
- candidate target SHA-256: Luna/Terra
  `ef6be9cc0a8f009dda28ec900d83cdc7031bce430a13ad387afc6bc38b2df43e`;
  Sol `368726a182ab96ce779bb56c41c7c145677ed00d168897d636cf8c5c7d841c5e`

The same clean fixture replayed all six frozen plain outputs. Terra 100k plain
passed all three behaviors; the other five retained the unmodified baseline
and failed both duplicate checks. This independently reproduces the executable
success pattern and 20-versus-4 code scores in the comparison table. The plain
replay result SHA-256 is
`9d1e39bbc7ba7d10bfe3970f17bf5ddc28660e3f2cc21f9477dc785bed1c7b36`.

## Headline comparison

| model | arm | goal consistency, first 10 | dialogue context, first 10 | code quality | executable success |
| --- | --- | ---: | ---: | ---: | --- |
| Luna | 100k plain | 15.00 | 35.00 | 4/20 | no |
| Luna | 250k plain | 9.00 | 32.50 | 4/20 | no |
| Luna | 100k + VRS | **91.00** | **70.00** | **20/20** | **yes** |
| Terra | 100k plain | 91.00 | 75.00 | 20/20 | yes |
| Terra | 250k plain | 43.00 | 45.00 | 4/20 | no |
| Terra | 100k + VRS | **99.00** | **98.75** | **20/20** | **yes** |
| Sol | 100k plain | 27.00 | 43.75 | 4/20 | no |
| Sol | 250k plain | 48.00 | 50.00 | 4/20 | no |
| Sol | 100k + VRS | **100.00** | **100.00** | **20/20** | **yes** |

All initial task hashes match across the three arms for each model.

## Complete 12-boundary VRS result

| model | goal mean | goal worst | first goal loss | dialogue mean | dialogue worst | first dialogue loss | contradictions | reasks |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Luna | 92.50 | 60.00 | boundary 3 | 70.83 | 37.50 | boundary 3 | 0 | 0 |
| Terra | 99.17 | 90.00 | boundary 9 | 98.96 | 87.50 | boundary 9 | 0 | 0 |
| Sol | 100.00 | 100.00 | none | 100.00 | 100.00 | none | 0 | 0 |

Luna lost part of the duplicate-before-read wording, the unrelated-extra
allowance, and part of the preserved-check list at boundary 3. The fields later
reappeared from VRS, so the temporal-stability rubric records nine unsupported
reappearances. There were no contradictions, filler substitutions, reasks, or
unavailable answers. Terra omitted part of the scope and preserved-check list
once at boundary 9. Sol retained every scored field at all 12 boundaries.

## Coding result

Every VRS model changed only `src/swegca_vrs2/checkpoint.py`. Every produced a
small duplicate-member guard before the first archive member read and preserved
unique unrelated extra members. External grading reran:

- duplicate `metadata.json` hidden test: passed
- duplicate array-member hidden tests: passed
- existing checkpoint regression tests: passed
- unsafe extraction or unpickling scan: passed
- target-only scope check: passed

All three VRS cells scored 20/20 and achieved strict executable success. Among
the reused plain controls, only Terra 100k achieved the same result; the other
five controls scored 4/20 and failed strict executable success.

## Provider token use

`input` includes cached input. `uncached input` is `input - cached input - cache
write input`. Cache-write input was zero in every shown cell. Prices are API
price equivalents from the frozen evaluator rates, not an account billing
statement.

| model | arm | provider calls | input | cached input | uncached input | output | reasoning output | compaction input | wall seconds | price equivalent USD |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Luna | 100k plain | 88 | 6,962,050 | 5,226,496 | 1,735,554 | 23,766 | 4,752 | 1,350,928 | 655.37 | 0.4802 |
| Luna | 250k plain | 87 | 11,679,950 | 8,751,360 | 2,928,590 | 14,505 | 5,772 | 2,261,121 | 524.77 | 0.7782 |
| Luna | 100k + VRS | 119 | 9,236,895 | 7,192,832 | 2,044,063 | 20,190 | 3,962 | 1,183,110 | 750.49 | 0.5769 |
| Terra | 100k plain | 55 | 4,347,134 | 2,961,664 | 1,385,470 | 9,305 | 1,756 | 910,959 | 498.20 | 3.4749 |
| Terra | 250k plain | 81 | 10,945,683 | 7,993,088 | 2,952,595 | 14,424 | 6,041 | 2,260,807 | 807.83 | 7.6769 |
| Terra | 100k + VRS | 112 | 8,719,885 | 6,795,264 | 1,924,621 | 15,120 | 2,618 | 1,093,285 | 531.38 | 5.3897 |
| Sol | 100k plain | 63 | 4,928,102 | 3,345,792 | 1,582,310 | 8,997 | 2,847 | 1,092,929 | 345.49 | 7.8475 |
| Sol | 250k plain | 78 | 10,504,669 | 7,658,880 | 2,845,789 | 8,407 | 2,665 | 2,260,334 | 367.24 | 14.6148 |
| Sol | 100k + VRS | 120 | 9,376,035 | 7,279,872 | 2,096,163 | 17,961 | 3,189 | 1,183,016 | 929.40 | 11.6558 |

VRS used more provider calls because every compaction boundary has a separate
exact-address recovery turn and the evaluation includes recovery and coding.
Its total input remained below the 250k plain control for all three models.

## Native VRS protocol and lifecycle

| model | source-thread compactions | all provider compactions | exact boundary protocols | original experiences | VRS state transitions | journal rows | linked experiences | DB artifacts |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Luna | 12 | 13 | 12/12 | 908 | 11 | 919 | 908 | 0 |
| Terra | 12 | 12 | 12/12 | 869 | 11 | 880 | 869 | 0 |
| Sol | 12 | 13 | 12/12 | 926 | 10 | 936 | 926 | 0 |

Every boundary protocol passed the full-address, pinned-pair, four-stage,
session-first, no-main-fallback, and matching-release checks. Luna boundary 8
also used one `memory_read_path` inside the same pinned view to read its exact
Replay record. Recovery and coding protocols passed for all models.

During every active session, main observation and linked counts stayed zero.
After SessionEnd, original session experiences were retained and attached in
place. The journal totals above include VRS consolidation state transitions;
those are classified separately and remain in each linked shard's latest pair.

## Runtime and resource evidence

- product/runtime commit: `9d18bd06915eb2caff1d6d55c1761bf2fdd26fa1`
- runtime wheel SHA-256:
  `1955e20ac08d16b232d32f82dad445d0838d001bd37c97d05d52a98e09faf892`
- final invocation: `15245a20da1f46f1a9f94f402293a4b7`
- service result: success, exit status 0, restart count 0
- service wall time: 2,325.116 seconds
- service CPU time: 975.226 seconds
- memory cgroup: 4 GiB, zero swap; no observed OOM or OOM kill while active
- CPU quota: 1,600 percent
- kernel NVMe read/write limit: 625,000,000 bytes per second each
- measured artifact state admission constant: 536,870,912,000 allocated bytes
  (the literal 500 GB requirement audit below found and corrected this 500 GiB value)
- largest completed evaluation VRS state: 5,017,600 allocated bytes (Luna)
- raw stderr files with content: 0
- raw results SHA-256:
  `41361e9efcd6901ac0a8d08fdebc14798ee950e3595f2a38f450149bc97f262a`
- graded summary SHA-256:
  `5faaed5b0a2da3cf8bc471b56925d1e241b8bf34e7c67f7aa313c09a4b1d4e90`

## Post-run exact storage-cap correction

The completed v018 result remains bound to commit `9d18bd0` and wheel
`1955e20a`; preserving those identifiers is required for reproducibility. A
whole-stack audit after the run found that this measured artifact interpreted
the requested 500 GB ceiling as 500 GiB. The evaluation states did not approach
either limit: their allocated sizes were 5,017,600 bytes (Luna), 4,763,648
bytes (Terra), and 4,902,912 bytes (Sol), so this unit error did not alter the
reported model scores or lifecycle result.

Product commit `50f6f721efdaeaf36ffddb6cf03c0a77098ea24d` corrects the admission
ceiling to exactly `500,000,000,000` allocated bytes. Its rebuilt wheel SHA-256
is `0beec3a54ed845535c10407b1bfcd36d4fc703ed64ad1244d1bec32c380fb2b1`.
The full product suite passed 121 tests after that correction.

The same whole-stack audit then found that the generated Codex hook resolved a
virtual-environment Python symlink to the base interpreter, which cannot import
the installed package. Product commit
`50585aafb97737bed513c797491046c85831c75e` preserves the venv executable
path. The suite passed 122 tests, its installed wheel SHA-256 is
`dcddd29b34342b8e807fbfe77a612e5c6bdd59e6201a2f519389c9c607c06acd`,
and a generated hook successfully imported and ran through that exact path.
The latest installed wheel also passed an isolated exact-address smoke covering
all four stages, session-first lookup without opening main, and main fallback
only after a complete session miss.

Product commit `06dde24` then removed the benchmark's duplicated 500 GiB
constant and imported the product's exact 500,000,000,000-byte ceiling. Three
new cgroup-limited services each ran 50,000 random exact Replay calls, 50,000
complete Déjà vu-through-Replay calls, and 5,000 calls for each largest-record
case. All three passed with zero 1 ms violations. Across the repetitions, the
largest Déjà vu-through-Replay value was 0.1759 ms and the largest-record
through-Replay value was 0.2413 ms.

Product commit `18401a0` removed the external lock dependency and replaced
its used exclusive-file-lock behavior with a native OS lock. The full
standalone suite passed 125 tests, including competing owners, reentrant
acquisition, release by the background closer, session capture, SessionEnd
attachment, batches, and shards. The installed package had no external lock
package, loaded no database module during exact session and main fallback
smokes, and created no database artifacts. A new 4 GiB and 5 Gbit/s
cgroup-limited run reported the database module unloaded and zero 1 ms
violations; its complete Déjà vu-through-Replay maximum was 0.1572 ms.
Release-audit commit `65ec52d` also makes the archive verifier reject the
retired lock import, agent adapter imports or names, database imports or
artifacts, and direct forbidden runtime dependencies. The final wheel SHA-256
is `e0144305a8fa863f52679b7a4aa1e01901fa23942314e364d36c40c302a656c9`;
all 48 installed package source files are byte-identical to that wheel.

A separate maximum-depth structural benchmark traversed all five production
address levels and performed 50,000 exact Replay lookups. Its maximum was
0.0759 ms with zero 1 ms violations. The benchmark explicitly reports that it
uses synthetic sealed lower levels and makes no experience-scale or
parameter-scale claim.

These are post-run product corrections, not retroactive changes to the
frozen v018 measurement artifact.

## Limits of this result

This is one measured cell per model and arm, so it establishes the observed
outcome rather than statistical variance across repeated model samples. The
plain controls are the frozen v005 runs with ten compactions; equal-boundary
comparisons therefore use the first ten VRS boundaries.

The product's current-experience Replay benchmark and 5 Gbit/s kernel-limited
benchmark are separate from this compaction evaluation. The phrase “one billion
VRS parameters” still has no defined mapping from experiences, cues, edges,
slots, or bytes to parameters. This evaluation does not claim that unresolved
scale target.
