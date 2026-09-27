# Native event ingress and durable input cues

## Implemented path

Host protocol version 6 adds:

- `swegca/agent/attach`: provider, instance, session. Currently provider `codex`
  has a native parser. It returns the domain-separated identity and attaches the
  session without selection. Its original-experience session name is the native
  session string. Instance is provided by the host owner.
- `swegca/agent/attach/resume`: same binding, explicitly resumes its durable store.
- `swegca/select`: selects that returned identity on the serialized owner.
- `swegca/agent/event`: native (exact JSON string), sequence, observedAt, seed,
  step, optional candidateLimit. The event session must match the selected
  native binding before any record/receipt mutation.

The existing SWEGCA event kernel selects the route. UserPromptSubmit goes through
Runtime::receive_envelope: prompt bytes enter existing Déjà vu/Recall first, then
one complete native envelope is retained through the existing evidence admission,
shuffle, three-phase judgment and refinement. The cue comes from that Recall,
not a second host-supplied digest. Unknown native fields remain byte-exact. No
prose support/refutation is invented; unobserved outcomes stay insufficient.

Content and native lifecycle events are retained without replacing the current
Recall/Replay. SessionEnd, Stop and subagent/compaction notifications never invoke
VRS end. Only the explicit existing host end operation publishes that session for
Main work. Full Replay returns the original JSON bytes and original media/source;
partial Replay returns slices of those same bytes, not the extracted prompt.

## One original, one durable cue binding

The evidence record's existing 176-byte prefix uses byte 158 as an optional input
key flag (0 or 1); byte 159 remains zero. With flag 1, a 32-byte SWEGCA input cue
follows the prefix, before the original media and payload. The record checksum
and original address digest cover observation, cue, media and complete original
payload together. Flag 0 records retain their exact existing representation.
There is no separate prompt experience, sidecar index or migration process.

Session recovery, full decode, streaming decode, partial payload reads and Main
merge recover the same cue. Streaming verification still authenticates the entire
record before exposing a slice or sealed observation. The cue is lookup provenance,
not proof that a producer's contents or observations are true. Lower-level C++
recording accepts an optional bound key; the native host protocol never accepts a
caller-selected key and instead derives it with the preserved core cue primitive.

## Evidence and limits

- Runtime lifecycle tests: 171 checks, including a 70KB-plus envelope, a slice
  crossing the 64KiB read boundary, unchanged complete payload, temporary reopen,
  Main publication/reopen and prompt-based Recall with exact original addresses.
- Connection tests: 15,934 checks; original shuffled refinement vectors unchanged.
- Physical block tests: 424 checks.
- Real MCP subprocess tests: 1,533 checks, including differing metadata for the
  same prompt, full Replay bytes, wrong-session/invalid-event rejection, native
  lifecycle preservation, resume, and another installation reading accumulated Main.

This is the native event ingestion endpoint, not installed desktop capture.
Delivery ordering/retransmission identity and recovery after an unread committed
reply are implemented below. Other crash boundaries remain to be verified. Hook JSON parsing
and framing precede this Runtime boundary. These tests do not demonstrate desktop
input-to-Recall below 1ms or complete desktop event coverage. No app configuration
was changed and no Claude connection was enabled.

## Delivery replay and recovery (host protocol 7)

Native sessions now require sequence 0, 1, ... without gaps. The core
`route_agent_delivery` chooses append / reuse / reject from the session phase,
existing sequence, byte identity and previous sequence. Identity binds sequence,
original timestamp and exact native bytes. Processing seed/step do not create a
new delivery. A changed timestamp or changed bytes under the same sequence is
rejected. A genuinely new event with identical text and the next sequence remains
a new experience.

Before recording, the owner reserves its delivery index node. It acknowledges an
identical retry using the existing original, with `duplicate:true`, and performs
no record, shuffle, refinement or receipt invalidation. A still-current input
receipt is returned; otherwise `receipt:null`. After restart, old Recall/Replay
receipts are not reconstructed or falsely reported as live. The acknowledgement
confirms durable ingestion, not the pre-input candidate set lost with the process.
The client must treat this field accordingly.

