# Serialized app-server delivery owner

`AppServerWire` composes the native event adapter and bidirectional request table
for one wire connection. It automatically selects an attached session from a
request/notification's explicit threadId, or from a response's registered request.
It assigns the next session delivery sequence and adds the committed request's
sequence to response ingestion parameters. Direction is an observed transport
fact supplied by the process-I/O owner, never guessed from the selected session.

The host attach/resume result (protocol 10) now includes `nextSequence`, computed
from the authenticated contiguous delivery index. A wire owner attaches that
session at this value. It can restore an authenticated outstanding request with
its direction and earlier committed sequence. It does not discover pending wire
requests by itself, nor infer an unknown response's session.

## Ingestion/forwarding contract

1. `prepare` parses a native frame, resolves its session, and creates a move-only
   plan. Unknown sessions/responses and exhausted configured capacity fail.
2. `parameters` provides the exact `swegca/agent/event` fields, including sequence,
   timestamp, unchanged native bytes and optional requestSequence. The process
   owner selects the matching VRS session and sends that request.
3. Only after the matching VRS acknowledgement does the process owner call
   `recorded`. It registers requests with their committed sequence or retires the
   matched response request; only then does the session sequence advance.
4. `forward` refuses unconfirmed plans and returns the original byte-exact frame
   after confirmation. It is eligibility for forwarding, not proof of downstream
   delivery or a once-only network send.

Stale/foreign plans cannot advance another session. Unique shared owner identity
survives plan lifetime. Source input must come from the client direction. Pending
request capacity is checked before preparing a new request. Request-registration
allocation failure leaves the plan unconfirmed/retryable. Calls are serialized;
the process owner must not persist competing plans with the same session sequence.
All evidence judgment/storage remains in the existing SWEGCA/VRS path. This owner
implements transport routing, not another evidence classifier or lifecycle judge.

## Verified and unfinished boundaries

The C++ test executable can act as an exchange fixture. The Python integration
suite feeds the C++ owner's generated parameters into a real VRS MCP subprocess,
waits for its successful record reply, and then acknowledges the C++ owner. It
checks byte-exact forwarding for requests and replies in two sessions where client
and server use the same ID. Reopen reports sequence 2 for each session; no Main
merge occurs until explicit end.

The real desktop/backend pipe/socket pump is not installed or running. Thread
creation/global messages, authenticated reconstruction of outstanding wire
requests and their directions, partial writes/disconnect recovery, attachment
contents, fragment lineage and actual desktop input latency remain incomplete.
The wire owner parses each frame once and transfers its parsed representation
privately into the adapter/binder. This avoids duplicate parsing, but is not
evidence that desktop input-to-Recall is below 1ms.

Verification: wire owner 29 checks, request binding 38 checks, real MCP subprocess
suite 2,080 checks including the C++ owner exchange. The fixture uses unbuffered
binary pipes so readiness checks cannot overlook lines buffered by Python's text
reader. No desktop/backend connection was started by these tests.

## Stream socket forwarding

A confirmed Delivery can now bind one SOCK_STREAM endpoint and send its exact
native JSON plus one LF delimiter through `send_ready`. Binding duplicates the
fd with close-on-exec; closing/reusing the caller's fd cannot redirect a partially
sent frame. The original socket flags and global SIGPIPE disposition are not
modified. MSG_DONTWAIT and MSG_NOSIGNAL make each send attempt nonblocking and
prevent a closed peer from terminating the process.

The Delivery retains its successfully sent byte offset through partial sends,
EAGAIN, EINTR, moves and peer failure. Completion closes the owned descriptor and
repeated completion checks produce no additional bytes. A bound or partly sent
frame cannot be rebound to another socket. The original native body must be a
single JSON line without a delimiter; pretty-printed/multiline bodies are refused
instead of rewritten.

