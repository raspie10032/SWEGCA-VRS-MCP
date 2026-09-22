# Windows / Claude Desktop

v2.2 includes the local backend. A Linux socket, SSH, WSL or remote main is not
needed. Install the Windows **desktop application** of Claude; local stdio MCP
configuration belongs to the desktop client, not a remote website connector URL.

The prior Windows installer used a product wheel and has been removed. A
Windows Python runtime with source-built NumPy and immutables has not yet been
verified under the no-prebuilt-wheel requirement. The final implementation
language is C++. The configuration below describes the source runtime boundary
for a verified local Python environment; it is not evidence of a completed
Windows installation.

In Claude Desktop open Settings → Developer → Edit Config. Windows configuration
is `%APPDATA%\Claude\claude_desktop_config.json`. Merge the following entry into
its existing `mcpServers` object, preserving other entries. Replace the example
paths with the actual source checkout, Python executable, and state directory.

```json
{
  "mcpServers": {
    "vrs2-memory": {
      "command": "C:\\Users\\YOUR_NAME\\AppData\\Local\\SWEGCA\\VRS2-venv\\Scripts\\python.exe",
      "args": [
        "-m", "swegca_vrs2.server",
        "--state-dir", "C:\\Users\\YOUR_NAME\\AppData\\Local\\SWEGCA\\VRS2",
        "--allow-ingest"
      ],
      "env": { "PYTHONPATH": "C:\\Users\\YOUR_NAME\\SWEGCA-VRS-MCP\\src" }
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

If startup fails, check `%APPDATA%\Claude\logs`. Confirm the exact Python
executable and source path exist, and run the module once with `--help`. A manually started stdio server waiting
silently is normal; it is waiting for a client. A duplicate-owner error means
another process is using that state directory; close that process normally.
Never delete the native memory directory to fix an installation path.

These setup steps follow the [official MCP SDK real-host guide](https://py.sdk.modelcontextprotocol.io/get-started/real-host/)
and [Claude local MCP guide](https://support.claude.com/en/articles/10949351-getting-started-with-local-mcp-servers-on-claude-desktop).
The release's actual Windows runner tests and Claude UI tests are reported
separately; passing stdio tests does not claim a Claude account/UI was exercised.
