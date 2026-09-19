# Windows / Claude Desktop

v2.1 includes the local backend. A Linux socket, SSH, WSL or remote main is not
needed. Install the Windows **desktop application** of Claude; local stdio MCP
configuration belongs to the desktop client, not a remote website connector URL.

Install Python 3.11+ and use PowerShell 7. Run the commands in the repository
README, or run `pwsh -File tools/install_windows.ps1 -PythonVersion 3.12` after
downloading this repository. The script installs v2.1.0 and prints a configuration
snippet. It does not edit existing Claude settings.

In Claude Desktop open Settings → Developer → Edit Config. Windows configuration
is `%APPDATA%\Claude\claude_desktop_config.json`. Merge the following entry into
its existing `mcpServers` object, preserving other entries. Replace `YOUR_NAME`
with the actual profile path, or use the script-generated paths.

```json
{
  "mcpServers": {
    "vrs2-memory": {
      "command": "C:\\Users\\YOUR_NAME\\AppData\\Local\\SWEGCA\\VRS2-venv\\Scripts\\swegca-vrs2-mcp.exe",
      "args": [
        "--state-dir", "C:\\Users\\YOUR_NAME\\AppData\\Local\\SWEGCA\\VRS2",
        "--allow-ingest"
      ]
    }
  }
}
```

Fully close and reopen Claude Desktop after updating configuration. Do not also
run the server manually against the same state directory: one main owns it.
With `--allow-ingest` there are nine tools, including `memory_store` and
`memory_context`. Omitting that flag gives eight read/receipt tools.

To check the lifecycle in Claude:

1. Ask it to call `memory_store` for an original sentence, with a unique request
   ID, a source such as `user://windows-check`, and revision `1`.
2. Ask it to obtain `memory_status`, then retrieve that sentence using
   `memory_context` and the current snapshot. Check original text and source.
3. Close Claude Desktop, reopen it, and request the same memory again. Main's
   identity, source record and committed snapshot should persist.

If startup fails, check `%APPDATA%\Claude\logs`. Confirm the exact executable
exists, and run it once with `--help`. A manually started stdio server waiting
silently is normal; it is waiting for a client. A duplicate-owner error means
another process is using that state directory; close that process normally.
Never delete the memory directory or its database to fix an installation path.

These setup steps follow the [official MCP SDK real-host guide](https://py.sdk.modelcontextprotocol.io/get-started/real-host/)
and [Claude local MCP guide](https://support.claude.com/en/articles/10949351-getting-started-with-local-mcp-servers-on-claude-desktop).
The release's actual Windows runner tests and Claude UI tests are reported
separately; passing stdio tests does not claim a Claude account/UI was exercised.

## Unreleased 2.2 development branch: shared resident mode

This section applies only when a wheel built from the 2.2 development branch
is installed. The published v2.1.0 wheel does not have `--loopback`.
Add `--loopback` to the MCP `args` shown above. A prompt hook, stop hook and
Claude's stdio MCP may then use the same state directory: the first bridge
starts one main bound to `127.0.0.1`, and later bridges attach to it. The
endpoint uses an owner-local random secret; no public listener or model is
started. All bridges must use the same `--state-dir`. Start the first bridge
with `--allow-ingest` if explicit observation writes are required. A later
write-enabled bridge cannot silently upgrade a running read-only main.

For explicit offline `checkpoint`, `compact` or `consolidate` maintenance,
close clients and stop the resident cleanly first:

```powershell
& "$env:LOCALAPPDATA\SWEGCA\VRS2-venv\Scripts\python.exe" -m swegca_vrs2.loopback --shutdown --state-dir "$env:LOCALAPPDATA\SWEGCA\VRS2"
```

Then run `python -m swegca_vrs2.maintenance --state-dir <store> --help` to
inspect explicit maintenance commands. Consolidation needs exact original
`memory:...` parent IDs and the current pair ID. It creates a source-bound
navigation object, never a factual summary or independent experience. Original
records and conflicting revisions remain accessible. A later MCP bridge will
start the resident again; do not delete or reset the state directory.
