# SWEGCA numerical source boundary for the rebuild

This document separates author SWEGCA rules from behavior introduced by the
old VRS 2.2 product. It is a source comparison, not a C++ implementation,
parity result, or performance claim. The earlier draft that treated VRS 2.2
constants as author requirements is preserved outside this worktree in the
agent bridge's rejected draft directory.

## Author rules to reproduce

| Rule | Author source | Required comparison |
| --- | --- | --- |
| Observation `Main.ingest` validates and appends to HotIndex, then Graph | VRS-MCP product `store.py` at `7536139`, `HotIndex.append` and `Main.ingest`; Claude rules (b) | Request ID, fingerprint, cue postings, revision and pair chain, including exact error behavior. |
| `Graph.append` creates record and cue nodes, record direct signal 0.1, bidirectional new edges with sign 1 and base strength 0.5 | VRS-MCP product `store.py` at `7536139`; Claude rules (d) §1 | Node and edge order, float32 bits, result snapshot. A record's signal means existence, not truth; polarity is not inserted as a new edge sign. |
| Event settlement reads directed dependencies and keeps pending work when the round budget runs out | `mosaic_vrs_event_signal.py` and `mosaic_vrs_event_delta.py` at `tinylm` `3bddcb7`; Claude rules (d) §§4–5 | Exact pending set, edge binding, score update and float32 order. Pending is never relabeled convergence. |
| Connectivity regions use weighted modularity and overlapping membership coefficients from positive neighbor mass, normalized with `fsum`; shared original is an original with multiple memberships | `mosaic_vrs_connectivity_regions.py` at `tinylm` `3bddcb7`; Claude rules (d) §6 | Region labels, coefficient sum, generation binding and shared-original identity. No added membership floor. |
| Region preactivation, coactivation witnesses, decayed portal planning and one local navigation page precede Recall | `mosaic_vrs_portal_activation.py`, `mosaic_vrs_coactivation_navigation.py`, `mosaic_vrs_portal_lifecycle.py`, `mosaic_vrs_local_navigation.py` at `tinylm` `3bddcb7`; Claude rules (e) | One eligible portal and one page, deferred regions, witness lineage, source generation identity. No transitive BFS claim. |
| SWEGCA evidence accumulator groups source, context and evidence axes before any judgment | `SWEGCA-Architecture` `5901a5a` `mosaic_evidence_accumulator.py`, and `tinylm` `3bddcb7` variant; Claude rules (f) §§1–6 | Provenance, conflict, abstention, distinct source and context, no retrieval-rank authority. Variant choice requires exact source review. |
| `refine_vrs` shuffles edges, updates state in ordered batches, reinforces or decays strengths, clamps by base, and computes stability | `tinylm` `9ec73bd` `tools/organize_rozephine_mixed_experience_connections_hybrid.py:793–906`; Claude rules (f) §7 | Explicit PRNG and shuffle version, batch order, float32 state and strength, stability. Seed, cycles, passes and batch size are parameters, not fixed author constants. |

## Old product behavior that is not an author rule

The old `vrs_evidence.py` sufficiency weighted direct formula, `vrs_refine.py`
fine-region recursion and connector phase, thresholds `FINE_MIN_NODES=400`,
`MEMBER_FLOOR=0.05`, `SHARED_FLOOR=0.2`, portal key summary limit 8, default
seed 1729 and 16 cycles, and its edge candidate scoring come from the old
VRS 2.2 vehicle. They are not licensed by the author source as mandatory
SWEGCA behavior. Their old presence must not be used to omit author
coactivation, memberships or portal lifecycle.
`compact_index.py@c06092a:311` also normalizes explicit cue posting keys with
`_cue`; the author `HotIndex.append@7536139:154` applies `casefold()` only
before insertion. This 2.2 normalization is not copied into the rebuilt
author-rule postings path.

The user still requires physical splitting, linked original experience,
parallel work on the 16-thread CPU, preservation of the entire SWEGCA logic,
and a 4 GB memory limit. The implementation must meet those requirements
using author rules or obtain an explicit decision for any new rule. A split
storage representation cannot change source addresses or bypass `Graph.append`
when session VRS experience enters a main generation.
The C++ `ValidatedEventVrsInputs` introduced for the rebuild is only the
author's cold validation and immutable generation-binding boundary expressed
over a storage view. It scans finite direct, score, edge and strength values,
signs, endpoints and source-bound dependencies once. Event advancement,
float32 rounding, pending rounds, Graph.append, regions, coactivation and
portal lifecycle are not implemented by that boundary. A future physical
view must prove immutable backing and preserve the packed native edge layout;
`EventEdge` is a logical value, not an on-disk `sizeof` format.
The event kernel's two incoming sums use Python `math.fsum`, so the C++
arithmetic helper follows CPython 3.12.7 `Modules/mathmodule.c:1289-1363`
(partial expansion and final half-even correction) for finite event terms.
That source port is not numerical-parity evidence until the complete graph
path can be built and checked against pinned author cases. Ordinary `double`
accumulation would not preserve the author's specified rounding order.
The C++ `advance_event_vrs` now carries the author's synchronous round order:
all incoming edges contribute to each pending node, incident edges update
after the new node scores, signed-zero float32 comparisons control pending
work, and exhausted rounds return that work as pending. Its sparse maps and
per-node edge lists still need a bounded physical representation for the
4 GB product gate. It is not Graph.append, settlement publication, region
construction, coactivation, or a timing result.
The seven numeric translation units compile with floating-point contraction
disabled and fast-math disabled; otherwise fused or reassociated operations
could change Python's separate binary64 steps before float32 rounding. These
flags are a build contract, not a parity test.

