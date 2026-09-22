# SWEGCA VRS-MCP full rebuild contract

This document is the implementation map for a new VRS-MCP runtime. It is not
an acceptance report. The previous Python runtime remains a reference until
the new runtime has replaced every product entry point. A passing old test is
not evidence that the new architecture is complete.

## Authority and experience

The public SWEGCA specification at
`SWEGCA-Architecture/paper/swegca/ARCHITECTURE_SPEC.md` defines one main owner,
status-unfiltered addressable experience, evidence-gated decisions, explicit
authority domains, and reversible persistent mutations. For this memory MCP,
the main owner alone owns identity, VRS generations, original experiences,
source/revision lineage, current evidence judgments, and attachment of ended
sessions. MCP transport, host hooks, workers, and a language model are
producers or readers; they cannot turn a retrieved record into truth or an
action permission. No internal language-model call belongs in this runtime.

Every admitted host-visible record is an observation in a native session VRS
generation with its exact source address and uncertainty. The transcript is an
ingress stream, never a recall store. During an active session, Déjà vu reads
that session VRS first. Only empty session `matched_cues` at Déjà vu
opens the long-term main. Session VRS shards become main-owned only after the
host's real `SessionEnd`; idle time and `Interrupt` cannot attach them.
Host-visible tool results are records too. Oversized records are split into
ordered, source-bound observations within the author's field limits; their
content enters the session VRS instead of remaining only as transcript
addresses. The original byte and line spans, content digest, role, revision,
and part order remain attached so Replay can open the exact original segment.
The new C++ source adapter now extracts Codex and Claude host-visible records,
filters private reasoning payload fields, and streams normalized observation
parts with stable request IDs, source addresses, raw-line and content byte
spans, and content digests. Whitespace-only parts are journaled as explicit
JSON string literals because the author's observation text field rejects
blank text; episode construction decodes them back to the exact original
content and checks each part digest. This adapter is not yet connected to
the host file watcher, session journal commit, or SessionEnd ownership link.
The native transcript scanner now holds a private per-source cursor lock,
reads only complete JSONL lines, batches validated observations beneath the
transport frame budget, and advances its content-free cursor only after the
session VRS ingest interface returns the exact original IDs and pair
certificate. A partial line stays unread. A line above the current 16 MiB
parser bound stops at the previous safe cursor; no line is discarded. The
cursor also binds the host file's device and file identity, so replacement
at the same path resets scanning even when the new file is not shorter. The
session Main coordinator must implement the ingest interface before this
scanner can commit a real experience. A streaming parser for larger single
lines remains necessary for the full no-omission requirement.
The C++ SessionEnd gate now records an explicit host end intent, waits for
the live watcher to release its lock, captures a stable final tail, and asks
the session Main owner to seal every VRS shard. It verifies each sealed
native journal generation, published row count, last pair ID, ownership
lock release, and in-session path before writing an ended marker. Interrupt
has no path to this end intent. The native Main ownership registry now
reopens the ended marker and its intent, rechecks every sealed journal, and
atomically attaches the whole session batch. Identical retries are accepted;
ID, path, or generation reassignment is rejected. This ownership link does
not yet run the background SWEGCA Main append or publish a Main read
generation. A native background journal walker now checks that a shard is
linked, pins its sealed source journal, validates every typed row, and
delivers each contiguous source pair group to a Main transaction sink. Its
durable cursor advances only after a Main group commit, so a crash retries
the same request IDs through the author's idempotence rule. The concrete
Main transaction sink and scheduling of this walker remain unfinished; this
code alone does not integrate experiences. The ownership registry now stores
one complete SessionEnd link event per SWEGCA native journal row. Its
checksummed journal preserves attachment order and streams one event at a
time without loading all registered shards or rewriting a global JSON file.
The link journal now has a derived native exact-key directory for session,
shard ID, and shard path. Normal attachment uses those keys to reject
reassignment without walking prior link events. A journal/index publication
gap is replayed from the original journal; an invalid derived directory is
quarantined and rebuilt while preserving the original link journal. Opening
the native journal still scans its full generation, so the live resident
registry owner and fast trusted-open/recovery boundary remain necessary for
large-journal attachment throughput. No latency acceptance is claimed here.
An existing old `linked-shards.json` makes the new registry fail closed rather
than silently ignoring or migrating prior ownership records.

