# Immutable cognition revisions

## Scoped comparison revisions (2026-09-27)

Scoped Replay now saves its selected original, source session, exact scope,
scope connection, observation boundary, seed/step and complete exported
assessment prefix. These are derived records, not new evidence samples.
The existing immutable revision blocks are reused. A domain-separated channel
head in `cognition-latest` keeps each `(inputOriginal, scope)` latest pointer
separate from the parent's latest pointer. Publication uses the existing sealed
block, hard-link, rename and directory-sync sequence; ended inventory includes
all channel heads under its existing canonical filename checks.

Example Main-owner query:

```json
{"jsonrpc":"2.0","id":1,"method":"swegca/agent/cognition","params":{"identity":"<source session>","inputOriginal":{"block":"<block>","offset":"<offset>","bytes":"<bytes>","digest":"<digest>"},"scope":"byte content unchanged","latest":true}}
```

An explicit `revision` can replace `latest:true`. Scope queries require one of
these selectors. The reader validates the stored scope as well as the input
binding; a scoped revision cannot be read as a parent revision or as another
scope. `liveRevision` is present only for the corresponding completed, saved
in-memory scoped comparison. Merely reading a saved record does not create a
live comparison or Re-evidence receipt.

The first successful scoped Replay persists before replying. Later admitted
native counterevidence refreshes and persists its scoped comparison before ACK.
A failed save does not set the saved flag. Repeated queries of an unchanged
saved comparison do not republish or add observations. Parent cognition and its
latest pointer are not changed by these scoped publications.

After restarting an active native session, retransmitting its original input
obtains the existing historical receipt. `vrs_replay` with that receipt and
`scope` exports the stored latest scoped assessment and its exact selected
original, marked `historical:true`, with its revision digest. It reads the
authenticated original, not a fresh candidate or the present filesystem state.
Ended sources support the owner query above before/after Main merge without
reattaching an input route.

**Remaining boundary:** a saved scoped comparison is an archived result.
Automatic reconstruction of a live route-issued comparison from its stored
observation boundary, and automatic continuation against further evidence after
process restart, are not implemented here. Stored JSON is not treated as a
new authority-bearing receipt. Natural-language scope interpretation, installed
desktop use and full resource/latency gates remain incomplete.

Verification: CPU 6,7 / make -j2; **5232 stdio checks**, **512 session checks**.
The stdio test reads the scoped journal *before* another scoped Replay to prove
late-conflict persistence before ACK, verifies immutable repeated reads, scope
mismatch rejection, unchanged parent latest, restart/historical export and
ended/Main reads. Storage tests cover independent channel heads, reuse without
new samples, restart/end inventory, failed rename, failed record/directory sync,
and explicit retry through reopen without altering the parent channel.

The native Main host persists the initial input-time cognition at its existing
input key. Later successful automatic comparisons and explicit Re-evidence of
the current automatic Replay publish immutable revisions through Runtime,
SessionRuntime and SessionStore. Partial payloads never authorize comparison. A manually selected full Replay
can now persist its successful core comparison as a revision without replacing
the automatic cognition selection.

Each revision is bound to the committed input extent, original digest, session
identity and SHA-256 of the metadata. It includes inputOriginal, seed, step,
memory summary, selectedOriginal, sourceSession and the stored replayPrefix.
The prefix captures the same core assessment exported with the selected bytes.
It is a JSON prefix ending at the opening contentHex quote, not a complete JSON
object or executable code. No model judgment or new support/refute label is
introduced. Derived assessments do not increment original observations or
refine their own connection strength.

Repeated reads of an already saved snapshot do not write. Recomputed identical
metadata selects the same revision, verifies it and reuses it. Save failure
cannot mark the snapshot saved; a later default full Replay must complete the
pending persistence before exporting that snapshot. The existing storage quota,
record checksum, explicit session-end inventory and publication rules apply.

## Main-owner query

After attaching/resuming the native session, send a JSON-RPC host request:

```json
{"jsonrpc":"2.0","id":1,"method":"swegca/agent/cognition","params":{"identity":"<attached session identity>","sequence":"2"}}
```

This returns the immutable initial `record`, `revision:null`, and `liveRevision`:
the digest of the currently completed and persisted later snapshot, if it
belongs to that input. A stale/incomplete snapshot does not expose a live digest.
The request leaves the selected input session unchanged.

To retrieve a known revision, add `"revision":"<digest>"` to params. `record`
then contains that revision and `revision` echoes its digest. The reader checks
the sealed record, result hash and metadata input binding. A digest belonging
to another input, unknown sequence, missing record or unbound session is an
error. Lookup does not run a new comparison or grant Re-evidence authority.

