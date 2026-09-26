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
Host ordering/retransmission identity and recovery after a lost reply still need
implementation; repeated deliveries are not yet deduplicated. Hook JSON parsing
and framing precede this Runtime boundary. These tests do not demonstrate desktop
input-to-Recall below 1ms or complete desktop event coverage. No app configuration
was changed and no Claude connection was enabled.