Resume rebuilds the index from originals referenced by the committed session
connection heads. It does not depend on a delivery sidecar/database or raw
transcript. Only exact native-envelope source/media/session bindings are accepted.
Generic receive/retain/observe calls cannot bypass native session ordering.
Index recovery errors leave that attached context unselectable; reopen is required.
Other sessions retain their selection and receipts.

New-event fingerprint hashing is after its existing Recall/record path. Retries
hash before acknowledging so altered content cannot masquerade as a retry.
No payload hashing or disk scan for delivery identity is inserted ahead of new
input Recall. There is still host parsing, sequence validation and node reservation
before Runtime entry; desktop input latency remains unproven.

Recovery now verifies originals with a fixed 64KiB scratch buffer and retains
only sealed delivery identity, sequence and original address. The delivery index
still uses memory proportional to native event count, a remaining resource gap
for large sessions. It does not weaken the configurable budget or establish 4GB-scale
graph completion. A crash before catalog publication can leave preserved physical
orphan records; they are not committed experiences and are not acknowledged by
this index. Arbitrary interruption points still require targeted validation.

Verification includes same-process retry, altered payload/timestamp rejection,
out-of-order rejection, committed-original recovery, and an unread response followed
by SIGKILL/reopen/retry. The latter adds no stored bytes and the next input recalls
exactly one prior experience. Existing native lifecycle tests still require
explicit end before Main work.

Delivery verification result: MCP subprocess suite 1,699 checks; event parser/core
suite 140 checks. Recovery of an improperly bound store is rejected and cannot be
selected or ended. Duplicate retries leave the connection revision unchanged;
the next genuinely new event adds the existing append+refine revision pair and
preserves the insufficient-evidence strength. These are ingestion/recovery tests,
not model performance or desktop latency measurements.


## Streaming delivery recovery

`SessionRuntime::visit_deliveries` now reuses a session read cursor (at most one
open original block) and the private evidence streaming decoder. The record
sequence/time initialize the same core delivery hash used by ingress. The
original session, source, outer evidence media and inner payload media are
validated while streaming. Only original payload bytes enter the delivery hash;
observation headers and the optional lookup cue remain covered by the enclosing
record checksum and address digest.

No provisional chunks are exposed. `OriginalDelivery` is constructed only after
full trailer/address authentication and SWEGCA observation admission. The host
then inserts its existing retry index entry. This removes full-payload allocation
from resume without a second read or a separate sidecar. Read bandwidth still
scales with stored original bytes; the retry index still scales with event count.

A targeted Runtime case creates a 2MiB original, reopens it under a 256KiB VRS
tracked allocation budget, and verifies its exact delivery fingerprint without
increasing tracked retained memory or performing writes. Full Replay fails with
bad_alloc under the same budget. Session/source/media mismatches fail before a
visitor receives a result. The 64KiB stack scratch and ordinary process overhead
are outside that PMR counter; this is not a 256KiB process-RSS claim.

Streaming change verification: Runtime 180 checks, physical block 424 checks,
MCP subprocess 1,767 checks in this run (async polling affects the counter).

## Explicit end by session identity

The host accepts `swegca/end` with optional `identity`. When supplied, the
identity must name an attached session; invalid, malformed, or already detached
targets fail without falling back to the currently selected session. With no
identity, the selected-session behavior remains, and no selection is an error.

Runtime::end_session(identity) runs the existing SWEGCA session end/publication
transitions for that owner alone. Closing an unselected attached session does
not select it, invalidate another session's Recall/Replay, or close that other
session. A failed close also leaves the other selection intact. Transport
context is erased only after the targeted Runtime close succeeds. Named close
works with no selected session as long as its target is attached.

The close publishes the original/graph source into the existing durable work
queue. It does not itself run work or schedule a merge. Existing work/start,
work/poll and work operate only on published ended sources. EOF, idle, turn
completion and native SessionEnd notifications still do not substitute for
this explicit host operation. Desktop wiring of the actual user end action
remains separate unfinished work.