After process restart, the owner can attach/resume and retrieve a retained
revision digest. The original duplicate-input Replay still uses the historical
initial record; it is not silently replaced by the later result. Latest published revision discovery is available with `latest:true`, mutually
exclusive with an explicit revision digest. If no revision has been published,
that query reports not found; omitting both selectors still reads the initial
input-time record. For an ended/published session, use `inputOriginal` (the complete recorded address)
instead of `sequence`, with the same source session `identity` and optional
`revision`. This reads the Main-owned store without attaching or reopening an
input route, including after Main merge and process restart. Supplying both
locators is rejected. Active unmounted sources still require their normal attach
flow. No claim of complete revision navigation or installed desktop use is made.

Verification uses actual stdio subprocesses: automatic response update and
explicit comparison, preserved initial Replay, different-input/unknown-digest
rejection, repeated-read storage size stability and exact revision retrieval
after restart. Storage-layer tests additionally cover duplicate saves, invalid
addresses, size bounds and end-of-session immutability.

Ended-session regression additionally verifies queries before and after Main
merge, after restart without session attachment, rejection of unknown sources
and forged addresses, and continued input to an unrelated selected session.
The closed session does not gain an input route from these reads.

## Explicitly selected full Replay

For a native session, vrs_re_evidence on an authenticated manually selected
Replay persists inputOriginal, seed, step, sourceSession, selectedOriginal and
`comparison`. The latter is the complete core comparison response (including
currentOriginals), before adding the returned `revision` field. Re-evidence
still runs only for a core-verified conflict; an insufficient comparison is
recorded as insufficient. No new evidence verdict is inferred from persistence.
Automatic explicit comparisons also return their saved revision digest.

The same agent/cognition query retrieves a manual revision by digest, including
after session end, Main merge and restart. Repeated identical calls reuse the
same record. A failed or partial Replay cannot publish a manual comparison.
The fixed 64KiB metadata ceiling has been removed. Content size is bounded by
the configured VRS MemoryBudget minus the encoded record envelope, with capacity
overflow checks. Actual reading allocations still use the shared available PMR
budget; admission by size does not reserve memory or promise that a full read
will succeed under simultaneous use. Storage remains under StorageBudget. Large
comparison address lists still materialize in memory, and streaming comparison export remains unfinished.
Generic host fixtures outside native session bindings still do not automatically
persist cognition, as before this change.

A 131,073-byte revision is preserved exactly through save/read and restart.
An exhausted shared MemoryBudget rejects the read without poisoning the store;
releasing the held allocation permits an exact retry. An encoded record exceeding
the configured memory limit is rejected before hashing/writing. This is bounded
metadata handling, not a proof of whole-process 4GB RSS or arbitrarily large
streaming comparison export.

## Latest published revision

`save_cognition_revision` first seals the immutable record, then hard-links it
into a staging name under cognition-latest and atomically renames that alias to
the original-input key. The source record has already been synced; the alias
directory is synced before success is returned. An identical currently selected
record is reused without a rename, but the containing directories are synced
again before acknowledgement to complete any interrupted prior publication. Explicitly republishing an older existing
result makes it the latest successfully published result without duplicating its
physical payload. Latest means publication completion order, not highest step,
file modification time, semantic confidence or a full chronological event log.

The link aliases the already charged inode. Metadata and filesystem overhead
remain outside the logical data-byte budget, as in other hard-link publications.
Latest aliases enter the ended-session inventory under a distinct domain.
Staging names are not published records. Open/read verifies the sealed result
and its binding to the supplied original input, rather than trusting the alias
name. Interrupted publication can leave a sealed revision not yet latest; retry
can finish the alias publication. Publication errors make the active store
unusable until recovery. Earlier revision files and the initial record remain.

Host example: params containing identity, inputOriginal and `latest:true` return
both the selected revision digest and its record after restart without a prior
client-held digest. No input route or new judgment is created by the query.

## Publication failure retry

Session tests wrap rename and fsync at the actual latest-alias and canonical
record directories. They inject errors before alias replacement, after alias
replacement at directory sync, and after linking the sealed canonical record.
The failure poisons the active store, preserving initial and older results.
Reopen can see the old or newly linked latest result according to the reached
boundary. A retry must sync the canonical record directory, its session parent
and the latest directory even when file identity already matches. Repeated sync
failure is not reported as success; a successful retry adds no payload bytes.
This is syscall fault/reopen coverage, not a physical power-loss experiment.


## Bounded subtree validation during retrieval

The host validates the metadata with parse_json_selected and retains only
inputOriginal for address binding. It still validates complete JSON syntax,
UTF-8, escaped code points, duplicate object keys, trailing input and nesting
limits. Skipped array members are released as they are checked; skipped strings
and numbers are validated without keeping their decoded values. Object keys
remain necessary for duplicate detection, so parser space still follows object
width and selected subtree size. The original stored bytes are not rewritten.

A regression containing a 1MiB string and 20,000 two-field objects fails whole-
tree parsing under 4KiB, while selected parsing succeeds under the same budget.
The host appends the original record directly into its final response allocation
with overflow-checked reservation, avoiding another full temporary string.
Stored record and final response buffers still exist; this is not fully streaming
file-to-wire export or a whole-process memory bound.