Each observed wire direction permits only one active frame. A second frame cannot
interleave with a partial first frame. Destroying an unsent frame releases that
lane. Destroying a partially sent frame leaves the lane unavailable for the
remaining owner lifetime, because skipping its suffix would corrupt framing.
No automatic reconnection, replay of a possibly delivered request, or inference
of downstream execution success is implemented. The actual process-I/O owner
must bind the correct endpoint for each direction.

Verification: 35 socket checks using Unix socket pairs, a 512KiB message and a
small kernel send buffer. Tests include real backpressure/partial sends, injected
EINTR, moving a partial frame, caller-fd closure, duplicate completion, competing
frames, abandoned partial frame and peer closure without SIGPIPE termination.
Wire owner checks remain 29 and real MCP round-trip suite passes 2,080 checks.
These isolated sockets do not connect to or modify the running desktop/backend.

## Bounded frame reception

`SocketFrames` owns an exclusive duplicated stream descriptor and reads at most
one 4KiB staging chunk per poll. LF-delimited frame bodies remain byte-exact. A
ready frame is held until the owner consumes it; additional frames received in
the same chunk stay buffered. Fragmented input is never exposed as a complete
frame. Socket EOF does not end a VRS session.

The current line is allocated through the shared memory resource. If line growth
fails after recv, the bytes remain in fixed staging and the cursor is unchanged;
a later retry can continue without re-reading or losing those bytes. Frame-size
violations or EOF with an unfinished line make that reader terminal. It does not
silently discard an oversized line, resynchronize to a later message, or forward
a truncated input. The 4KiB staging array and normal object/process overhead are
separate from the PMR counter. Partial/fault transport data are not yet persisted
as VRS experiences by the uninstalled live process owner.

Verification: 40 reader checks cover empty socket, injected EINTR, split/coalesced
frames, stable acknowledgement, exact size limit, empty line, oversized stream,
truncated EOF and allocation-failure retry with a 9KiB line. The C++ exchange
fixture now receives original frames through a Unix socket/SocketFrames and,
after actual VRS MCP acknowledgement, sends them through the production partial-
write implementation into a second SocketFrames endpoint. Automatic A/B response
routing and byte-exact delivery still pass: 29 owner checks and 2,080 MCP checks.

These are isolated process/socket fixtures. The running desktop, its backend and
any real model/account calls remain untouched. Live endpoint attachment, global
and new-thread lifecycle frames, outstanding-request recovery and full content
assembly are still required for actual desktop integration.

## Composed duplex pump

`AppServerPump` now owns both stream readers and composes the wire owner and
partial-write stages. Its event-loop caller steps each observed direction fairly
and supplies the existing VRS RPC acknowledgement callback. The pump preserves
separate received/prepared, ingestion-confirmed, request-table-confirmed and
socket-bound states. A retry after request-table allocation failure does not
invoke an already successful ingestion callback again. Unacknowledged ingestion
keeps the same original, sequence and observation timestamp for retry.

One unrecorded ingress is serialized across both directions, preventing two
frames from obtaining the same session sequence while an acknowledgement is
outstanding. Once a frame is recorded, its partial forwarding can coexist with
another direction's ingestion. EOF is reported to the process owner without
ending VRS or deleting outstanding requests. Unsupported/global frames remain
held by the reader when adaptation fails; they are not silently forwarded.

The exchange fixture now uses the pump directly: original frames enter its
client/server sockets, its callback submits generated parameters to a real VRS
MCP process, and its socket output is checked at the correct opposite endpoint.
The old separate manual receive/prepare/send exchange fixture was removed.

This is the reusable I/O loop, not an installed desktop proxy executable. The
caller still needs actual endpoint selection, authenticated session attachment,
matching-RPC acknowledgement handling and outstanding-request reconstruction.
The observation timestamp is the supplied frame-preparation timestamp, not an
instrumented first-byte arrival time; no input latency result is inferred from it.

Verification: 42 duplex-pump checks and 2,080 stdio subprocess checks pass.
The pump checks include rejected/throwing ingestion, opposite-direction ordering,
byte-exact forwarding, response routing and EOF with outstanding requests. An
injected exhausted allocation budget after successful ingestion proves that
request-table recovery retries without invoking ingestion a second time.
These counts cover isolated sockets and the actual VRS subprocess, not a live
desktop installation or the input-to-Recall latency requirement.

