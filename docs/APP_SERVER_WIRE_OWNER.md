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
