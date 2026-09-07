# SWEGCA+VRS Hermes memory provider

Standalone directory plugin. Install `swegca-vrs-mcp` in the host's Python
environment with `--no-deps`; the adapter requires only the standard library.
Set `SWEGCA_MEMORY_SOCKET` to a running private memory service, select
`memory.provider: swegca-vrs`, and disable `memory_enabled` and
`user_profile_enabled` to replace both built-in memory channels.

Full configuration, privacy boundaries and limitations:
https://github.com/raspie10032/SWEGCA-VRS-MCP/blob/main/docs/HERMES_MEMORY.md

CLI profiles only; no gateway multi-user isolation or live-LLM acceptance claim.
