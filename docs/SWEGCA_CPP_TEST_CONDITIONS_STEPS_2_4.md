# SWEGCA C++ test conditions: steps 2-4 (journal, experience, evidence wiring)

Status: draft for static review (board §10 step 9). No test code exists and
nothing is built until this list and the rest of the architecture pass static
review. Each condition names the rule it comes from, the setup, and the one
expected result. An expected result is never weakened to make a run pass: a
failing condition is fixed in the architecture (board §10 step 10).

Sources: board `docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md`
@cefdc3f §2 (invariants), §3B/§3C, §5, §6 (failure model), §7, §9, §10;
`ARCHITECTURE_SPEC.md@5901a5a` (I01-I08, I10); author L3
`mosaic_unrestricted_experience.py@5901a5a`, `mosaic_external_memory.py`,
`mosaic_versioned_memory.py`, L2 `mosaic_evidence_accumulator.py`.
Code under test: `claude/arch-integrate` @ac097ba and the provenance and parted-experience change after it.

The architecture fixes no product budget and keeps no usage counter:
memory, storage and the worker count are the host's, which counts and
judges them (user 2026-09-23 via codex 16:01, 16:10). Two host profiles:

- Baseline (SWEGCA-VRS minimum): 16 workers, 4 GB memory, 500 GB storage,
  5 Gbps storage bandwidth, input to Recall within 1 ms (board §1
  nano-core, user 2026-09-22). Every condition must hold here: the system
  must run within it (user 16:13).
- Scaled: a larger configuration for larger use, with more of any
  resource (user 16:13). The same conditions hold with the scaled budgets;
  no condition may depend on the baseline figures being ceilings.

A condition that states a bound is checked against its profile's bound, not
a smaller convenient one; A0 checks that the core has no such constant.

## A. Compile-rejection cases (must not compile)

A0 (source check, not a compile case): no file under `cpp/swegca_architecture/`
names a product budget (4 GB, 500 GB, 16 workers, `ResourceLimits`); the
ledger, the journal and the page cache take theirs from the host.

| # | Case | Rule |
|---|---|---|
| A1 | Construct `JournalStore`, `ExperienceJournal`, `ExperienceSelector`, `EvidenceAdmission`, `ReEvidence`, `VerdictSink` or `MemoryLedger` outside their friends | I01, board §4 |
| A2 | Copy or move-assign `PublishedRecord`, `ExperienceRecord`, `SelectionReceipt`, `StagedGeneration`, `CueTokens` | I01, lifetime |
| A3 | `SelectionReceipt<A>` for any `A` other than `NoAuthority`; convert a receipt, `ExperienceRecord`, `SelectedExperience` or replay handle to any authority-domain type | I10, experience-authority separation, failure 2 |
| A4 | Build `IndexVisitor` or `SelectionJudge` from a plain function (not an object), or from a callable with the wrong signature | function-reference contract |
| A5 | Call `EvidenceAccumulator::admit` from anything but `EvidenceAdmission` | I05, board §3C |
| A6 | Obtain a mutable view of journal bytes, a record's payload, or a receipt's fields | I07 |

## B. Journal (step 2; board §9)

Publication and recovery:

