# Individual experience-pair verification

User-defined proposition: **when two experiences collide, does a common
 denominator exist?** Each pair receives its own SWEGCA verdict. Evidence may
 come from the complete incoming input set, not only from that pair or an
 already-processed VRS state.

## Active implementation

- `for_each_experience_pair` shuffles the experience schedule and streams every
  distinct pair exactly once. Pairs with no common observation are included;
  self-pairs are excluded. Canonical endpoint identity is stable across seeds.
- `tools/tag_associations.cpp` now uses **experience IDs**, not tag IDs, as link
  endpoints. The historical executable name is retained; the schema is now
  `experience_pairs_common_member_v1`.
- Current concrete common-denominator observations are equal model-qualified
  tag members. Image/feature/DINO originals remain bound and preserved. No
  visual-similarity threshold or arbitrary general semantic predicate is added.
- Every incoming experience contributes addressable membership observations to
  the shared evidence index. Its native tag record names source and witness
  IDs; those IDs resolve to native source/binding/feature addresses.
- For EACH pair, `observe_common_member` checks the two actual bound member
  sets. The pair's common members select witnesses from the entire input
  index. A third experience can corroborate a common member, but cannot make
  a member occur in an endpoint that did not contain it.
- The index caches observations, not pair verdicts. Each pair constructs a new
  `AssociationEvidence` and calls `judge_association`, followed by the existing
  1.01 / 0.995 / 1 strength projection. Native pair records preserve common
  members and the number of supporting observations outside the endpoints.
- Absence of a common recorded member is unresolved, not fabricated negative
  evidence. This tag observation path has no explicit negative observation
  source; it therefore does not manufacture rejections. Counts spanning
  several tags are witness occurrences, not independent-sample probabilities.
- Prior strengths are loaded by canonical experience-pair identity. Changing
  the shuffle seed does not reset or misassign them. Writes use 17 significant
  digits instead of the earlier lossy default precision.
- The image consumer follows core-approved **experience-to-experience** links
  and emits the reached experience's tags with `via_experience` provenance.
  It no longer interprets those endpoints as tag IDs.

## Running

```
make build/swegca-tag-associations build/swegca-image-associations
build/swegca-tag-associations DATASET OUTPUT BINDING_RUN PREVIOUS_PAIR_RUN_OR_MINUS SHUFFLE_SEED
```

Pass `-` for the first experience-pair graph (initial strength 1). The old
 tag-to-tag graph cannot supply experience-pair strengths: it is explicitly
 rejected, not silently reinterpreted or migrated. Previous artifacts are
 preserved. Subsequent runs accept only matching experience identity/order and
 member observations. Each invocation takes an explicit shuffle seed.

No stopped real-data multi-pass run was resumed. No Main merge, live-service
 change or new long-term goal was performed.

## Verification boundary

- Pair scheduler test: all six pairs of four inputs, whole-input visibility,
  independent accept/reject/abstain fixture results, different shuffle coverage,
  no self-pairs and exception propagation.
- Core observation tests: 25 checks passed, including common/member absence and
  invalid bindings. Fixed judgment/strength microbenchmark 2.360 ns/op; not a
  full pair or retrieval latency measurement.
- Synthetic native-storage integration: A={red,square}, B={red,circle},
  C={red,triangle}, D={blue,fish}. All six experience pairs were checked:
  three approved and three abstained. A-B has three red witnesses (A,B,C),
  proving that the third experience supplies evidence.
- A second small fixture pass with a different shuffle seed retained pair
  identity: approved pairs 1.01 -> 1.0201; abstained pairs stayed 1.
- Native graph recall from A produced circle via B and triangle via C, not D's
  unrelated tags. The descriptor in this test is synthetic, not an image-quality
  measurement.

This fixes the verification unit and evidence access. It does not establish
 convergence or image association quality. Shared generic tags can still make
 many real experience pairs approve; no undocumented rarity filter was added.

## Identical experiences count once (user's invariant)

The ingestion identity covers the actual image payload digest, the complete
feature-record bytes and the raw-score payload digest. Feature-record bytes
are compared after the identity lookup; inputs are not merged by image hash
alone or by similarity. This is exact payload identity, not semantic
near-duplicate detection. JSON content/ordering and recorded metadata are not
silently stripped or rewritten to manufacture equality.

An exact repeated delivery resolves to its first canonical experience ID.
It adds no vertex, pair, witness, or additional refinement inside that pass.
Every delivery receipt, including repeats, is sealed as a native delivery
record and listed in aliases.jsonl. Thus multiplicity of delivery preserves
provenance without becoming multiplicity of evidence. Intentionally starting
another VRS refinement pass is a separate operation from duplicate delivery.

The consumer now scans canonical sealed feature records rather than the input
JSONL occurrence stream. It therefore neither double counts duplicate inputs
nor mistakes raw row numbers for canonical experience IDs.

Integration results:

- Original fixture: four experiences, six pairs, three approvals, three abstentions.
- Add an exact repeated experience with a different delivery receipt: five
  deliveries, still four experiences/six pairs; the entire link CSV values
  remain identical, including support counts and strengths. Receipt preserved.
- Same image, changed feature payload: five distinct experiences/ten pairs;
  not collapsed into the old experience.
- Recall on the duplicate-containing fixture still uses four canonical
  experiences and returns only the two expected additional tags.
