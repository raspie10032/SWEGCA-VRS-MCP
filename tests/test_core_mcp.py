import json
import os
from pathlib import Path
import sys

from mcp import Client, StdioServerParameters
import pytest

from tests.test_stateful import row, NOW, signed_rows

ROOT = Path(__file__).resolve().parents[1]


def params(store, writable=True, keys=None):
    args = ["-m", "swegca_vrs_mcp.server", "--state-dir", str(store)]
    if writable:
        args.append("--enable-writes")
    if keys:
        args.extend(["--producer-keys", str(keys)])
    return StdioServerParameters(command=sys.executable, args=args, cwd=str(ROOT),
                                env={"PYTHONPATH": str(ROOT / "src")})


@pytest.mark.parametrize("mode", ["auto", "legacy"])
async def test_real_mcp_persistent_experience_vrs_restart(tmp_path, mode):
    store = tmp_path / "memory"
    event = row().model_dump()
    async with Client(params(store), mode=mode, read_timeout_seconds=30) as client:
        names = {t.name for t in (await client.list_tools()).tools}
        assert names == {"system_status", "get_episode", "recall", "judge", "get_world",
                         "record_experiences", "converge_vrs", "commit_judgment", "rollback_world"}
        written = await client.call_tool("record_experiences", {
            "observations": [event], "request_id": "write-1", "expected_revision": 0})
        assert not written.is_error, written
        assert written.structured_content["added"] == 1
        vrs = await client.call_tool("converge_vrs", {"request_id": "vrs-1", "expected_revision": 1})
        assert not vrs.is_error, vrs
        assert vrs.structured_content["graph"]["converged"]
        recalled = await client.call_tool("recall", {"query": "build"})
        assert recalled.structured_content["episodes"][0]["episode_id"] == "e0"
        snapshot = recalled.structured_content["memory_snapshot_id"]
        judgment = await client.call_tool("judge", {"hypothesis_id": "compiler", "current_event_id": "e0"})
        assert not judgment.is_error, judgment
        assert judgment.structured_content["activation"]["stage_order"] == ["deja_vu", "recall", "replay", "re_evidence"]
        assert not judgment.structured_content["eligible_for_bounded_world_write"]
        forged = await client.call_tool("record_experiences", {
            "observations": [{**event, "authority": True}], "request_id": "bad", "expected_revision": 2})
        assert forged.is_error
    # A new actual server process, with write tools absent.
    async with Client(params(store, False), mode=mode, read_timeout_seconds=30) as client:
        names = {t.name for t in (await client.list_tools()).tools}
        assert "record_experiences" not in names
        assert len(names) == 5
        recalled = await client.call_tool("recall", {"query": "build"})
        assert recalled.structured_content["candidate_count"] == 1
        assert recalled.structured_content["memory_snapshot_id"] == snapshot
        assert recalled.structured_content["episodes"][0]["steps"][0]["observation"]["text"] == "compiler build result"


async def test_real_mcp_authenticated_world_commit_and_rollback(tmp_path):
    import time
    import hashlib
    from swegca_vrs_mcp.observations import sign_observation
    auth, rows = signed_rows()
    # Subprocess uses the real clock, unlike the deterministic unit fixture.
    adjusted = []
    for i, event in enumerate(rows):
        event = event.model_copy(update={"observed_at_ns": time.time_ns() - 1_000_000})
        key = hashlib.sha256(f"synthetic-producer-{i}".encode()).digest()
        adjusted.append(event.model_copy(update={"signature": sign_observation(event, key)}).model_dump())
    keys = tmp_path / "synthetic-keys.json"
    keys.write_text(auth._config.model_dump_json(), encoding="utf-8")
    keys.chmod(0o600)
    async with Client(params(tmp_path / "memory", keys=keys), mode="legacy", read_timeout_seconds=30) as client:
        async def call(name, args):
            result = await client.call_tool(name, args)
            assert not result.is_error, result
            return result.structured_content
        initial = await call("get_world", {})
        assert (await call("record_experiences", {"observations": adjusted,
            "request_id": "write", "expected_revision": 0}))["authenticated"] == 32
        assert (await call("converge_vrs", {"request_id": "vrs", "expected_revision": 1}))["graph"]["converged"]
        args = {"hypothesis_id": "compiler", "current_event_id": rows[-1].event_id}
        assert (await call("judge", args))["eligible_for_bounded_world_write"]
        assert (await call("commit_judgment", {**args, "request_id": "commit", "expected_revision": 2}))["world_committed"]
        rolled = await call("rollback_world", {"request_id": "rollback", "expected_revision": 3})
        assert rolled["restored_hash"] == initial["state_hash"]
        assert (await call("system_status", {}))["episode_count"] == 32
