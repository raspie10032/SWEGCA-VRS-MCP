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

This component is not yet wired into the live transport or MCP ingress; therefore
responses without explicit threadId are still rejected by the standalone event
endpoint. Durable reconstruction of outstanding request correlations and requests
that create a new thread remain integration work. A `recorded` call is the trusted
owner's acknowledgement, not an independently verified durable-storage receipt.
No connection, model turn, app setting or desktop process was started by this work.

Correlation verification: 38 focused checks (bidirectional/type collisions,
late/foreign tickets, same-address reconstruction, int64 boundaries, capacity and
allocation failure), 174 event adapter checks, and 1,810 MCP subprocess checks.