## One read path

The host input enters Déjà vu first. Déjà vu is the fast memory key and may
expose familiarity but no original episode identity. The author's weighted
region preactivation, coactivation witness query, portal plan, and first local
navigation cue page follow Déjà vu. Recall uses matched and navigation cues
to retain every candidate address selected by the author's rules. The portal
route is not a transitive graph sweep and deferred regions remain visible.
No fixed top-K may remove Recall addresses.
The current C++ `recall_memory` is a source-level reference for the complete
posting union, one-step semantic-family closure, cue overlap, and author sort.
It materializes candidate IDs and is not the bounded product read path.
Before host integration, the derived physical index must represent the same
complete address set within the 4 GB memory and pre-Replay timing gates; it
may not turn a page limit into a candidate limit or omit family dependencies.
The author `HotIndex.append` builds semantic cues from text, explicit cues,
proposition, or source fallback. The later physical `CompactIndex.append` also
put the episode ID into its cue vector (`compact_index.py:311-316`). The new
ID-to-row address route stays a physical lookup, never an extra semantic cue
in Déjà vu, navigation, Recall, or the returned original episode.
The author's explicit-cue posting key receives `casefold()` before insertion;
the episode cue is then stripped and whitespace-normalized. Preserve those
distinct values where they differ (`store.py@7536139:154-166`).
Déjà vu normalizes and deduplicates current cues in their first-seen order,
checks exact posting presence, and counts the exact distinct union of all
matched posting addresses. Its signal exposes counts and matched cues, never
experience identifiers. A compressed physical representation may calculate
that union without decoding original bodies, but an estimate or truncated
posting set would change the author's result. The current C++ code defines
this read contract; no published physical index yet satisfies it, so the
input-to-Recall latency target remains unverified.

Replay opens the first selected current original, preserving source, revision,
historical outcome, uncertainty, and exact content. Re-evidence judges that
Replay against the current VRS generation. Only a detected relevant conflict
opens opposing original experiences. Unverified, negative, failed, and
superseded records stay addressable; their availability grants no authority.
The selected Replay set is smaller than the complete Recall candidate set.
The full result stays in `memory_selection` beside the four-stage receipt.
Inside the receipt, Recall, Replay, and Re-evidence have the same opened
original rows in the same order, preserving the author's identity checks.

The required latency boundary is **user input through completed Déjà vu,
navigation, and Recall < 1 ms**, with Déjà vu as the first memory-store
operation. Host dispatch belongs in that boundary. Hook-process entry,
first Recall entry, whole MCP response, and Replay latency are separate
diagnostics. If the host does not expose a user-input timestamp, a
hook-entry measurement cannot prove the user-input target; report the full
target as unverified until it can be measured. A cold Replay read cannot be
substituted for this boundary. The limit must be checked as main size grows;
one small-source timing cannot prove it.

## Storage, scale, and implementation

The native journal is the source of truth, with checksummed frames and
generation binding. A checkpoint and all cue/proposition/region/read indices
are derived and rebuildable. Each shard must retain original addresses,
numerical event state, evidence decisions, regions, memberships, portals,
and coactivation lineage. After SessionEnd, an atomic native journal link
transfers session-shard ownership; background replay of those already admitted
VRS observations through the author's main `ingest`/`Graph.append` path then
integrates them into the main generation. Original session journals remain.
Neither step may run during an active session or use transcripts for Recall.

