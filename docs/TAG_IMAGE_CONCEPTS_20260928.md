# All recorded tags × all images; common concept nodes

## Authority and verification modes

The user specified a binary predicate for this operation: a tag matches an
image experience or it does not. SWEGCA makes the final decision. The adapter
must supply the appropriate measured outcome; it must not choose a verdict.

- Binary tag-image observation: `observe_tag_image_match` returns support for
  a matching recorded tag and refute for a nonmatch. Invalid source bindings
  fail this batch rather than being counted as normal abstentions.
- Existing `judge_association` makes the verdict; existing
  `revise_association_strength` applies ×1.01 or ×0.995. Their decision and
  strength rules were not replaced by a Python/GPU verdict.
- Ternary observation paths retain insufficient/conflicting evidence and
  abstention. `observe_recorded_member` and `observe_common_member` are not
  globally changed into negative-evidence generators.
- Output schemas/mode labels distinguish `binary_tag_image_v2` (binary) and
  `experience_pairs_common_member_v1` (ternary). A caller must choose the
  predicate contract, not switch modes to obtain a preferred result.

An initial implementation wrongly reused the generic insufficient-on-absence
observation. SWEGCA consequently abstained. That run is explicitly invalidated
at `vrs-tag-image-gpu-20260928/INVALIDATED.md`. The corrected run reused the valid
GPU observation masks and recomputed core decisions from initial strength 1;
this is a replacement of the erroneous pass, not an additional reinforcement.

## Implementation

`tools/tag_image_refinement.cpp prepare DATASET PAIR_GRAPH OUTPUT` reads the
canonical native experience records, verifies actual image/processing payload
bindings and exports their recorded tag member IDs. Identical experiences have
already been reduced to one canonical source; duplicate delivery receipts do
not become extra comparisons or supporting images.

`tools/tag_image_gpu.py OUTPUT GPU_ID` is an observation-only adapter using the
already installed torch runtime. GPU 0 processes tag IDs [0,2720), GPU 1
[2720,5441). Each tag is compared against all 6,274 images in batches of 32.
There are no new tag thresholds, downloaded packages or model weights. These
are comparisons of recorded, verified image/tag observations, not new raw-pixel
classification or independent proof that the tagging model is semantically
correct. Neither GPU assigns approve/reject/abstain.

`apply` authenticates the two input/output partitions, cross-checks **every**
GPU observation against the original recorded members, invokes SWEGCA for every
pair, and seals exact binary64 strengths. Matrix v2 layout: three little-endian
uint64 header values (tag ID, canonical source count, version=2), followed by
one (uint64 status, binary64 strength) per canonical source ID.

Every tag gets a node retaining its original tag address and verification
matrix address. Approved source IDs form its outgoing experience references.
More than one distinct image makes it a `common_concept`; one is a
`single_observation`; no approved image is unresolved. This is structural
sharing, not an inferred same-character identity or a rarity cutoff. Original
experiences, image-image links and provenance are retained.

`inspect OUTPUT TAG_ID` retrieves a concept node through its authenticated
native record, not from a log. No Main merge or live consumer replacement was
performed; the pre-existing image recall executable does not automatically
switch to this new concept graph.

## Corrected real-data result

Artifacts: `/var/home/raspie/Documents/Codex/vrs-tag-image-gpu-20260928-corrected`.

- Recorded tags: 5,441; canonical images: 6,274; comparisons: **34,136,834**.
- SWEGCA approvals: **290,285**, stored strength **1.01**.
- SWEGCA rejections: **33,846,549**, stored strength **0.995**.
- Abstentions: **0**.
- Common concept nodes: **3,829**; single-image nodes: **1,612**.
- `1girl`: native concept node 0, 6,256 distinct images; verified native read.
- RTX 4070 Ti SUPER: 17,065,280 observations, worker processing ~2.510s.
- RTX 5060 Ti: 17,071,554 observations, worker processing ~2.489s.
- Worker processing windows overlap ~2.489s. CUDA interval measurements include
  the batch transfers/writing between launches; they are not isolated kernel
  timings. GPU process runtimes including startup were ~3.65s and ~3.64s.
- Input preparation 7.87s; corrected core application/persistence 16.86s.
  These stage durations exclude development/verification gaps and are not a
  claimed resident latency or measured GPU-vs-CPU speedup.
- GPU worker host RSS peaks: 1,020,996 / 1,017,464 KiB; application RSS 11,656
  KiB. Observed tensor allocation peak ~23.9 MB per GPU, excluding CUDA context
  and other applications. CPU affinity 6,7 leaves other CPUs available.

Independent native-file verification authenticated all 5,441 matrices and
checked all 34,136,834 stored statuses/strengths against source memberships;
all concept memberships and distinct-image counts matched. Small fixture tests
also check binary rejection, common-concept native retrieval and that an
invalid original fails rather than silently abstaining. Core observation tests:
28 passed. No convergence or unseen-image quality improvement is claimed here.

## Deferred by the user

Selection between binary and ternary verification can depend on data format,
proposition and available evidence. General selection/adaptation logic is
explicitly deferred. The current binary contract applies only to the user's
specified tag-image predicate; it is not a universal rule that all image data
must receive binary decisions. No automatic mode-selection system or long-term
goal was added.