| # | Setup | Expected |
|---|---|---|
| B1 | Stage N records, publish, reopen | Same HEAD digest, generation, tail sequence; every record replays byte-identical with the same position and record digest |
| B2 | Publication fault matrix. The publisher's steps in order: (a) each segment piece written, (b) each page-log piece written, (c) the manifest log appended, (d) new directory entries made durable (when files were created), (e) `HEAD.part` written, (f) `HEAD.part` made durable, (g) `HEAD.part` renamed to HEAD (the commit point), (h) the directory made durable. A crash is injected immediately before and immediately after the durability call (fsync) of each file step (a)-(c), (e)-(f), and before and after each rename and each directory durability call (d), (g), (h). | Crash at or before (g): reopen shows the previous HEAD, every leftover (`.part`, unpublished tail bytes, unreached page logs) is removed, and no record of the interrupted generation resolves. Crash after (g), before (h) completes: reopen shows exactly the previous or exactly the new HEAD and everything that HEAD names replays. Crash after (h): the new HEAD |
| B3 | An in-process I/O failure (not a crash) at every point of B2 | store poisoned (B14); a reopen gives the B2 result for that point |
| B4 | B2 for a generation that opens a new segment and a new page log (new directory entries) and for one that only appends | Same results; step (d) present only in the first |
| B5 | B2 for `compact_view` and for `rebuild_view` (page-log steps only, then manifest and HEAD) | Old view or new view, never a mix; records unaffected |
| B6 | Crash during reopen's own cleanup (removing leftovers) | A second reopen reaches the same state as B2 |
| B7 | Crash between the HEAD durability (h) and the snapshot swap | Reopen shows the new HEAD (the swap is in memory only) |
| B8 | Corrupt one byte of a published segment the head generation wrote | `open` fails closed (`journal_segment_*` / `journal_record_*` digest code) |
| B9 | Corrupt one byte of an older segment | `open` succeeds; `replay` of a record in it fails with a digest code; `for_each_record` and `rebuild_view` fail on the chain |
| B10 | Delete or truncate a page log in the view's range | `open` succeeds with the view unavailable; `resolve`, `replay`, index lookups, stage and compaction fail `journal_view_unavailable` (replay goes through the view); `for_each_record` still scans and verifies the whole chain; `rebuild_view` from the records restores every lookup and replay |
| B11 | Unknown file in the directory | `open` fails `journal_unknown_entry` |
| B12 | Second owner of the same directory | `journal_already_owned` |
| B13 | Publish a stale StagedGeneration (HEAD moved) | `journal_head_changed`; nothing published |
| B14 | Any I/O failure during publish | store poisoned; every later call fails `journal_store_poisoned` until reopened |

Views:

| # | Setup | Expected |
|---|---|---|
| B15 | Records with addresses and index entries; lookup every address and every (kind, value) | Exactly the records carrying them, in address order, with positions equal to replay |
| B16 | Concurrent publishes while 16 readers look up | Each reader sees one snapshot's complete answer (no entry from a later generation, none missing from its own) |
| B17 | Random insert order into the trees up to many levels | Heights within `max_address_height`; compaction due only past the documented bound; after compaction the answers are identical |
| B18 | Record of kind other than 1 or 2 with a lowercase index entry | encode fails `journal_record_invalid:index`; a crafted segment with one fails decode |
| B19 | Index entry with the separator, a value empty, a key over 4096 bytes, entries not strictly increasing | `journal_record_invalid:index` |
| B20 | `for_each_index_match` with an entry that is not an index entry | `journal_index_invalid` |

Budgets (board §11 limits):

