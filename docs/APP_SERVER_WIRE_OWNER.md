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
