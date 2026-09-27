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
