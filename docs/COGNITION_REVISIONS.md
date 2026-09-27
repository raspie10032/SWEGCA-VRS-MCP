# Immutable cognition revisions

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
initial record; it is not silently replaced by the later result. Automatic
latest-revision discovery across restart is not yet implemented. For an ended/published session, use `inputOriginal` (the complete recorded address)
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
These records use the existing 64KiB cognition metadata bound; large comparison
address lists and automatic latest-revision navigation remain limitations.
Generic host fixtures outside native session bindings still do not automatically
persist cognition, as before this change.
