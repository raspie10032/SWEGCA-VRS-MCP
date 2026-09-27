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
sequence and outstanding requests without ending or merging experiences.

Verification: Runtime lifecycle 186 checks (including exclusive locking,
create/reopen, partial initialization and foreign-root preservation), wrapper
33 checks, real subprocess 2,389 checks. The desktop fixture is launched twice
with the exact same wrapper/proxy settings: its connection advances from five
to seven originals while the thread's three originals remain; no Main merge.
Live installation, root provisioning and explicit end handling still remain.