The author's `HotIndex.append` successor is calculated against an unpublished
view. For multiple rows, every next row sees preceding pending additions,
deduplication, supersession links, proposition members, snapshot IDs, and
outcome counts. `Graph.append` reads the same pending view and its original
episode headers, including explicit proposition and polarity. A duplicate is
a no-op for memory and Graph. Journal commit precedes public replacement of
memory, Graph, and their bound pair; an aborted transaction discards the full
pending view. A failed pending view is never reused. This preserves the author's
`Main.ingest` state/commit/publication order (`store.py@7536139:371-405`) while
allowing a bounded physical shard implementation later. The currently added
C++ pending view implements the row visibility part only; product publication,
physical sharding, and performance remain unverified.
The detached Main batch now has a native original-journal append boundary.
It compares the parent pair and expected journal head before writing one
checksummed batch frame. If the append became durable but rotation failed,
an owner retry accepts it only after matching every original row and its
single frame exactly. It does not publish the derived memory/Graph directories
or advance the pinned read generation. The full Main commit and recovery
coordinator is still required before this boundary is a product ingress path.
The same durable batch can now be applied to derived original-ID, metadata,
raw cue, proposition, successor, source, and request-ID directories. Each
candidate row is checked against its journal frame before any projection
write. A frame visitor validates the whole batch with one frame read; selected
Replay still opens only its requested original. A retry verifies an already
indexed original header and reuses the
same first journal address; all postings and operation certificates retain
their row sequence. This path only prepares row-limited derived state. The
Graph node and numerical stores, publication coordinator, and crash rebuild
remain necessary before any new Main read generation becomes visible.
The live Main memory seed now folds only newly added originals through the
author's snapshot recurrence and six outcome counts, checking every planned
successor against the pinned parent pair. Duplicate originals leave both
values unchanged. This seed is prepared for the future published HotIndex;
it does not itself publish the incomplete Main generation.
Graph now has a native node-address directory candidate. It records the
author's continuous record-then-fresh-cue node order in append-only name and
reverse-address files, with a tiered exact name index instead of a resident
string map. Sixteen prefix workers can prepare independent name slots. Old
readers hide later addresses by their pinned node count. The indexed cursor
advances only after the affected name tables are synced, so a node-file tail
without completed name lookup cannot receive a publication certificate.
Main's read owner
requires the node directory's journal generation, Graph snapshot, pair,
published row limit, and count to match its other pinned state. This is a
physical node directory only: bounded Graph numeric, regions, dependency
and coactivation stores, full publication/recovery, and <1 ms acceptance
remain unfinished.
The node publication writer additionally requires its proposed pair to be
the author's digest of the supplied memory and Graph IDs and the native
journal head to carry that same pair at the published row. It cannot certify
an arbitrary pair string or a row beyond the committed journal head.
Node application itself accepts the detached source Graph batch only after
the exact committed observation frame, pair, parent generation, and numerical
array lengths agree. It can replay a historical committed frame during Main
recovery; the writer advances its unpublished Graph binding after each batch
while existing readers stay pinned. Publishing still waits for the current
journal head.
The pending view and the native HotIndex metadata projection now derive the
same header from the author's episode. The checksummed projection frame binds
that header and the distinct raw posting keys to a journal sequence and pair
ID; it carries no original observation body. A physical header/posting
directory and published HotIndex reader are still required.
The metadata log now assigns each frame to one of 256 original-ID prefixes,
returns its durable byte offset after sync, and checks generation, source row
limit, frame checksum and exact episode ID on an addressed read. Independent
prefixes have separate locks. This log has no cue lookup or publication gate
on its own; a derived address/posting directory and crash recovery still need
to bind it to a published Main generation.
The exact ID directory's v2 slots now bind both the journal frame address and
the header-log address to the same original ID and journal sequence. Its
16-worker rebuild validates each journal row once; workers construct headers
only for the first occurrence of each original, then sync the header frame
before writing that ID slot. A read of header metadata can therefore avoid
opening the original body. The new native cue directory adapts the existing
VRS hash-prefix tiers, exact cue-byte verification, dual checksummed heads,
and append-only posting chains. Nodes name first original journal sequences;
published row limits hide future nodes from older readers. A descending
sequence merge counts the exact distinct posting union without holding all
candidate IDs in RAM. A second journal-ordered pass now validates source
observations, resolves the first original and its header projection, and
routes raw cue postings, proposition memberships, supersedes-successor, and
original-source
links to 16 bounded worker queues by digest prefix. A key always reaches one
worker in journal order. The proposition, successor, and source links reuse the same
derived posting format in separate directories; they do not grant evidence
authority. A supersedes link requires a prior original with the same source
and a different revision, and no other successor may claim that prior ID.
The source key is checked against the original observation before indexing.
Main's pinned read generation requires all four posting directories and the
exact original directory to carry the same journal generation, row limit,
and pair certificate. The native published HotIndex now reads headers from
the exact ID's projection address, and cue, proposition, successor, and source sets
from those pinned directories. Main additionally checks that its pair's
HotIndex uses those same directory objects. An explicit semantic-family
reader can be supplied; ordinary standalone observations have the author's
empty family tuple. A native writer for explicitly recorded family
directories is still missing.
Both derived rebuild passes now use the author's typed `journal_entry` rule.
Alias, usage, and consolidation rows must pass their exact fingerprint and
request-ID prefix checks and remain counted in the source journal. They do
not create a new observation ID, cue posting, proposition, or successor link.
Their own Graph/VRS effects and pair certificates still require the Main
generation replay path; skipping them in an address index is not dropping
their experience or proof of complete Main restoration.
The detached alias and usage Graph transitions now reproduce the author's
flat alias-root map, usage overwrite, snapshot digest chain, and receipt
fields. An unchanged numerical source may be rebound with an empty event
delta only after its parent snapshot is checked. These transitions are not
yet a Main commit/replay coordinator. Consolidation must run the author's
VRS refinement and verify its version certificate; no alias/usage shortcut
stands in for it. The current maps are source-level state and still need a
bounded resident or native representation for the 4 GB product gate.
Main's pinned read generation now retains the auxiliary Graph state and
requires its snapshot ID to equal both the validated numerical source and
the pair's VRS snapshot. The initial auxiliary state uses Graph.empty's
identity digest and empty alias, usage, and receipt maps.
The alias ingress plan now strips Python whitespace, sorts unique aliases,
requires every proposition to be known from the main index or current alias
registry, skips unchanged bindings, and prepares the author's journal body,
fingerprint, request ID, successor Graph metadata, and pair certificate.
The plan remains unpublished until the missing Main journal coordinator
commits its row. It does not turn alias declarations into evidence.
The usage ingress plan now visits the source directory only for changed
counts, confirms that at least one original for each source has no successor,
and prepares the author's usage journal body, fingerprint, request ID,
successor Graph metadata, and pair certificate. Source postings are streamed
until a live original is found; the full list does not enter RAM. Usage remains
provenance, not new evidence. This plan also awaits the Main coordinator.
The current source `Graph.append_many` is the required ingress generation:
new record and literal cue nodes in one observation batch see earlier nodes
in that batch, resolved records receive their v0.2 direct value, and new
edges retain base strength until later consolidation. Superseded records are
marked unresolved, and the batch receives one graph snapshot ID and source
receipt. `graph_batch_append.cpp` now prepares this detached batch delta.
`main_observation_batch.cpp` now normalizes an entire observation batch,
checks published and repeated request IDs, stages the author's HotIndex
successor in input order, folds only newly added original fingerprints into
that Graph batch, and calculates one pair certificate for every new journal
row. Existing requests retain their historical pair. These are detached
candidates: no journal append, physical index publication, or Main owner
replacement occurs in this planner.
The older single-record `graph_append.cpp` event-signal settlement is not the
product ingress path; its remaining users must be rebuilt or removed before
publication. Main batch journal commit and source-bound Graph node application
are present as separate primitives. The complete Main commit/recovery owner,
physical Graph numerical and dependency stores, and batch replay publication
remain unfinished. This source path has not been built or parity-tested.
`EventDeltaView` now asks the source-bound dependency interface to advance its
verified immutable endpoint/sign prefix. It no longer requires the in-memory
`SegmentedEndpointDependencyIndex` concrete type. The source segment order
remains the contract; a physical disk directory has not yet been installed.
The native numerical node/edge record codec fixes little-endian field
widths, bit-preserved float32 values, separate edge base/current strengths,
and record checksums. It rejects invalid values on both encode and decode.
It is a physical value format, not a publication or 4 GB acceptance result.
The first physical page primitives now store 256 fixed-width numerical records
per immutable node or edge page and retain earlier page locations through a
three-level copy-on-write address map. Page reads verify their journal
generation, logical page ID, count, checksums, and live record values. A
partially written final derived page is truncated only by its locked writer;
the original observation journal is untouched. The endpoint dependency
directory, the publication certificate,
and bounded read cache are still missing; no product numerical generation
uses these files yet. The committed batch applier now verifies the exact
native observation frame through the same check as the Graph node directory,
prepares the author's EventDelta successor, validates any old numerical page
it replaces against that pinned parent, and writes only touched node/edge
pages before returning unpublished map updates. Crash retries may leave
unreferenced complete pages until derived-file reclamation is implemented.
The page map updates now enter a separate native derived journal under the
same Main owner lock. Each row binds the Graph parent/successor, memory and
pair IDs, exact original observation frame address, counts and changed page
offsets. Recovery reopens that original frame, checks every row's type and
pair, and validates every referenced changed page before rebuilding the
bounded address map. This remains a derived index: original observations and
their SWEGCA replay, not the map row, determine numerical truth. A map
checkpoint, Main publication gate, physical endpoint index, and reclamation
of orphaned pages are still needed.
This adapter currently reads physical address files on lookup. It is a
source-bound correctness path, not accepted evidence for the author's
`lookup_requires_io=False` hot property or the user-input-to-Recall <1 ms
target. The resident bounded index and selective page strategy remain to be
implemented without deleting the cue, proposition, supersedes, region,
portal, or explicit-family rules.
During a fresh, unpublished cue rebuild, the derived files are synced once
behind the publication barrier instead of after every posting. Publication
locks all prefixes, syncs every data and table file, then writes the durable
marker. Later appends to an already published directory retain per-posting
sync so older published row limits keep a durable prior head.
The Main operation overlay now checks request ID and normalized observation
fingerprint before any new HotIndex/Graph candidate is staged. Identical
requests retain their original episode and pair certificate; conflicting
reuse invalidates the unpublished transaction. A native operation directory
now stores exact request keys, fingerprints, optional original addresses,
historical pair certificates, and journal sequence in SWEGCA-style digest
prefix and graduated slot tiers. A 16-worker, journal-ordered rebuild
validates every typed row before deriving these certificates; duplicate
request IDs fail closed. Main's pinned generation now requires the operation
directory to share its journal generation, published row limit, and pair
certificate. Publication still needs the Main commit/recovery coordinator
to verify the final pair and bind it with the other directories.
The directory code has not been built or product-tested.
The pair ID is the author's SHA-256 of canonical schema version, memory
snapshot ID, and VRS snapshot ID. The owner may replace that immutable pair
only if its expected current ID still matches, and only after the corresponding
journal commit. `MainReadGeneration` now retains the pair together with the
validated numerical input, node and region directories, coactivation index,
portal state, native journal read view, exact original address, four posting readers,
the native operation directory, and
published row limit. Construction rejects VRS and original-source binding
mismatches before one CAS owner can expose the bundle. The four-stage path now
opens the selected original through that exact native address; conflicting
opposing originals use the same path one at a time. A reader must retain its
snapshot pointer through Recall and Replay. This owner is still a primitive:
the Main commit coordinator and physical storage publications are missing.
The derived `PublishedHotIndex` no longer offers original episode bodies.
The four-stage read and subsequent coactivation observation both use the
published exact-address directory and the pinned native journal to reopen
only the originals they actually need.
The same canonical pair digest is now callable for an unpublished memory/Graph
candidate before its journal row is written; constructing the published pair
uses that exact function. This keeps the certificate calculation identical on
both sides of the commit boundary.

