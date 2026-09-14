# SWEGCA VRS2 Memory MCP

**v2.1.0 runs on one Windows or Linux machine.** The distribution includes its
local main owner, persistent observation store, hot memory activation, native
VRS2 event arithmetic and overlapping connectivity regions. No Linux server,
Unix socket, WSL, GPU, model download or API key is required.

Hermes is **not included** in the v2.1 wheel or source archive. Historical Hermes
and v0.3 code remains in repository history/source for existing users; it is not
imported, installed or automatically migrated by this version.

## Windows installation

Use Python 3.11 or newer. In **PowerShell 7**:

```powershell
py -3.12 -m venv "$env:LOCALAPPDATA\SWEGCA\VRS2-venv"
& "$env:LOCALAPPDATA\SWEGCA\VRS2-venv\Scripts\python.exe" -m pip install "https://github.com/raspie10032/SWEGCA-VRS-MCP/releases/download/v2.1.0/swegca_vrs_mcp-2.1.0-py3-none-any.whl"
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
.venv/bin/python -m pip install https://github.com/raspie10032/SWEGCA-VRS-MCP/releases/download/v2.1.0/swegca_vrs_mcp-2.1.0-py3-none-any.whl
.venv/bin/swegca-vrs2-mcp --state-dir "$HOME/.local/share/swegca-vrs2" --allow-ingest
```

The process waits for MCP messages on stdin. It does not print an interactive
prompt. Normal stdout is reserved for MCP JSON messages; diagnostics use stderr.

## Memory workflow

1. `memory_status`: obtain the current immutable pair snapshot.
2. `memory_context`: supply a query including useful task context, a unique
   request ID and that snapshot. Main executes Déjà vu → Recall → Replay →
   Re-evidence and returns original records with source/revision and current
   conflict controls. Candidate order is not acceptance or truth.
3. Follow `next_call` and deferred references before relying on missing content.
   `memory_read_path` and `memory_read` provide exact source access. Page sizes
   bound transport, never the total accessible memory.
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

There is no automatic conversation interception. Clients decide when to request
recording; main validates and commits observations. Recorded claims and matching
source addresses do not certify independent factual corroboration.

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
Use `--state-dir` to choose another directory. Do not point this version at a
legacy v0.3 store: there is no automatic legacy-store import. Existing data is
preserved; use the earlier release/environment to access its original format.

Close the server before copying its entire state directory for backup. Never
delete the state directory to upgrade the application environment. Restart loads
and validates the durable journal before serving hot queries.

## Development verification

```bash
python -m pip install '.[test]' build
python -m pytest -q tests/standalone
python -m build
python tools/verify_standalone.py
```

[Windows and Linux CI](../../actions/workflows/standalone.yml) installs the built
wheel and tests actual stdio clients, new memory, original-source retrieval,
process restart, conflicts, transaction failure, authority and archive contents.
Actual Claude UI testing is separate from MCP SDK compatibility verification.
Historical results: [v2.0 bridge validation](docs/VRS2_VALIDATION.md).
