"""Installed-package MCP persistence smoke; uses a fresh temporary synthetic store."""
import argparse
import asyncio
import json
from pathlib import Path
import tempfile
import time

from mcp import Client, StdioServerParameters


async def run(server, mode):
    with tempfile.TemporaryDirectory(prefix="swegca-mcp-smoke-") as directory:
        args = ["--state-dir", str(Path(directory) / "memory")]
        async with Client(StdioServerParameters(command=server, args=[*args, "--enable-writes"]),
                          mode=mode, read_timeout_seconds=30) as client:
            names = [t.name for t in (await client.list_tools()).tools]
            result = await client.call_tool("record_experiences", {
                "request_id": "synthetic-write", "expected_revision": 0,
                "observations": [{"event_id": "synthetic-1", "hypothesis_id": "build",
                    "producer_id": "synthetic-agent", "context_id": "synthetic-session",
                    "axis": "observational", "outcome": "failure",
                    "observation": {"text": "synthetic compiler exit failure"},
                    "evidence_refs": ["fixture:synthetic-1"], "observed_at_ns": time.time_ns()}]})
            assert not result.is_error, result
            assert result.structured_content["added"] == 1
            result = await client.call_tool("converge_vrs", {"request_id": "synthetic-vrs", "expected_revision": 1})
            assert not result.is_error and result.structured_content["graph"]["converged"], result
            result = await client.call_tool("recall", {"query": "compiler"})
            assert not result.is_error and result.structured_content["candidate_count"] == 1, result
            snapshot = result.structured_content["memory_snapshot_id"]
        async with Client(StdioServerParameters(command=server, args=args),
                          mode=mode, read_timeout_seconds=30) as client:
            result = await client.call_tool("recall", {"query": "compiler"})
            assert not result.is_error and result.structured_content["memory_snapshot_id"] == snapshot, result
            assert result.structured_content["episodes"][0]["steps"][0]["outcome"] == "failure"
            assert len((await client.list_tools()).tools) == 5
    # TemporaryDirectory removes only this newly created synthetic fixture.
    print(json.dumps({"status": "PASS", "mode": mode, "tools": names,
                      "persistent_recall_after_restart": True, "memory_snapshot_id": snapshot,
                      "growth_claimed": False}, indent=2))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--server", required=True)
    parser.add_argument("--mode", choices=["auto", "legacy"], default="auto")
    options = parser.parse_args()
    asyncio.run(run(options.server, options.mode))