The existing `VRS2JNL1` frame remains the canonical journal representation:
little-endian payload length, zlib payload, SHA-256 payload checksum, and the
five row fields (sequence, request ID, observation envelope, fingerprint,
pair ID). This physical format comes from the current VRS 2.2 native journal,
not from the author SWEGCA functions. A read-only 2026-09-22 inventory found
47,566 current journal rows in two SQLite session/shard originals while the
session native head holds only its eight-byte magic. Preserving current
experience therefore requires a separate, one-time verified export of the
original rows and their fingerprint/pair/source lineage into this canonical
native format. The original files stay untouched during the rebuild; no
SQLite reader or export path remains in the final runtime. `checkpoint.vrsc`
contains a Python pickle and is only a derived cache. The C++ product
rebuilds derived state from the native journal and does not need a Python
checkpoint reader. Codec and digest implementations sit behind small C++
modules and use no prebuilt wheel or external memory-decision logic.
The frame reader accepts the canonical JSON byte structure emitted by the
sole native journal writer. It validates a complete frame before streaming
its rows to a new main generation; it does not depend on the Python decoder's
acceptance of alternative key order, whitespace, or extra fields.
The C++ journal can now stream validated frame addresses and reopen one
addressed frame for one exact sequence. These addresses are a derived read
index, not an experience store. A renamed `head.vrsj` is resolved through the
sequence-ordered immutable segment name after rotation; the generation,
frame span, checksum, and row sequence are checked on the cold read. The
exact-address join is now present as a library primitive. Published HotIndex
and end-to-end selected Replay wiring are still missing, so this does not yet
prove bounded Replay cost.
The addressed cold read validates the entire compressed frame first, then
stops its second streaming pass at the chosen row; it does not construct the
remaining originals in that frame during Replay.
Given an exact address, the C++ Replay source now reconstructs one author
`MemoryEpisode` from that original row only after verifying its request ID,
observation fingerprint, and requested episode ID. This is a cold original
read for selected Replay, not a Recall-time transcript or capsule scan. Its
address must still come from a generation-bound derived directory, not from
searching journal bodies during a user read.
The address-directory builder can stream each verified original row together
with its frame address in one journal walk. It validates the complete frame
and its sequence span before delivering any row from that frame. This is a
rebuild/ingest path; it does not run on a user Recall or Replay request.
For a new real-time observation, the native journal append now returns the
durable frame address and its new sequences directly. Main can add the one
new original address without scanning earlier journal rows. A duplicate
author episode still keeps its first address in the exact directory.
`NativeJournalReadView` now pins the physical generation path and row limit
without a new journal scan. A later append cannot widen that view, and a
generation rewrite no longer immediately deletes the previous journal path
while a reader may hold it. The eventual owner must reclaim retired paths
only after their readers have gone and must enforce the 500 GB storage gate.
The C++ exact-address directory under construction uses the existing VRS
prefix, odd-step, and sealed-level physical lookup rule. A slot stores only
the original journal sequence and frame offset/span; the observation body is
not copied into an external capsule. Readers carry the published Main row
limit, so a newly inserted address cannot become visible through an older
pair. A complete owner may atomically write `PUBLISHED.json` with the journal
generation, row limit, and pair ID after the corresponding address writes are
durable; read-only directory opens reject an unpublished or mismatched
generation. This is not yet a published product index: startup recovery,
journal head watermark reconciliation, disk quota enforcement, and Main
ownership integration remain required before production reads.
The selected-original primitive now joins the exact directory to the native
journal and refuses a generation mismatch or a row newer than the caller's
published limit. The concrete `PublishedHotIndex` and four-stage owner still
need to call this primitive after their complete Recall has selected one
original. Merely having the primitive in the library is not product wiring.
The exact-address rebuild walks one already validated active journal, checks
each original observation's request ID and fingerprint, and retains the first
address for a repeated author episode ID. It routes verified IDs into 16
bounded, ordered worker queues by address prefix, so independent segment
writes can proceed concurrently without reordering a duplicate original.
Each worker still uses synchronous segment I/O; throughput and 4 GB RSS are
unmeasured. The rebuild requires a fresh unpublished directory; a failed or
interrupted build is discarded and rebuilt rather than published. This
directory is still only one derived projection. It does not
replace the complete HotIndex posting, proposition, supersession, semantic
family, Graph, region, coactivation, and portal representations.