## Matching actual VRS acknowledgements

`AgentEventCommit` owns a serialized select/event RPC transaction on an already
initialized, exclusively owned VRS connection. The process owner supplies the
authenticated session identity, the wire delivery's parameters, and a unique
RPC ID namespace for that connection. Selection must be acknowledged before
the event request is exposed. IDs for the two stages are distinct strings.
The owner must not interleave session-selection mutations from other clients
between them. This is transport plumbing; all experience judgment remains in
the existing SWEGCA-backed VRS endpoint.

Responses must use JSON-RPC 2.0, match the exact expected string ID, contain a
result object and contain neither a method nor an error. The selection result
must be empty. The event result must contain a well-formed original location
(block/digest, offset and positive length with no extent overflow). Invalid
or error responses leave the pending request unchanged. A full event response,
including any Recall receipt, is retained before completion is exposed. Its
storage allocation must succeed before the pump may acknowledge ingestion.
This authenticates no endpoint by itself and proves no downstream execution;
it relies on the process owner's trusted, exclusive VRS connection.

The socket/process fixture now exchanges the generated RPCs and actual endpoint
responses, replacing its former `recorded` acknowledgement string. It also
discards one successful event reply, retransmits the same request, and checks
that VRS returns the same original with `duplicate: true` before the pump
forwards the native frame once. Response matching rejects stale IDs, error
replies, incorrect result types and malformed/overflowing locations. Requests
are encoded as single JSON lines while preserving native string contents.
The production process owner, initialization, endpoint attachment and durable
outstanding-request reconstruction are still not installed.

Verification after this integration: 87 combined pump/acknowledgement checks and
2,100 stdio subprocess checks pass, with builds and runs restricted to CPUs 6,7.

## Executable stream owner

`make build/swegca-app-server-proxy` now builds the actual stream-owner process:

```
swegca-app-server-proxy CLIENT_FD SERVER_FD VRS_FD CONFIG
```

The launcher must supply three distinct, exclusive connected stream descriptors
above 2. The VRS descriptor is the dedicated stdin/stdout channel of a fresh VRS
MCP process, not a second reader on another client's connection. No listening
port, backend replacement or running desktop configuration is installed by this
executable. Startup initializes protocol 2025-06-18, requires host-input version
10, sends initialized, and attaches or resumes the configured native sessions.
Only then does stdout emit `ready`. Stdout/stderr carry status, not native content.

Example configuration (all resource integers are decimal strings):

```json
{
  "memoryBytes": "8388608",
  "frameBytes": "65536",
  "pendingRequests": "64",
  "instance": "local-desktop",
  "seed": "7",
  "step": "0",
  "sessions": [{"session": "native-thread-id", "mode": "attach"}]
}
```

Use `resume` only for an existing retained VRS session. A resumed sequence comes
from authenticated VRS history, but outstanding wire request direction/IDs are
not reconstructed yet: an unmatched resumed reply is rejected, not guessed.
The configured seed/step pass directly to existing refinement. The process does
not derive evidence validity or expiry from wall-clock transport timestamps.

The main loop services both native directions. Buffered coalesced frames are
drained without waiting for a new socket edge. Partial writes wait for the
destination's writable state. VRS RPCs use synchronous backpressure and poll;
no model is invoked. EOF half-closes only the corresponding outgoing stream,
allowing the opposite side to finish responses. Neither EOF nor process failure
issues `swegca/end` or `swegca/work`. Endpoint error/disconnection stops forwarding.
No automatic reconnect, partial-stream resend or lossless crash recovery is
claimed. Uncaptured failing frames are not durably spooled by this executable.

The proxy allocation budget is additional to the VRS process's budget. It does
not prove a combined 4GB RSS ceiling; a common cgroup/launcher is still required.
The default socket wait has no timeout and applies backpressure until endpoint
progress, closure or process termination. The input-to-Recall requirement is
unproven for this IPC path. Runtime Recall receipts are checked/retained within
the commit but are not yet injected back into the agent's context.

