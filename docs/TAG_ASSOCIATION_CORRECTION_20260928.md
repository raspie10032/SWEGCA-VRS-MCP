# Verified bundle to tag association correction

Scope: the user-confirmed image/tag/DINO bundles supply evidence for tag
associations. A missing tag is not an explicit refutation. This change does not
claim image identity, tag semantic accuracy, or causal World authority.

`tools/tag_associations.cpp` preserves original feature records and native
image/feature/score binding references. It authenticates the image digest and
reads the processing record digests. The verified pairing premise comes from
the user; hashing does not independently prove model output correctness.

`association_kernel.hpp` observes actual tag membership in those bundles and
judges the shuffled witnesses: support only accepts, explicit refutation only
rejects, absent or conflicting evidence abstains. The existing strength factors
are 1.01 / 0.995 / 1. The previous stored edge strength is read, not reset.

This is a new, explicitly typed local association decision path. The general
four-axis `judge_evidence` policy remains unchanged. `AssociationJudgment`
cannot be converted to its `EvidenceJudgment`. Local association support must
not be reported as passing that general policy.

The runner collects candidates from observed co-occurrence. Thus an all-accept
result is expected for valid bundles without explicit refutations; it is not
an independent semantic accuracy measurement. Rejection and conflict branches
are exercised by synthetic unit fixtures, not by this dataset. DINO vectors
and image originals remain part of the referenced bundles; this run does not
judge vector similarity or cross-image identity.

Validation: association tests 20 checks; original core tests 3,974 checks,
zero core hot-path allocations. Fixed judgment plus strength microbenchmark:
2.568 ns/op, excluding membership scan, input hashing, storage and graph work.

Full dataset run: `/var/home/raspie/Documents/Codex/vrs-tag-links-20260928-02`.
6,274 bundles, 5,441 tags, 793,281 candidate pairs; 793,281 accept, zero reject
or abstain; all previous strengths 1 become 1.01. Runtime 24.3266 seconds.
All 6,274 bundles remain in the graph, with 18,822 original member references,
290,285 membership edges and zero isolated bundles. No Main merge or running
service replacement was performed.