The two populated SQLite stores have separate owner identities and sequence
spaces. Export each to its own immutable native archive, recording the exact
original path, the SHA-256 of the ordered five-field rows read in one SQLite
transaction and reread from the archive, the archive directory and each file's
SHA-256, row count, and source identity. For each row in sequence order, hash
its canonical UTF-8 JSON five-element array preceded by that array's byte
length as an unsigned little-endian 64-bit integer. The source row digest
includes content visible through any SQLite WAL. Do not concatenate the
owners' rows or invent an integration order.
Rebuild a separate active C++ generation for each owner. The archived five-field
rows retain the old pair IDs as historical certificates. The active journal
retains each original observation body, request ID, sequence, fingerprint,
source address, and revision, but carries pair IDs re-derived by the complete
SWEGCA generation rules; no active journal mixes certificate domains. Existing
2.2 consolidation rows may have their derived certificate fields and
fingerprints regenerated by the same full-rebuild rule. A real SessionEnd and
validated ownership/link record, not export completion, govern any later
session-to-main attachment and integration.

Every C++ function that implements an author rule carries a source tag of the
form `// SWEGCA: <file.py>@<commit>:<first>-<last>`. The commit gate resolves
the file and line span in the pinned tinylm, SWEGCA-Architecture, or VRS-MCP
source. A `user@YYYY-MM-DD:<line>` tag points into the approved execution
order for rules that come from the user rather than an author module. A tag is
provenance for review, not evidence that the function is behaviorally equal.
Product and architecture tags use the full repository-relative source path,
because those repositories contain duplicate module basenames.