| # | Setup | Expected |
|---|---|---|
| B21 | Storage use would pass `storage_bytes` (the host's budget; 500 GB in the baseline profile) | stage and publish fail `journal_storage_budget_exhausted` with nothing written; a directory whose published use already exceeds `storage_bytes` fails `open` with `journal_storage_budget_exceeded` |
| B22 | Recovery chain over `max_recovery_bytes` | `journal_recovery_over_budget` |
| B23 | Main ledger usage during stage, publish, lookup, compaction, rebuild at full scale | never above the ledger limit; every buffer charged; after each call Main's `used()` equals its prior value (the page-cache carve is a constant part of it, charged at open) |
| B24 | Page cache with a carve of C bytes under a lookup storm | carve usage never above C; cached pages stay until evicted and are charged to the carve only; when a read needs carve budget, a page no reader holds is evicted and its bytes return to the carve; with every cached page held by a reader, the read is served on Main's account and not kept |
| B25 | Main ledger smaller than the requested carve | `open` fails `memory_budget_exhausted`; with 0, no cache and every lookup still correct |
| B26 | User input to first Recall, end to end (query tokenization, every index lookup including page-cache misses that read page logs, candidate judgment hand-off, replay of selected records), largest tested journal, the host's workers (16 in the baseline profile) | within 1 ms; measured only at step 10. A single lookup's time is not this measure |

## C. Experience (step 3; board §3B, §4, §5)

Append and replay (invariant 3, failure 1):

| # | Setup | Expected |
|---|---|---|
| C1 | Observations with every field set and with every optional absent, including failed outcomes, uncertainty 1, contradiction 1, empty structured, any raw bytes | All appended (no filter); replay returns every field exactly; `raw_digest` = SHA-256(raw) |
| C2 | Same observation twice (one call and two calls) | One address, one record; the second stage stages nothing for it |
| C3 | Same identity, other authored cues | `experience_index_conflict`; nothing staged |
| C4 | Same observation in a second transaction | Existing address; its transaction entry stays the first |
| C5 | Address = digest of (kind, source, revision, revised address, outcome, payload digest) | Recomputed address equals the stored one; a record whose address differs fails decode `experience_address_mismatch` |
| C6 | Lineage (revised address or derived_from) unknown, or naming a non-experience record | `experience_lineage_unknown` / `experience_kind_invalid`; nothing staged |
| C7 | Uncertainty or contradiction NaN, negative, above 1; -0 | invalid code; -0 stored as +0 |
| C8 | Authored cue with two tokens, uppercase, or punctuation | `experience_cue_not_a_token` |
| C9 | Resources repeated; lineage repeated | `experience_resource_duplicate` / `experience_lineage_duplicate` |
| C10 | Record with an index entry removed, added (non-cue), or altered | decode fails `experience_index_incomplete` / `experience_index_invalid` |
| C11 | Maximum sizes: 4096-byte source and revision of alternating classes, 4096 authored cues, 1024 lineage, 1024 resources | Staged and decoded; entry count within `max_record_index_entries` |
| C11a | Raw and structured bytes at 0, the inline size, one byte over it, one byte under and over each part-count boundary (1, 2, 65536, 65537 parts: depth 1 and 2), and a depth-3 size | Each stages and replays byte for byte through `for_each_raw_chunk` / `for_each_structured_chunk`; the envelope stays under `max_payload_bytes`; depth and top count are the ones the size gives; `raw()` on a parted blob fails `experience_blob_parted` |
| C11b | Every generation `next` stages | Parts before the experience record; no generation over either journal generation limit; the append is `done` only after its last record; memory held is at most one part per level while replaying |
| C11c | Crash (B2) after any part generation, then the same append again | Only unreferenced parts remain after the crash; the retry appends no part twice (equal parts are one record) and ends with the same addresses |
| C11d | `next` called after a generation it returned was not published | `experience_parts_unpublished`; no experience record staged |
| C11e | A part replaced, truncated, extended, reordered, with another kind, an index entry or authority; a top digest altered | reading fails `experience_part_invalid`, or decode fails `experience_address_mismatch` (the head's digest binds every top digest) |
| C11f | Two observations whose bytes share parts; one already in the journal | Each shared part is appended once; the parts of the existing one are not appended |
| C11g | A derived experience, its lineage original, derived and parted in root sources | Root sources are the sorted union of its lineage's; an original's is its source; a record whose root sources are not increasing fails `experience_root_sources_invalid` (parted) or decode (inline) |

Views (board §3B :122-123):

| # | View | Expected |
|---|---|---|
| C12 | cue | every token of source and revision and every authored cue finds the record; a token over 4019 bytes finds it through its digest entry |
| C13 | source, namespace, resource, transaction | the exact text finds exactly the records with it |
| C14 | content | the raw SHA-256 hex finds every record with those raw bytes |
| C15 | lineage | X finds every experience derived from X |
| C16 | successor | X finds the experience that revises X (validity: X is current iff none in U) |
| C17 | a key of the wrong form for its view | `experience_view_key_invalid` |
| C18 | rebuild_view after B10 | every C12-C16 answer identical to before the damage |

Selection `Select(q, U) -> (C, J, rho)` (invariants 3, 11; failure 2):

| # | Setup | Expected |
|---|---|---|
| C19 | Query whose tokens match some records | Candidates are exactly the records carrying a query token in U; the judge sees every one once; J holds every verdict, C every selected one, each replay-verified |
| C20 | Records published after U is read | never a candidate |
| C21 | No token matches | `experience_select_no_candidates` (no whole-universe fallback, board §9) |
| C22 | Judge selects none | `experience_select_nothing_selected` |
| C23 | Judge records no verdict, two verdicts, or a verdict for another address | `experience_judgment_missing` / `_repeated` / `_address_changed` |
| C24 | Retrieved entries over `max_retrieved` | `experience_select_over_policy`; no candidate dropped unjudged |
| C25 | The receipt digest | changes when any judgment, selected item, universe field, query or context changes; authority flags all false and inside the digest |
| C26 | Judge's strings freed right after `record` | receipt unaffected (copied on record) |

## D. Evidence wiring (step 4; invariants 4, 5; failures 3, 4)

| # | Setup | Expected |
|---|---|---|
| D1 | Admission of an observation citing a published experience | replayed and decoded from one snapshot with the HEAD generation; `applied` |
| D2 | Citing a non-experience record or an unknown address | decode / `journal_address_unknown` failure; tally unchanged |
| D3 | Same address admitted twice | `duplicate` (checked before stale); tally counts it once |
| D4 | Judged on a generation other than HEAD's | `stale`; tally unchanged |
| D5 | Expired, insufficient outcome | `expired` / `insufficient`; tally unchanged |
| D6 | Re-evidence at a new HEAD generation | only with the state of that generation (`re_evidence_state_not_current` otherwise); only an admitted original (`re_evidence_original_not_admitted`); the judge receives the decoded experience |
| D7 | HEAD published between admission and gate | the admission stays judged against the earlier generation; `evidence_current` false at the new one until Re-evidence there |
| D8 | Each negative gate alone, every other gate passing: (a) effective samples below the minimum, (b) source diversity below the minimum, (c) a required axis's source diversity below the minimum, (d) context diversity below the minimum, (e) regime change suspected, (f) upper bound below the threshold, (g) lower bound not above the threshold with the upper bound above it, (h) an invalid tally | (a)-(e), (g), (h) `abstain` with reasons `minimum_effective_samples`, `source_diversity`, `axis_source_diversity`, `context_diversity`, `regime_change_suspected`, `uncertain`, `invalid_input`; (f) `reject` with `upper_bound_below_threshold`; never `accept` (invariant 5) |
| D8p | Positive: supporting evidence with enough effective samples, sources, per-axis sources and contexts, no regime change | `accept` with `causal_lower_bound`, lower bound above the threshold |
| D8s | Stale, duplicate, claim-mismatched evidence (D3, D4, `evidence_claim_mismatch`) | not counted: the decision equals the one made without them |
| D9 | High producer confidence with no admitted evidence | no `accept` (I05) |
| D10 | Observation whose source family is not the family of any root source of its experience; whose context or step is not the experience's; an experience with no context | `evidence_provenance_mismatch:source_family` / `:context` / `:observed_at`, `evidence_context_unbound`; accumulator unchanged |
| D11 | Many experiences of one source, admitted under one family each time; then Main groups two sources under one family | source diversity counts the family once; an ungrouped source matches only its own name, and after one admission under it `assign` to another family fails `source_family_reassigned` |
| D12 | Many producers labelling experiences of one source and one context | source and context diversity stay 1 (each is the smaller of the verified count and the producer count) |
| D13 | A derived experience cited as evidence | its family may be that of any root source it rests on, never of a source outside them |

## E. Failure-model and invariant coverage map (steps 2-4 share)

| Rule | Conditions |
|---|---|
| I01 | A1, A2, B12 |
| I02 | A3, A5 |
| I03 | C1, C5, D1 |
| I04 | D5, D8 |
| I05 | A5, D9 |
| I06 | B8-B11, B13-B14, B21-B25, C6-C10, C21-C24, D2-D8 |
| I07 | B1-B7, C1, C5, C11a-C11f |
| I08 | D8, D10-D13 (source diversity is a proxy, no producer rank; provenance from the experience) |
| I10 | A3, C25 |
| Experience-authority separation | A3, C25 |
| Failure 1 | A1, C1 |
| Failure 2 | A3, C19-C25 |
| Failure 3 | D9 |
| Failure 4 | D3, D4, D8, D8s |

Failures 5-8 and 10, I03's write receipt, E001/E002 (F below) belong to steps 5-7 (codex); failure 9 (worker isolation) to step 8.

## F. E001/E002 (ARCHITECTURE_SPEC.md@5901a5a:154-162; steps 5-6, codex)

The spec reports E001 (empty evidence addresses) and E002 (an address the
decision was not produced from) as negative results of the old writer; the
rebuild must close them.

| # | Setup | Expected |
|---|---|---|
| F1 (E001) | Proposal with no evidence addresses, every other gate passing | Bind fails; no capability, no write, no-write receipt |
| F2 (E002) | Proposal citing an address outside the decision's admitted set, every other gate passing | Bind fails; no capability, no write, no-write receipt |
