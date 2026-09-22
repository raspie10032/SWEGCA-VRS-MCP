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
The pair ID is the author's SHA-256 of canonical schema version, memory
snapshot ID, and VRS snapshot ID. The owner may replace that immutable pair
only if its expected current ID still matches, and only after the corresponding
journal commit. Main must retain and pin the numerical Graph generation named
by the pair's VRS ID while readers use that pair; the C++ pair/CAS primitive
alone does not provide this Graph binding or a complete product transaction.

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
episode-to-frame address join, published HotIndex, and end-to-end selected
Replay are still missing, so this does not yet prove bounded Replay cost.
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
The C++ exact-address directory under construction uses the existing VRS
prefix, odd-step, and sealed-level physical lookup rule. A slot stores only
the original journal sequence and frame offset/span; the observation body is
not copied into an external capsule. Readers carry the published Main row
limit, so a newly inserted address cannot become visible through an older
pair. This is not yet a published product index: startup recovery, journal
head watermark reconciliation, disk quota enforcement, and Main ownership
integration remain required before the directory may serve production reads.
The selected-original primitive now joins the exact directory to the native
journal and refuses a generation mismatch or a row newer than the caller's
published limit. The concrete `PublishedHotIndex` and four-stage owner still
need to call this primitive after their complete Recall has selected one
original. Merely having the primitive in the library is not product wiring.

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
