# SWEGCA VRS2 Memory MCP

**v2.2.0 runs on one Windows or Linux machine.** The distribution includes its
local main owner, persistent observation store, hot memory activation, native
VRS2 event arithmetic and overlapping connectivity regions. No Linux server,
Unix socket, WSL, GPU, model download or API key is required.

The replaced external-agent and v0.3 code is absent. This repository provides
the VRS2 main, memory MCP, live session layer, and their native runtime.

## Windows installation

Use Python 3.11 or newer. In **PowerShell 7**:

```powershell
py -3.12 -m venv "$env:LOCALAPPDATA\SWEGCA\VRS2-venv"
& "$env:LOCALAPPDATA\SWEGCA\VRS2-venv\Scripts\python.exe" -m pip install "git+https://github.com/raspie10032/SWEGCA-VRS-MCP.git@main"
& "$env:LOCALAPPDATA\SWEGCA\VRS2-venv\Scripts\swegca-vrs2-mcp.exe" --help
```

If your installed Python version differs, replace `-3.12` with that version.
The included [Windows setup script](tools/install_windows.ps1) also creates an
environment and generates a Claude configuration snippet with your actual paths.
It leaves existing Claude configuration intact.

Follow [Windows / Claude Desktop setup](docs/WINDOWS.md). The executable owns
the local memory directory. Use one running server per directory.

## Linux installation

```bash
python3 -m venv .venv
.venv/bin/python -m pip install git+https://github.com/raspie10032/SWEGCA-VRS-MCP.git@main
.venv/bin/swegca-vrs2-mcp --state-dir "$HOME/.local/share/swegca-vrs2" --allow-ingest
```

The process waits for MCP messages on stdin. It does not print an interactive
prompt. Normal stdout is reserved for MCP JSON messages; diagnostics use stderr.

The command above is the direct persistent-main MCP for clients that explicitly
call `memory_store`. Codex live-session use must run the session-first executable
and lifecycle hooks below.

## Codex live-session installation

Register the installed `swegca-vrs2-codex` executable as the MCP server named
`swegca-vrs`. The package normalizes that name to `swegca_vrs` in tool call IDs.
For example, add the actual executable and state paths to Codex configuration:

```toml
[mcp_servers.swegca-vrs]
command = "/absolute/path/to/venv/bin/swegca-vrs2-codex"
args = ["--state-dir", "/absolute/path/to/swegca-vrs2-codex"]
```

Generate the matching lifecycle hook file with the same installed Python:

```bash
/absolute/path/to/venv/bin/swegca-vrs2-codex-hooks \
  --state-dir /absolute/path/to/swegca-vrs2-codex \
  --server-name swegca_vrs \
  --output "$HOME/.codex/hooks.json.new"
```

Review and place the generated `hooks` object in Codex's hook configuration.
The generator refuses to overwrite an existing file. Every `SessionStart` and
`SessionEnd` is registered without guessing the host's reason string. Tool
input injection is limited to `mcp__swegca_vrs__memory_*`.

## Memory workflow

1. `memory_status`: obtain the current immutable pair snapshot.
2. `memory_context`: supply a query including useful task context, a unique
   request ID and that snapshot. Main executes Déjà vu → Recall → Replay →
   Re-evidence and returns original records with source/revision and current
   conflict controls. Candidate order is not acceptance or truth.
3. Follow `next_call` and deferred references before relying on missing content.
   `memory_read_path` and `memory_read` provide exact source access. Page sizes
   bound transport, never the total accessible memory.
   To open a known original address, use its exact `memory:...` episode ID as the
   `memory_context` query; lexical similarity is not required for address access.
4. `memory_release`: release the transient request; accumulated records remain.
5. When explicitly remembering an observation or result, use `memory_store`
   with `request_id`, original `text`, `source`, and `revision`. It is exposed
   only when started with `--allow-ingest`. Use stable IDs for retries.

Example `memory_store` arguments:

```json
{
  "request_id": "project-check-001",
  "text": "Windows에서 새 기억 저장 후 프로세스를 재시작했고 같은 원문을 다시 조회했다.",
  "source": "project://vrs2/windows-check/001",
  "revision": "1",
  "outcome": "success",
  "cues": ["VRS2", "Windows", "재시작"]
}
```

Outcomes are `success`, `failure`, `negative`, `uncertain`, `conflict`, `pending`.
They describe history; a failure label alone is not a logical contradiction.
Optional `proposition` and `polarity` (`support` / `refute`) preserve explicit
claim identity. Opposing claims remain visible together even across pages.
For an actual same-source correction, pass the old `episode_id` as `supersedes`
with a new revision. The old original remains addressable. Metadata, including
qualifications or emotion annotations, is preserved as data, not instructions.

For Codex, `swegca-vrs2-codex-hooks` generates lifecycle hooks that start one
session-local tailer. The tailer sends every complete host-visible transcript
record directly into a session-local VRS within its one-second poll. Event hooks
also perform cursor-safe scans as delivery boundaries. Reads
query that session VRS first and open durable main only after a complete miss.
`SessionEnd` starts a detached finalizer within the host's three-second hook
deadline; the finalizer captures the stable transcript tail and then adopts the
complete session VRS as main-owned linked shards. No transcript outbox, log
recall, observation export or re-ingest merge is used. Other clients can still
record explicitly through `memory_store`.

Recorded claims and matching source addresses do not certify independent
factual corroboration.

## Runtime and authority boundaries

The new package is `swegca_vrs2`; it does not import the historical
`swegca_vrs_mcp` package. [Native port manifest](NATIVE_VRS2_PORT.json) records exact
first-party source/definition hashes. The native numerical version remains
`vrs-re-evidence-event-signal-f32-v2-experimental`; it is not relabeled as the
old shuffle algorithm or the complete Rozephine application.

Main records external observations, keeps immutable hot generations, binds
re-evidence strength proposals once per ingress, settles affected numerical
dependencies and rebuilds connectivity only in affected connected components.
Unrelated components are shared. Identical observations do not count again as
new experience or repeated reinforcement. An explicit opposing source correction
weakens the old connection; unresolved competing claims prevent reinforcement.

Numeric connectivity is not logical entailment. Text retrieval uses literal
Unicode keys and Hangul substring cues, plus explicit proposition closure; it
does not promise language-model semantic understanding. VRS strength promotion
is derived from the current snapshot; it is not a grant of World/action/model
update authority. All such external authority remains disabled in this MCP.

No internal or final wording LLM is invoked. No agent identity or memory is stored
in model weights. The current client's model is external to the memory server.
These are software functionality checks, not a demonstrated cognitive growth run.

## Data and upgrades

Default Windows store: `%LOCALAPPDATA%\SWEGCA\VRS2`.
Default Linux store: `$XDG_DATA_HOME/swegca-vrs2` or `~/.local/share/swegca-vrs2`.
Use `--state-dir` to choose another directory. The runtime accepts only the
native checksummed VRS journal and checkpoint format.

Close the server before copying its entire state directory for backup. Never
delete the state directory to upgrade the application environment. Restart loads
and validates the durable journal before serving hot queries.

## Development verification

```bash
python -m pip install '.[test]' build
python -m pytest -q tests/standalone
python -m build
python tools/verify_standalone.py
python tools/smoke_mcp.py --server .venv/bin/swegca-vrs-mcp
python tools/smoke_codex_mcp.py --server .venv/bin/swegca-vrs2-codex
```

Repository GitHub Actions are disabled. Verification is run locally against the
built source and actual stdio clients, including new memory, original-source
retrieval, process restart, conflicts, transaction failure and authority.
Actual host UI testing is separate from MCP SDK compatibility verification.
