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
The author's `mosaic_vrs_region_graph.py@3bddcb7` exports four sealed numeric
arrays for cold grouping and `mosaic_vrs_region_storage.py@3bddcb7` loads or
saves a detached topology. Neither implements a partitioned live Graph or
cross-shard physical links. They cannot be cited as proof that the user's
split-and-link and 4 GB requirements are already implemented. A new physical
segmentation must preserve the same logical node, edge, region, membership,
portal and original addresses while retaining the author's calculation rules.
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
The ten numeric translation units compile with floating-point contraction
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
actual region publication and Main commit remain unfinished. The graph read
surface also resolves a node's source-bound region memberships and its maximum
outgoing strength without opening or copying original episodes. A shared-
experience bridge derives overlapping memberships from the original's existing
cue addresses and hot episode header. It keeps one original episode identity,
revision, source addresses and historical outcomes; it does not manufacture a
region-specific episode. The old generic Python region's `address_index`
contract maps to the Graph's literal `cue:` node directory here. Numerical
parity for this bridge has not been established. Region candidate lookup now
uses the existing hot cue postings: each region unions its cue originals,
multiple regions intersect, and the sorted result is only an address proposal
for the ordinary four-stage read. The read directory must prove that the
published memory and Graph belong to the same generation. This materializes
candidate sets and has no demonstrated 4 GB bound or pre-Replay latency.
The author portal activation at product revision `c06092a` begins with
`preactivate_regions` after Déjà vu. The C++ component-local translation now
maps matched literal cues to existing graph node addresses, sums overlapping
memberships, normalizes with `fsum`, and ranks regions without reading an
original body. It does not select a portal or measure the input-to-Recall
boundary yet.
The graph-adapted local navigator now validates a shared original against the
current pair, leads with its destination-core cue nodes, and retains the
author's seed-first cursor, bounded visits, skipped-seed accounting and
continuation. A Graph component also contains whole-original nodes; pages
visit those indexed terms but pass only literal `cue:` names to Recall. Page
completion does not claim complete memory or transitive graph search. Portal
selection and a connected Main read route still remain to be written.
The detached coactivation event now checks the opened Recall, Replay and
Re-evidence rows against the pinned original episodes and records one event
hyperedge, including failed, negative and pending outcomes. It makes no
pairwise relation or usefulness score. Because the product Graph publishes a
separate topology per disconnected component, each observed original carries
its own topology ID; the event's single topology ID is present only when all
opened originals share one. The detached association index now adds only
observed events, posts shared immutable witnesses by topology and origin,
rechecks original lineage against the current pair, and returns sorted
destination proposals with explicit rejection reasons. It never turns request
count, outcome or membership into a truth score. Main-owned durable event
admission and generation-bound index publication are not yet implemented;
the current ordered maps also lack a demonstrated 4 GB bound.
The detached portal planner now retains source-bound keys and every witness,
uses exact reduced request fractions `H/(H+age)`, converts each positive ratio
to rounded binary64 before `fsum`, compares the resulting float against the
rational minimum, keeps age and explicit revocation reasons, and selects the
first eligible candidate in author order. This uses local C++ integer limbs,
not a prebuilt numeric package. The policy's rational numerator and
denominator are bounded to unsigned 64-bit storage and reject values outside
that range; Python's arbitrary integer policy domain is wider. Numeric parity
and the complete pre-Recall timing path remain unverified.
The pre-Replay navigation prefix now accepts a completed Déjà vu signal,
preactivates the matched graph components in first-seen cue order, queries
observed associations, plans eligible source-bound portals, navigates at most
one indexed cue page, and passes those cues to ordinary complete Recall.
Unvisited origins remain explicit. Component order is a physical adaptation
because the product Graph has separate connected-component topologies; it
does not compare coefficient magnitudes across unrelated topologies. No
Replay or Re-evidence occurs inside this prefix. Session-first selection,
Main generation pinning, host hook dispatch and the full `<1 ms` measurement
remain unfinished.
The selected four-stage suffix now retains the full completed Recall in
`memory_selection` while the activation receipt carries one selected original
and any conflict-related opposing originals opened later. Its first Replay
precedes the proposition opposition check and Re-evidence; a detected explicit
conflict triggers only opposing-polarity Replay, then another Re-evidence pass.
All three opened receipt stages retain identical episode order. This is a
detached source path; current Main publication and host routing are still
unfinished.
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
The first native region read primitive stores the author's complete term,
core-label, overlapping-membership and reverse stable region-node arrays in
fixed-width checksummed blocks bound to one journal generation, component,
VRS snapshot and topology ID. Cold open validates every block, array shape,
offset boundary, region address and membership weight without retaining the
whole arrays in RAM; individual reads fetch only addressed blocks. This file
is an unpublished region read representation. The exact Graph edge arrays
remain owned by the source-bound numerical Graph pages, and topology
directory publication, generation manifest, bounded block cache, retirement
and Main atomic ownership are still unimplemented. Therefore this primitive
is neither a complete topology publication nor a 4 GB or latency result.
The region node-binding primitive stores the author's global-node to
component and component-local position addresses as copy-on-write immutable
pages. A pending Graph node has an explicit absent record, matching the
source behavior that nodes appended since the last region rebuild do not
silently inherit an old component. A generation manifest must still bind the
page-map root, exact topology files, source numerical Graph, memory owner and
pair certificate before this data can implement `GraphRegionDirectory`.
The source-bound binding view cold-validates a complete, contiguous logical
page map against the exact Graph snapshot and node count. Its fixed 16-shard
cache has a caller-supplied byte budget and changes no component or local
address on a miss. Only a component's root record may hold a nonzero physical
topology-catalog offset; member and pending records must keep it zero. The
view still refuses to act as a region directory until the catalog and the
missing manifest prove which immutable topology file belongs to each
component and Main pair.
The native topology catalog is an append-only, direct-offset directory of
checksummed component, VRS snapshot, topology ID and safe relative file-name
records. A component root binding can therefore open one exact cold-validated
topology without a graph-wide RAM map or directory scan. Catalog append and
file sync still precede the generation manifest and Main publication gate.
The native region manifest is a derived journal under the canonical Main
owner lock. Each row binds one complete binding-page map to the current
canonical source head, memory snapshot, Graph snapshot and pair certificate;
it explicitly grants no authority. A changed Graph snapshot resets the map
and requires every logical binding page, preventing topology files from an
older VRS snapshot from entering a new generation through COW reuse. A
same-Graph navigation replacement may reuse unchanged pages. Recovery checks
the complete manifest chain, the final source pair, every active binding page
and every component root's immutable topology. Main still needs a concrete
bounded `GraphRegionDirectory` and a coordinator that exposes it through the
existing exact-generation CAS.
The concrete native region directory opens only from that manifest state. Its
cold construction verifies every present global node against the exact term
at its stored component-local address. Runtime component and local lookups use
the fixed-budget binding-page cache; topology handles use a separate fixed
slot cache and always recheck the Graph snapshot on a hit. A cache miss still
cold-opens and validates an immutable topology file, so a resident hot
projection and the Main-wide 4 GB accounting owner remain required before the
input-to-Recall latency target can be measured.

Existing VRS 2.2 pair certificates and numerical arrays are historical
evidence. A new source implementation must account for their lineage and
explain numerical differences; copied originals alone do not prove numeric
parity. No new build, test, benchmark or resident service has been run for
this source comparison.
