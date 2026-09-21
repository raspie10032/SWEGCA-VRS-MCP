# VRS 2.2 evidence-input bottleneck — 2026-09-22

This source change keeps the SWEGCA evidence accumulator, decisions, graph,
regions, portals, shared experience, and numerical VRS kernel intact. Evidence
construction and usage accounting read the original episode's steps, metadata
and source address. They now use the existing `episode_light` representation,
which carries those fields without expanding every cue string. The full
episode path remains available for operations that actually need cues.

The fixed offline corpus is a native copy of 15,630 existing experiences:
71 regions, 147,356 nodes and 3,312,964 flat edges. Every record in this copy
has outcome `pending`; none has an explicit evidence polarity, so the
accumulator correctly records zero resolved observations. The change does not
infer an outcome from that fact.

| Measurement | Previous source | Light episode source |
| --- | ---: | ---: |
| Evidence construction, one cold pass | 1.194 s | 0.662 s |
| Full consolidation, 16 workers, repeat 1 | 3.446 s | 2.537 s |
| Full consolidation, 16 workers, repeat 2 | 3.613 s | 2.581 s |
| Full consolidation, 16 workers, repeat 3 | 3.607 s | 2.555 s |

All measured consolidation outputs have the same version ID,
`ee5922c418667dd0bf4cc6b4590a388becb66bad7ed18ebeb849072230e263ea`.
The new within-run one-worker/16-worker wall speedups were 1.644, 1.658 and
1.675 times. The first new run was retained in the command trace; repeats 2
and 3 are saved as
`evals/vrs22_context/results/consolidation16/light_evidence_2.json` and
`light_evidence_3.json` (SHA-256
`fa3835baf8e62f31230660870870603340aa46c0870cdf75df74e018c13bd951`
and `4c909ba208bb17e5ca28bfce87b4256476e8f280ec8077f333e50b627ed83830`).
The earlier three comparison runs are the `parallel_1.json` through
`parallel_3.json` files in that same directory.

The full benchmark ran with 16 requested region workers, BLAS/OMP/MKL thread
counts fixed at one, `memory.max=4,294,967,296`, swap disabled, and read/write
`io.max=625,000,000 bytes/s` on the physical SSD (`259:3`). The two saved new
runs report cgroup memory peaks of 1,018,802,176 and 1,019,179,008 bytes;
the native state occupies 85,905,408 allocated bytes. No SQLite module loaded.

A behavioral regression compares the full and light original episode views
across outcomes, pending explicit polarity, revision supersession, source
metadata, conflicting evidence and usage-driven association. It compares the
evidence objects and every numerical `build_inputs` field. The targeted test
and 37 related standalone tests passed. The complete standalone suite passed
**126 tests in 182.62 seconds** with the project environment and one BLAS/OMP/MKL
thread.

These are measurements on 15,630 records. They do not establish a one-billion
VRS-parameter processing bound or a universal <1 ms Replay bound; the user's
definition of a VRS parameter is not supplied, so records are not substituted
for it.
