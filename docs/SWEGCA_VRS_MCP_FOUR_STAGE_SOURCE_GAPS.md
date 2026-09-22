# Four-stage source gaps for the full SWEGCA VRS-MCP rebuild

This is a read-only source audit at VRS-MCP revision `c06092a`, not a new
execution-order decision or a test result. The reviewed order and N/C/E
corrections are in `SWEGCA_VRS_MCP_ORDER_FOR_REVIEW.md`. These findings
identify old behavior that cannot silently become the new product contract.

## Input through completed Recall

1. `src/swegca_vrs2/store.py:1474-1609` creates `keys(query)` before
   `detect_deja_vu`, then computes cue fanout, informative cues, proposition
   closure, ranking inputs, and optional region scope between Déjà vu and the
   `recall_candidates` call. The existing path therefore does not place the
   author navigation and completed Recall inside one measured pre-Replay
   boundary. Its own timing cannot prove the user's `<1 ms` condition.
2. `src/swegca_vrs2/engine/mosaic_memory_activation.py:247-268` computes
   `candidate_count` by constructing a set of every episode ID in matched cue
   postings *inside* Déjà vu. That work scales with posting fanout. A fast key
   claim cannot be inferred from the name of the stage.
3. `src/swegca_vrs2/projected_recall.py:384-456` creates candidates using
   `_exact(identifier)` before constructing the `RecallResult`. The exact
   header is not necessarily an original Replay body, but the work and reads
   occur before the represented Recall boundary. `sharded.py:261-301` also
   has distinct exact-address and projected branches, so one branch's timing
   cannot prove the others.
4. `src/swegca_vrs2/engine/mosaic_vrs_portal_activation.py:81-122` runs region
   preactivation, association lookup, portal planning, and one navigation page
   after Déjà vu but before `recall_memory`. That is the author portal route,
   currently unconnected to the public `Main`; connecting it puts that work
   in the measured Déjà vu + navigation + Recall boundary.

## Candidate navigation and completeness

1. `store.py:1607-1682` calls Recall and then labels returned candidates as
   local/member/portal/unbridged. Those labels do not themselves make region
   or shared-original portal traversal select the addresses.
2. `projected_recall.py:405-413` constructs the candidate IDs and then calls
   `_navigation` at line 412; later cross-shard portal labels are added after
   the four-stage receipt at lines 496-511. The selected candidate set is not
   demonstrated to be closed under those portal links.
3. The author `mosaic_vrs_local_navigation.py:57-104` has a continuation cursor
   and bounded cue pages. `mosaic_vrs_portal_activation.py:108-120` feeds only
   the first page into one Recall call and exposes deferred regions. Its own
   receipt says `complete_transitive_search=False`. A page budget is therefore
   not evidence that all candidate addresses were retained.
4. `store.py:1481-1603` can apply a region mask and record excluded rows when
   `region_scope='regions'`. The normal default is `all`; the rebuild must not
   treat an opt-in masked candidate result as proof of complete Recall.

## Selected Replay and coactivation

1. The generic `replay_memory` at
   `engine/mosaic_memory_activation.py:379-400` opens every candidate it is
   passed. The separate `activate_memory` and `activate_with_portals` callers
   pass their full Recall result (`mosaic_memory_activation.py:597-618`,
   `mosaic_vrs_portal_activation.py:120-124`). They conflict with the user's
   selected-first-original Replay rule if used directly.
2. The current `store.Main.recall` deliberately constructs a one-candidate
   `selected_recall` for Replay at `store.py:1717-1725` and may add opposing
   originals after initial Re-evidence at lines 1726-1762. The projected path
   similarly opens a chosen original and conflict-related opponents at
   `projected_recall.py:448-494`. This is a behavioral source reference, not
   proof that stage order, navigation, or transport is correct.
3. `engine/mosaic_vrs_coactivation.py:107-156` requires the receipt's Recall,
   Replay, and judgment IDs to be identical sets and then reads the actually
   opened original to record a coactivation event. The new path must preserve
   that observed-activation provenance. The full unopened Recall address set
   belongs beside the receipt in `memory_selection`.
4. `native_context.py:177-231` requires equal receipt candidate/Replay/judgment
   sequence sizes and matching same-index IDs before returning a page. Keep
   this check; page the full `memory_selection` address set separately.

## Dormant author components and missing dependencies

The static entry reachability inventory in `SWEGCA_VRS_MCP_SOURCE_LEDGER.md`
found the coactivation, portal, local-navigation, membership-cache, and
packed-membership modules outside all four current entry-module import graphs.
This does not prove they are never imported dynamically, but it prevents an
active-product claim from file presence alone. The author components expect
`controller._owner`, `_runtime`, `_lock`, and `_vrs_region_binding`, whereas
the public `Main` stores `_generation`, `owner`, and `_region_binding`.
`mosaic_vrs_membership_cache.py:96` imports a missing
`mosaic_vrs_nodeset_membership_cache`. These paths cannot simply be wired in.

## Host ingress and real SessionEnd source boundary

1. `codex_hooks.py:42-45` generates a `UserPromptSubmit` MCP tool hook for
   `memory_prompt`. In `layered.py:254-286`, that tool resolves the session
   resident and sends `hook_recall` before any fallback to the durable main.
   The session-first branch is explicit. The user-input-through-Recall time
   still includes host hook dispatch, MCP transport, resident routing, and
   the old pre-Recall work above; source order alone cannot prove `<1 ms`.
   Hook entry is only an internal diagnostic start. The user target includes
   host dispatch; without a host input timestamp, it remains unverified.
2. `session_capture.py:282-314` has an alternate command-hook prompt path that
   begins a recall lease and enters `VRSClient` before `hook_recall`.
   `VRSClient.__enter__` calls `ensure_daemon` at lines 157-169. The generated
   Codex hook currently chooses the MCP path, so this alternate path must not
   be mixed into a timing claim for the installed hook.
3. `session_capture.py:61-110` extracts visible user, assistant, system,
   developer, tool, summary, compaction and usage records. It explicitly
   excludes encrypted/raw reasoning and reasoning without a visible summary.
   `session_capture.py:316-435` splits long records into stable-address parts
   and sends them through `VRSClient.ingest_many` to the session VRS; its cursor
   records positions/counts and recent addresses, not recall content. The
   source audit cannot claim that an active host emits every desired item or
   that every emitted item is captured without a runtime end-to-end check.
   The rebuild must admit visible tool results themselves through SWEGCA VRS,
   splitting long results into ordered, source-bound observations within the
   author's field limits. An address alone is not experience admission.
4. The current hook path schedules `conversation_finalize.py:14-45` from a
   real `SessionEnd` event. The finalizer captures a stable transcript tail
   under the tailer's lock, then publishes an end marker.
   `session_capture.py:461-497`
   closes the session resident, inspects the primary and native child shards,
   and attaches them to main as native stores. `Interrupt` schedules capture
   only (`conversation_finalize.py:64-79`). This order matches the stated
   SessionEnd boundary at source level; it is not a live attachment proof.
5. `layered.py:162-225` opens main only after a ready session context reports
   `candidate_count == 0`; `layered.py:254-286` applies the same condition to
   the prompt hook. The reviewed new condition is empty Déjà vu `matched_cues`.
   The old path's later fallback is source evidence, not an accepted timing or
   selected-Replay implementation.

No evaluator, model benchmark, service, or resident VRS was run for this
source audit.
