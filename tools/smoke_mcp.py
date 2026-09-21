"""Exercise the built VRS2 MCP through real stdio, persistence, and four stages."""
from __future__ import annotations

import argparse
import asyncio
import json
from pathlib import Path
import tempfile

from mcp import Client, StdioServerParameters


async def one_process(parameters, mode):
    async with Client(parameters, mode=mode, read_timeout_seconds=30) as client:
        names = {tool.name for tool in (await client.list_tools()).tools}
        if names != {"memory_context", "memory_status", "memory_recall", "memory_continue",
                     "memory_resume", "memory_read", "memory_read_path", "memory_release",
                     "memory_store"}:
            raise RuntimeError(f"unexpected tool catalog: {sorted(names)}")
        for request_id, polarity, outcome in (
                ("smoke-support", "support", "success"),
                ("smoke-refute", "refute", "conflict")):
            reply = await client.call_tool("memory_store", {
                "request_id": request_id,
                "text": f"stdio native journal four stage smoke {polarity}",
                "source": f"diagnostic:{request_id}", "revision": "1",
                "outcome": outcome, "proposition": "smoke:claim",
                "polarity": polarity})
            if reply.is_error:
                raise RuntimeError(f"memory_store failed: {reply}")
        status = (await client.call_tool("memory_status", {})).structured_content
        context = await client.call_tool("memory_context", {
            "request_id": "smoke-context", "query": "stdio native journal four stage smoke",
            "expected_pair_snapshot_id": status["pair_snapshot_id"], "page_size": 8})
        if context.is_error:
            raise RuntimeError(f"memory_context failed: {context}")
        packet = context.structured_content
        if packet["candidate_count"] != 2:
            raise RuntimeError(f"expected two opposing experiences: {packet['candidate_count']}")
        handle = {key: packet[key] for key in ("request_id", "view_id")}
        stage_paths = {
            "deja_vu": ["receipt", "activation", "deja_vu"],
            "recall": ["receipt", "activation", "recall"],
            "replay": ["receipt", "activation", "replay"],
            "re_evidence": ["receipt", "activation", "re_evidence"],
        }
        for stage, path in stage_paths.items():
            stage_reply = await client.call_tool("memory_read_path", {
                **handle, "path": path})
            if stage_reply.is_error:
                raise RuntimeError(f"four-stage receipt missing {stage}: {stage_reply}")
        release = await client.call_tool("memory_release", handle)
        if release.is_error or release.structured_content.get("experience_deleted") is not False:
            raise RuntimeError("release changed durable experience")
        return status, packet


async def restart(parameters, mode, before):
    async with Client(parameters, mode=mode, read_timeout_seconds=30) as client:
        after = (await client.call_tool("memory_status", {})).structured_content
        if after["identity"] != before["identity"]:
            raise RuntimeError("main identity changed after restart")
        if after["pair_snapshot_id"] != before["pair_snapshot_id"]:
            raise RuntimeError("snapshot changed after clean restart")
        context = await client.call_tool("memory_context", {
            "request_id": "smoke-restart", "query": "stdio native journal four stage smoke",
            "expected_pair_snapshot_id": after["pair_snapshot_id"], "page_size": 8})
        if context.is_error or context.structured_content.get("candidate_count") != 2:
            raise RuntimeError("experiences did not survive restart")
        packet = context.structured_content
        await client.call_tool("memory_release", {
            key: packet[key] for key in ("request_id", "view_id")})
        return after


async def run(args):
    temporary = None
    if args.state_dir:
        state = args.state_dir.expanduser().resolve()
        state.mkdir(parents=True, exist_ok=True)
    else:
        temporary = tempfile.TemporaryDirectory(prefix="swegca-vrs2-smoke-")
        state = Path(temporary.name)
    parameters = StdioServerParameters(
        command=str(args.server),
        args=["--state-dir", str(state), "--allow-ingest"])
    try:
        before, packet = await one_process(parameters, args.mode)
        after = await restart(parameters, args.mode, before)
        files = sorted(str(path.relative_to(state)) for path in state.rglob("*") if path.is_file())
        if any("sqlite" in name.lower() for name in files):
            raise RuntimeError(f"SQLite artifact created: {files}")
        required = {"vrs-store.json", "checkpoint.vrsc"}
        if not required <= {Path(name).name for name in files} or not any(name.endswith(".vrsj") for name in files):
            raise RuntimeError(f"native store files missing: {files}")
        print(json.dumps({
            "status": "PASS", "transport": "stdio", "mode": args.mode,
            "state": str(state), "identity": after["identity"],
            "pair_snapshot_id": after["pair_snapshot_id"],
            "candidate_count": packet["candidate_count"], "internal_llm_calls": 0,
            "authority_granted": False, "growth_claimed": False,
            "storage_files": files,
        }, ensure_ascii=False, indent=2))
    finally:
        if temporary is not None:
            temporary.cleanup()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--server", type=Path, required=True,
                        help="Installed swegca-vrs-mcp console executable")
    parser.add_argument("--state-dir", type=Path,
                        help="Fresh diagnostic state; omitted uses and removes a temporary directory")
    parser.add_argument("--mode", choices=["auto", "legacy"], default="auto")
    asyncio.run(run(parser.parse_args()))
