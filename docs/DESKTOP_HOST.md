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