Verification: 87 pump/acknowledgement checks and 2,149 subprocess checks pass.
The real executable and VRS process exercise split/coalesced native frames,
two sessions, same-ID requests in both directions, exact forwarded bytes,
half-close shutdown, persisted sequence recovery and no EOF-triggered merge.
A second resumed run kills only its test VRS child: the proxy exits with an
error and forwards zero bytes of the new native input.

Remaining desktop work includes actual endpoint installation, dynamic/new-thread
and global lifecycle capture, attachment payloads, outstanding-request recovery,
Recall/Replay delivery to the agent, complete content assembly and latency proof.

## Lifecycle-driven session attachment

The wire adapter now recognizes the installed desktop schema's `thread/started`
notification and reads its session from `params.thread.id`. This is a lifecycle
experience, not user input or a VRS end instruction. Its original native bytes
and unknown thread fields are retained. Conflicting `threadId`, empty identity,
an attached request ID, or a client-originated started notification are rejected.

An unknown session may be attached through the wire owner's lifecycle callback
only for this server notification. The callback is not called for an unknown
`turn/start` input; session creation/recovery I/O is not added before every user
input. Once attached, ordinary inputs take the existing route without invoking
the lifecycle callback again. The known-session limit is checked before the
callback can create durable state.

`swegca/agent/attach/ensure` performs idempotent host attachment. Main checks its
owned source registry and canonical session directory to choose create or open;
storage errors are never interpreted as missing sessions. Existing original
experiences reconstruct the next sequence through the normal authenticated
delivery reader. Native source/protocol must match. A core session-phase check
rejects unusable or ended sessions before committing a new route; rejection
releases the source lease, so explicit-end Main work remains possible.

The proxy accepts optional `sessionCapacity` (decimal string). Without it the
limit remains the initial `sessions` array length. With a positive explicit
capacity that array may be empty. Upon an unknown server `thread/started`, the
proxy ensures the VRS session, binds the returned sequence, records the full
notification, and then forwards it. Restarted sessions are resumed in the same
way; the notification does not reset their stored sequence or revive an ended
VRS session. No user input, model request or desktop restart is synthesized.

Verification: 181 adapter checks, 41 wire-owner checks, and 2,225 subprocess
checks pass. A real proxy/VRS run adds a third session after readiness, records
its notification/input/response, and later rediscovers it at the persisted
sequence. Repeated ensure is idempotent; a mismatching source is rejected.
After explicit end, two ensure attempts fail and the ended source still merges
normally. The initial `thread/start` request and other global messages without
a native session binding remain unsupported: lifecycle notification support is
not complete new-thread handshake or live desktop integration.

## Single-request native ingress (host protocol 11)

The earlier select/event transaction has been removed from `AgentEventCommit`.
The owner now places the authenticated session `identity` in the existing native
event parameters. `swegca/agent/event` selects that already attached in-memory
route and processes the event in the same serialized host call. Unknown,
malformed and incompletely recovered targets are rejected. Native session/source
checks and all original recording/Recall/deduplication behavior remain in the
existing VRS path. The identity is transport metadata, not additional evidence.

This reduces each proxy ingress from two VRS RPC round trips to one and removes
the inter-call selection gap. There is no per-input storage discovery or new
model call. Standalone `swegca/select` remains available to other host operations;
native events without identity retain their existing explicitly selected-route
behavior. The proxy requires host-input protocol 11 so it cannot silently assume
targeted semantics from a previous endpoint that ignored the field.

Verification: 88 pump/acknowledgement checks and 2,213 subprocess checks pass.
The exchange driver now forwards exactly one generated event RPC per delivery,
checks its target, rejects absent/wrong/malformed bindings, and still exercises
lost acknowledgement/deduplication. The lower subprocess assertion count reflects
removing checks for the deleted selection round trip. No latency claim follows
from this structural change: the preserved over-1ms observations remain open.