The C++ `plan_vrs_state_update` now follows the source's first replayed
connection observation, one strength change per current verdict, first-seen
alias grouping, conflict abstention and retained source judgments. Its receipt
remains detached and denies action, write and semantic-promotion authority.
A source-based binder and event signal proposal consume it, but no Main
generation publishes their result.
The author calls Python `int()` and `float()` on observation fields; the C++
numeric conversion covers JSON numbers, booleans and ASCII decimal strings,
including numeric underscores. Python's non-ASCII decimal digits and extreme
string-float underflow still need exact cross-language treatment before parity
can be claimed. These are explicit remaining source gaps, not silently accepted
input changes.
The C++ event-strength binder now checks the detached state-update receipt
against the immutable VRS generation, one explicit connection namespace,
original edge strengths, source verdict operation and promotion decision. It
rounds changed strengths to the selected f32 or f16 storage format and rejects
an f16 threshold crossing caused only by rounding. The bound strength map and
target seeds are still detached work; signal settlement is translated in a
separate unit, while durable input equality and publication remain
unimplemented. The f16 conversion is a
source-based binary16 translation, not a measured parity result.
The C++ `settle_event_signal` now carries the source's fixed input-strength
map through synchronous score rounds. It reads every incoming signed edge for
each affected node, caches the invocation's topology and original scores,
revisits changed nodes and outgoing targets, and returns exhausted work as
pending. Its receipt reports zero reapplied strength updates and no storage
binding. The current ordered maps and per-node topology vectors still lack
a bounded physical representation under the 4 GB limit, and no production
Main generation publishes their result. The translated path has not been
compiled or compared numerically with the author implementation.
The C++ `plan_graph_append` now prepares the source's record/cue node order,
bidirectional positive edge additions, 0.1 record-presence direct signal,
unresolved new nodes, changed-node seeds and explicit-proposition strength
receipt against the unpublished HotIndex view. An opposing original causes
all same-claim strength proposals to abstain. This is the front of
`Graph.append`, not a published Graph: the sparse `prepare_event_delta` bridge
now creates a detached successor. `settle_graph_event` then applies the
source's 512-round signal budget, refuses pending work, and writes only the
changed score/strength addresses into a settled successor with the author's
score-bound snapshot digest. Affected-component region recomputation and Main
journal commit/CAS must still follow in the source order. The plan does not
infer truth from a shared cue or historical outcome.
The C++ `affected_graph_component` now follows the author's directed outgoing
walk from the new record and related prior records, then sorts the complete
reached node and edge addresses and remaps endpoints locally. It retains signs
and current strengths for every reached edge, including zero-strength edges.
This produces a source-shaped input to `ConnectivityRegions.build`. The C++
region unit now carries the author's weighted modularity local moves,
multilevel collapse, overlapping membership coefficients, reverse membership
directory, source-generation guard and topology receipt. Its C++ reductions
have not been compared to NumPy's `reduceat`, `sum` and `bincount` order, so
exact numeric parity and topology digest equivalence remain unverified.
The detached graph-region replacement now binds the old component directory
to the old input generation, builds the changed component's topology from the
settled generation, refuses unconverged work and records which old component
identities must be replaced. Its graph receipt retains the signal proposal,
explicit Re-evidence updates and author authority flags. Shared-original
bridging, actual region publication and Main commit remain unfinished.
The C++ endpoint segment index now preserves the author's stable per-segment
sort, segment-concatenation lookup order, source-generation binding and
geometric tail merge. That lookup order is required by `math.fsum` in the
event kernel. The current segment vectors are an algorithmic translation;
they are not the bounded disk-backed representation needed for a large main
under 4 GB. The verified append entry is private to `EventDeltaView`, which
preserves the parent edge endpoint/sign prefix by construction before it uses
the index. The delta view carries the author's immutable four-byte sparse
overlay for direct, score, unresolved, edge and strength values. A successor
shares the cold base and prior radix branches; changed addresses and appended
values are checked before binding. It takes the author's trusted delta route
after those checks, avoiding a full cold scan of every old node and edge per
append. The current in-memory radix and retained parent generations still
need a bounded disk representation and a generation-pinning publication path.

Existing VRS 2.2 pair certificates and numerical arrays are historical
evidence. A new source implementation must account for their lineage and
explain numerical differences; copied originals alone do not prove numeric
parity. No new build, test, benchmark or resident service has been run for
this source comparison.
