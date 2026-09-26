# Codex app-server event adapter

The installed desktop backend was rechecked as `codex-cli 0.155.0-alpha.9.2` at
`/usr/lib/chatgpt/resources/codex`. Its generated TurnStartParams and TurnSteerParams
schemas contain threadId and an input array; steer additionally requires
expectedTurnId. ItemCompletedNotification and AgentMessageDeltaNotification carry
threadId. Protocol background: https://learn.chatgpt.com/docs/app-server .

Host protocol 8 accepts `protocol:"app-server"` on agent attach/resume (default
remains hook). This binds the source as codex/app-server. A session cannot switch
protocols on resume: authenticated delivery recovery checks the stored source.
No fake hook wrapper or invented UserPromptSubmit event is produced.

`adapt_codex_app_server` preserves the exact native JSON. Known turn/start and
turn/steer requests produce input facts for the existing SWEGCA event router.
The complete input array is encoded as an exact structured lookup cue, with its
own media domain. This preserves boundaries between text items and all image,
audio, skill, mention and unknown fields. It does not concatenate selected text
or fetch files/URLs. Request IDs, turn IDs and outer request metadata stay in the
original event but do not change the input-array lookup cue. Whitespace is
normalized for this cue; JSON property reordering is not semantic normalization.

Other method envelopes with explicit params.threadId are retained as content.
Item deltas and completions, turn completion and thread archival do not authorize
VRS end. The same original storage, shuffle/core judgment, receipt preservation,
retry identity, streaming recovery and explicit Main publication paths apply.
No support/refutation outcome is invented from output prose or delivery success.

## Scope boundary

This adapter is implemented and exercised through a real VRS MCP subprocess; it
is not yet installed between the live desktop and its backend. Responses without
threadId, thread creation responses and application-level messages need the
transport owner's request-ID/session correlation. The parser rejects missing
bindings instead of assigning them to whichever session happens to be selected.

Receiving both item deltas and item/completed is not proof of episode-level
assembly or independent new evidence. References to image/audio files preserve
references, not fetched attachment bytes. Complete capture, item lineage and
fragment assembly remain work for the actual transport owner. No desktop files,
hook trust, app process, model request or account settings were changed.

Verification: event adapter/core suite 174 checks; MCP subprocess suite 1,810
checks in this run. Scenarios include multimodal input preservation, steer/start
cue agreement, byte-exact Replay, output events retaining the current receipt,
no merge on turn/archive messages, restart deduplication and explicit end/merge.

## Request/response correlation component

`AppServerRequests` now provides a bounded, Main-host-owned table for one app-server
wire connection. Pending keys distinguish request origin (client/server) and ID
type (string/signed int64, as generated RequestId specifies). A request's explicit
threadId supplies its session. Conflicting reuse of a live ID is rejected; exact
re-registration is harmless. Capacity is supplied by its owner and allocations
use the shared memory resource.

`bind` parses an unchanged result/error response and returns a move-only response
with an internally bound AgentEvent session. It does not remove the pending entry
or infer a VRS verdict/end. The owner must durably ingest the raw response, then
call `recorded`. Generations prevent late acknowledgements from erasing a new
request that reused an ID. Unique owner identities also protect against another
connection and object reconstruction at the same memory address. Failed parsing,
allocation or unknown-ID lookup preserves the pending request.

The live wire transport remains uninstalled. MCP response ingestion with an
explicit committed request proof is now implemented below; unbound responses
remain rejected. Requests that create a new thread remain integration work. A `recorded` call is the trusted
owner's acknowledgement, not an independently verified durable-storage receipt.
No connection, model turn, app setting or desktop process was started by this work.

Correlation verification: 38 focused checks (bidirectional/type collisions,
late/foreign tickets, same-address reconstruction, int64 boundaries, capacity and
allocation failure), 174 event adapter checks, and 1,810 MCP subprocess checks.

## Committed-request response ingress (host protocol 9)

The MCP native event endpoint now accepts `requestSequence` for an app-server
response. This is an explicit earlier committed delivery in the same bound
session, not a caller-supplied thread guess. The owner reads that exact original
through SessionRuntime/SessionStore, verifies its native source/media/session,
parses the original request, and uses AppServerRequests to match the response ID.
Notifications without IDs, response originals, unknown sequences, later deliveries
and mismatched IDs cannot serve as request proof.

The response's exact original JSON is then appended to the request's existing
connection through Runtime::observe and the existing shuffle/core/three-phase
refinement. The observation stays insufficient; JSON-RPC success/error is not
translated into evidence support/refutation or execution authority. Its context
field contains the request original's address digest, preserving the lineage
under the same observation checksum. The current Recall/Replay is retained.

Authenticated streaming delivery recovery now also returns the already-stored
observation context. A retried response must match its bytes/timestamp AND the
same request original digest, including after restart. An old response cannot be
reattached to a newer or older request merely because its JSON-RPC ID was reused.
No migration or new persistent sidecar is involved.

This endpoint uses a one-request matcher reconstructed from the committed
original. It therefore accepts a pending response after restart without trusting
a lost in-memory table. The live wire owner's table remains responsible for
choosing the correct requestSequence, observing which peer sent a frame, and
preventing conflicting live wire IDs. The proof authenticates an explicit
request/response association; it does not itself authenticate the physical peer.
The synthetic client/server roles in this single-request matcher are lookup roles,
not an inferred assertion about physical network direction.

The native request original is currently decoded in full when binding its reply;
large request envelopes can hit the configured allocation/read limit. Ordinary
delivery-index recovery remains streaming. Actual desktop interception, thread
creation/global messages, attachment bytes and item-fragment lineage remain
unfinished; this endpoint is not a claim of complete live capture.

MCP verification covers response ID mismatch, absent request proof, response-as-
request rejection, reused IDs with distinct original lineage, same-process and
post-restart retry, a response arriving after restart, exact response Replay from
the remembered request connection, and explicit-only Main publication.

Verification for protocol 9: MCP subprocess suite 1,942 checks, Runtime lifecycle
suite 180 checks, request binding suite 38 checks; git diff whitespace check passed.

Automatic session/sequence/request selection and VRS-confirmation-before-forward
logic are now provided by AppServerWire. Real MCP round-trip verification and the
remaining live process-I/O boundary are documented in APP_SERVER_WIRE_OWNER.md.

The installed schema's `ThreadStartedNotification` has `params.thread.id`, not
the ordinary `params.threadId`. The adapter now preserves that complete
notification as lifecycle content. AppServerWire accepts it only from the server
and can attach its new session through the VRS owner before recording it. Input
events cannot trigger this storage discovery. See the lifecycle-driven session
attachment section in APP_SERVER_WIRE_OWNER.md for constraints and verification.