The runtime has no SQLite, Hermes, prebuilt wheel, or external replacement
memory logic. The implementation target is C++ source. Resource gates are
4 GB resident memory, 5 Gbit/s assumed SSD bandwidth, and exactly
500,000,000,000 bytes maximum allocated storage. Consolidation must use the
available 16-thread CPU without serializing all independent shard work.
The user's billion-parameter unit has not been defined in the current VRS
code, so no record/edge/byte count may be reported as that target.

## Replacement map

| Current product area | New owner or boundary | Required preservation |
| --- | --- | --- |
| `session_capture`, `conversation_*`, `codex_hooks` | Host ingress and session lifecycle | Every host-visible record, stable addresses, live session VRS, real SessionEnd attachment |
| `layered`, `server`, `native_memory`, `native_context`, `native_transport`, `loopback` | MCP transport and author evidence pages over one owner | Full source tool catalog, handle and cursor contracts, session-first routing, exact IDs, bounded output pages, no transport authority |
| `store`, `native_journal`, `native_lock`, `resident`, `linked_shards` | One main owner and native generations | Original lineage, atomic publish, complete shards, journal recovery |
| `compact_index`, `cue_shards`, `exact_replay`, `read_projection`, `projected_recall` | Derived address and read projections | No source-of-truth duplication; full Recall addresses; selected original Replay |
| `flat_vrs`, `fast_regions`, `vrs_refine`, `vrs_evidence`, `engine/mosaic_vrs_*` | Main-owned numerical SWEGCA VRS | Author event settlement and refinement, accumulator, overlapping memberships, coactivation witnesses, shared-original portal lifecycle, generation binding; physical splitting and bounded parallel preparation |
| `engine/mosaic_memory_activation`, `engine/mosaic_proposition_directory`, `engine/mosaic_semantic_family_directory` | One four-stage activation | Déjà vu first, Recall closure, Replay selection, Re-evidence provenance and conflict |

