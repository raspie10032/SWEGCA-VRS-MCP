# Common-concept growth observations

User contract: count all three outcomes over the same VRS round/window:

- N = delta_accept + delta_reject + delta_abstain
- fractions = (delta_accept/N, delta_reject/N, delta_abstain/N)
- N=0 means no new observation; JSON fractions are null.
- No wall-clock denominator: CPU/GPU throughput does not change the ratios.
- Retain raw integer deltas and window identity for reproducibility.

Experience validity is a separate concept. These observations never invalidate,
delete, quarantine, exclude or approve an original experience. The kernel's
`invalid_counts` only reports a decreasing/overflowing count window. It does
not classify the underlying experience. No eligibility rule is changed here.

`ConceptGrowth` records distinct original experience identities, not delivery
counts or block IDs. Repeated verification contributes its real outcomes but
not another distinct experience. Merging block observations sums integer
counts before division and unions identities; it never averages percentages.
The producer must provide disjoint verification events when merging blocks;
this collector cannot detect overlapping events from aggregate deltas alone.

For each concept report:

- `distinct_population`: unique original experiences in the current input scope.
- `distinct_tested`: unique experiences with at least one decision in the window.
- `distinct_supported`: unique experiences with at least one acceptance in it.
- `tested_fraction` and `supported_fraction`: divided by distinct_population.

Support coverage is an observed acceptance footprint, not a new truth score.
It does not replace the retained reject/abstain fractions. Input membership and
relation-pair verdicts must not be silently mixed into one denominator.

The existing input-collision runner now emits `concept-growth.jsonl` each round
for direct tag/input membership only, using before/after integer counters and
original image identities. The reusable collector has no image-specific type.

No numerical promotion threshold has been specified or invented. Reports carry
`promotion_decision: null`. Common-concept promotion/traversal decisions must
remain SWEGCA decisions; collecting the features is not proof that promotion or
hub-pollution suppression is already implemented. In particular, a common tag
such as 1girl remains a valid observation and a usable explicit query key. Its
commonness alone must not fabricate relatedness between otherwise unrelated
experiences.

## User clarification: reduce connection cost

Promotion must do useful representational work, not only attach a label:
consolidate redundant common-concept connections while retaining original
experience identity, evidence provenance and independently supported relations.
Its purpose includes both limiting unrelated-experience traversal through common
hubs and reducing actual connection/storage cost. Experience validity remains
independent throughout.

A completed promotion receipt must record actual connection counts and storage
bytes before/after, plus references to the shared concept and retained exceptions.
Estimated savings or candidate statistics are not a completed reduction receipt.
No edges have been removed or consolidated by the growth statistics code.
