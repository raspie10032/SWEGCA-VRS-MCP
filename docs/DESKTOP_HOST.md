# Desktop stdio process host

The installed application at `/usr/lib/chatgpt/resources/app.asar` contains
`CODEX_CLI_PATH` selection in `.vite/build/src-C3YaUE83.js` and
`.vite/build/main-DUHZj4_w.js`. The local daemon path is conditional on that
override being absent. This is evidence about the inspected installation,
not a promise about other app versions. The installed `owl-app.ini` reports
26.915.31945. No app configuration or installed executable was modified.

`swegca-desktop-host` is a C++ process/stdio adapter for the existing proxy:

```
swegca-desktop-host PROXY VRS MODE ROOT VRS_CONFIG PROXY_CONFIG BACKEND [ARGS...]
```

Executable arguments are exact executable paths; no shell is invoked. It owns
three children: the supplied backend, VRS and proxy. Three private socketpairs
connect them. A separate readiness pipe absorbs the proxy's `ready` line so
stdout contains only native backend frames. Client stdin/stdout are relayed
with two fixed 64KiB buffers and nonblocking readiness handling. Original
framing and payloads are not changed here; SWEGCA decisions remain in VRS.
Readiness polling also checks child exits. EOF half-closes the client direction
and drains responses. SIGINT/SIGTERM or an error closes and reaps only children
started by this host, with bounded termination cleanup. EOF is not a VRS end.

Verification: real launcher + proxy + VRS binaries and a synthetic Python
backend fixture, invoked on CPUs 6/7. Native initialization, thread creation,
dynamic binding, user input and responses traverse desktop-style stdin/stdout.
Reopen confirms five connection originals, three thread originals, and no
Main merge. A separate interrupt fixture verifies all three child PIDs disappear.
The complete subprocess suite passed 2,385 checks. No account/model calls or
live desktop restart were performed.

Remaining: this CLI is not yet a drop-in replacement for the desktop's complete
backend invocation interface. Override packaging, forwarding non-app-server
CLI commands, actual installation, resource accounting across all processes,
Recall/Replay context delivery and actual input latency remain unfinished.
The fixed buffers and child process memory are not covered by VRS's PMR alone.

## Desktop backend override wrapper

`swegca-codex-wrapper` now accepts the installed desktop's original argument
list. `SWEGCA_DESKTOP_CONFIG` points to a bounded JSON configuration (maximum
64KiB). Fields: `backend`, `host`, `proxy`, `vrs`, `root`, `resourceConfig`,
`proxyConfig` are absolute paths; `mode` is create/open/limited-create/limited-open.
`backend` is the real executable, `host` is swegca-desktop-host. Self-referential
backend/host executables are rejected using inode identity. The wrapper uses
execv with the original argument vector, no shell or argument reconstruction.

Local app inspection found both invocation orders in its EQ function:
`-c features.code_mode_host=true app-server --analytics-default-enabled`, and
`app-server -c ... --analytics-default-enabled`. Both are recognized. The
installed backend's app-server help confirms stdio is the default and --stdio
is an alias. The wrapper routes only stdio server startup to the VRS host.
Help/version, schema generators and daemon version inspection pass to the real
backend. Other server transports/subcommands or ambiguous leading flags fail
rather than silently bypass VRS. Other explicit CLI commands pass through;
this wrapper does not claim to capture separate CLI agent sessions.

Verification: 33 isolated CLI routing checks, including byte-preserved argv,
spaces/metacharacters, both desktop invocation orders, recursive configuration
and unsupported server transports. The 2,385-check subprocess suite now launches
the real wrapper with the installed desktop's argument shape, then the host,
proxy, real VRS and synthetic backend. No actual app override was installed.
Remaining deployment work includes concrete persistent configuration, root and
session lifecycle setup, installation and live validation. Backend environment
is inherited unchanged. Model/account operations were not exercised.

## Persistent first start and restart

Wrapper/VRS mode `ensure` (also `limited-ensure`) now supports one persistent
configuration across launches. The root directory must be provisioned first.
Runtime acquires the existing StorageRoot exclusive lock before deciding:
create Main only if the root is empty; otherwise require an existing graph
directory and perform normal validated open. A nonempty unknown root, partial
Main, lock conflict, configuration mismatch or corruption is an error, never
a reason to recreate/overwrite storage. Directory discovery is startup only.

Proxy connectionSession and configured sessions also accept mode `ensure`,
using the host's existing attach/ensure lifecycle rules. Ended sessions remain
ineligible for new native events. Successful reconnection recovers recorded
sequence without ending or merging experiences. Because this launcher creates
a new backend, old pending RPC bindings are not installed in the fresh peer's
table (see Explicit peer lifetime below).

Verification: Runtime lifecycle 186 checks (including exclusive locking,
create/reopen, partial initialization and foreign-root preservation), wrapper
33 checks, real subprocess 2,389 checks. The desktop fixture is launched twice
with the exact same wrapper/proxy settings: its connection advances from five
to seven originals while the thread's three originals remain; no Main merge.
Live installation, root provisioning and explicit end handling still remain.