Every product entry point and every source file must be either replaced by a
new implementation with the same required role or shown to be non-product
historical material. No old implementation may remain reachable as a silent
fallback. The new C++ sources will be introduced separately, then product
entry points will switch together after this map is exhausted.

## Confirmed defects in the old path

1. The public `Main` uses `_generation`, `owner`, and `_region_binding`; the
   copied author portal and coactivation controllers expect `_owner`,
   `_runtime`, `_lock`, and `_vrs_region_binding`. Those controllers are not
   wired to the public main. The author portal activator is not reached by
   current product entry points.
2. The normal product recall builds some region/portal path descriptions after
   it has already built Recall candidates. That cannot prove these structures
   selected the addresses.
3. `mosaic_paper_vrs_generation_rebind` and
   `mosaic_vrs_nodeset_membership_cache` are imported by product source but
   absent. The first-party import verifier correctly fails.
4. The current installed Codex hook configuration still uses an older prompt
   command instead of the connected `memory_prompt` source path. Source code
   and installed behavior are separate evidence.
5. `native_context.memory_context` requires matching receipt Recall, Replay,
   and judgment identities. The new path must preserve this check by keeping
   only opened originals in the receipt and full Recall addresses in a
   separate `memory_selection` result.
6. An old hook test expects four original Replay rows for four packet slots.
   The current rule opens one selected original unless conflict requires more.
   The test must be reworked without weakening the receipt identity check.

These are observed defects and design constraints, not a claim that all other
defects have been found. Do not run the old or new test suite as an acceptance
signal before the replacement map and code are complete.