## Existing conversation resume

The installed backend's generated ThreadResumeParams schema requires threadId;
ThreadResumeResponse returns thread. The transport now treats a client
thread/resume request with an ID and nonempty threadId as lifecycle input.
It may ensure/restore that session before forwarding the request, even when
no thread/started notification arrives. Deferred pending-request reconstruction
runs only when the proxy owner explicitly declares a continued RPC peer. This
desktop launcher declares a new peer while retaining the VRS session. User turn/start/steer still cannot
trigger discovery. Server-origin resume and malformed resume requests fail;
they cannot fall back to the connection-only content route.

Verification: event adapter 215, Wire 61, actual subprocess 2,393 checks passed.
The desktop fixture restarts with identical configuration, resumes its prior
thread without a thread/started notification, then sends a user turn. Reopen
finds seven thread originals (the prior three plus resume/request response and
turn/request response), while connection originals remain seven. No live app
or account calls. A backend refusal to resume is recorded as its response; it
does not manufacture successful model state or authorize a Main merge.

## Thread content before start/resume notifications

Installed schemas ThreadReadParams, ThreadStatusChangedNotification and
ThreadNameUpdatedNotification all carry required threadId. Such messages may
need recording before a thread has been attached to this proxy. Wire now permits
attachment for explicitly identified content/lifecycle envelopes when the
existing core route_agent_event permits record. The host's attach/ensure still
checks actual session state; ended sessions are not revived. Input and unbound
responses cannot trigger this discovery. Existing request direction, ID,
capacity and post-recovery checks remain in effect.

Verification: Wire 74, duplex pump 88, subprocess 2,459 checks passed. A prior
conversation is read before its resume request after a desktop fixture restart,
without a thread/started notification. The read request/response are preserved;
reopen yields nine thread originals. Unit checks also cover a status notification
as the first message and reject an unknown user input even with free session
capacity. This is native envelope capture, not semantic verification of the
returned history or live desktop installation.


## Explicit peer lifetime

After spawning a backend, the host passes `--new-peer` to the proxy. Backend
RPC IDs may restart even though VRS conversation state continues. The proxy
therefore resumes experience sequence and memory but uses a fresh live RPC ID
table. This changes neither the backend's argv nor the stored old experiences.
No additional user setting is required for this launcher.

Actual subprocess tests leave one old request unanswered and reuse its ID after
relaunch, for both the connection session and a dynamically rediscovered thread.
The new reply references the new request original. The old request stays stored,
and no Main merge occurs. This is not proof of remote exactly-once execution or
automatic reconnect to a surviving backend.

## Aggregate limited desktop profile

`limited-create/open/ensure` now launches the entire desktop host in one user
systemd service using the resource configuration's `memoryBytes` and
`cpuAffinity`. The restarted host receives `bounded-*`, verifies actual
`memory.max`, `memory.swap.max=0` and affinity before spawning children, and
passes that verified mode to VRS. Host, proxy, VRS and the backend it owns share
one aggregate memory ceiling. The backend is included; the already-running GUI
and unrelated processes are not. A heavier backend may need a larger profile.
The common launcher preserves cwd and exact argv without invoking a shell.

Actual subprocess verification used a 64MiB profile, compiled host/proxy/VRS and
an isolated Python backend, with no model/account calls. After an RPC round trip,
all four live PIDs had the same cgroup and CPUs 6,7; memory.max was 67108864 and
memory.swap.max was 0. EOF reaped all four, and reopening showed no Main merge.
The subprocess suite passed 3,810 checks; wrapper routing passed 33; the standalone
profile probe independently verified the same profile after the launcher refactor.
This proves aggregate enforcement configuration, not large-graph operation
within 4GB, physical SSD throttling, or live desktop integration.

A current read-only check found no CODEX_CLI_PATH or SWEGCA_DESKTOP_CONFIG
overrides on running Codex backends. No running app or launcher entry was changed.

## Installed backend handshake and byte-granular limits

The opt-in `tests/installed_backend_smoke.py BUILD_DIR BACKEND` runs an actual
installed backend through host/proxy/VRS, in an explicitly named test cgroup.
It sends only initialize and initialized, never a turn or model-generation
request. Private native frames remain in temporary storage and are compared
byte-for-byte through VRS's authenticated original API after reopening. Cleanup
stops only that named test unit. It does not replace the current GUI backend.

The initial 4,000,000,000-byte run exposed kernel page rounding: on this 4096-byte
page host, memory.max reads 3,999,997,952. Resource launch/verification now both
use the requested budget rounded down to a full page; they never widen the
requested cap and reject a sub-page budget. The 64MiB aligned case still passes.

Actual installed codex-cli 0.155.0-alpha.9.2 passed: two client frames and two
server frames became four exact originals, survived reopen, and EOF caused zero
Main merges. The aggregate group had memory.max=3999997952 and swap.max=0.
No model generation was requested. This is real-backend startup/transport
evidence, not live GUI installation, user-turn quality, all-event capture, or
proof of large-scale operation under the budget.
