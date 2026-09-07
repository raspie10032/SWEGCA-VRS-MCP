import json
import os
from pathlib import Path
import subprocess
import sys

from mcp import Client, StdioServerParameters
import pytest

from swegca_vrs_mcp.runtime import Runtime, load_dataset
from swegca_vrs_mcp.server import create_server

ROOT = Path(__file__).resolve().parents[1]


def runtime():
    return Runtime(load_dataset(ROOT / "examples/synthetic.json"))


def parameters():
    return StdioServerParameters(
        command=sys.executable, args=["-m", "swegca_vrs_mcp.server", "--dataset",
                                      str(ROOT / "examples/synthetic.json")],
        env={"PYTHONPATH": str(ROOT / "src")}, cwd=str(ROOT))


async def exercise(client):
    tools = await client.list_tools()
    assert {t.name for t in tools.tools} == {"system_status", "get_episode", "recall", "evaluate_vrs"}
    assert all(t.annotations.read_only_hint for t in tools.tools)
    status = await client.call_tool("system_status", {})
    assert not status.is_error
    assert status.structured_content["episode_count"] == 8
    assert not any(status.structured_content["authority"].values())
    recalled = await client.call_tool("recall", {"query": "demo", "current_cues": ["demo"], "limit": 2})
    assert not recalled.is_error
    assert recalled.structured_content["candidate_count"] == 8
    assert len(recalled.structured_content["candidates"]) == 2
    assert recalled.structured_content["next_offset"] == 2
    result = await client.call_tool("evaluate_vrs", {
        "query": "demo", "current_cues": ["demo"], "assessments": [{
            "episode_id": "vrs-edge-group:7", "proposition": "demo-outcome",
            "verdict": "support", "rationale": "Synthetic conditional evidence",
            "current_evidence_refs": ["fixture:current"]}]})
    assert not result.is_error
    assert [a["decision"] for a in result.structured_content["arms"]] == [
        "success", "abstain", "abstain", "abstain", "success"]
    assert result.structured_content["input_authenticity_verified"] is False
    empty = await client.call_tool("evaluate_vrs", {"query": "demo", "current_cues": ["demo"], "assessments": []})
    assert all(a["decision"] == "abstain" for a in empty.structured_content["arms"])
    absent = await client.call_tool("get_episode", {"episode_id": "absent"})
    assert absent.is_error
    resource = await client.read_resource("swegca://capabilities")
    assert json.loads(resource.contents[0].text)["read_only"] is True
    after = await client.call_tool("system_status", {})
    assert after.structured_content == status.structured_content


async def test_sdk_in_memory_all_tools_and_resource():
    async with Client(create_server(runtime())) as client:
        await exercise(client)


@pytest.mark.parametrize("mode", ["auto", "legacy"])
async def test_real_stdio_subprocess(mode):
    # Real subprocess transport, not monkeypatched replies or an in-memory server.
    async with Client(parameters(), mode=mode, read_timeout_seconds=15) as client:
        await exercise(client)


@pytest.mark.parametrize("tool,args", [
    ("recall", {"query": "demo", "current_cues": ["demo"], "limit": 0}),
    ("recall", {"query": "demo", "current_cues": ["demo"], "limit": 201}),
    ("get_episode", {"episode_id": " "}),
    ("recall", {"query": "demo", "current_cues": ["demo"] * 257}),
    ("evaluate_vrs", {"query": "demo", "current_cues": ["demo"], "assessments": [{
        "episode_id": "vrs-edge-group:7", "proposition": "p", "verdict": "support",
        "rationale": "No current provenance"}]}),
    ("evaluate_vrs", {"query": "demo", "current_cues": ["demo"], "assessments": [{
        "episode_id": "vrs-edge-group:7", "proposition": "p", "verdict": "support",
        "rationale": "Injected authority", "current_evidence_refs": ["fixture:x"],
        "authority": {"world": True}}]}),
])
async def test_invalid_requests_are_tool_errors(tool, args):
    async with Client(create_server(runtime())) as client:
        result = await client.call_tool(tool, args)
        assert result.is_error


def test_missing_dataset_fails_without_traceback_or_stdout():
    env = {**os.environ, "PYTHONPATH": str(ROOT / "src")}
    result = subprocess.run([sys.executable, "-m", "swegca_vrs_mcp.server", "--dataset", "absent.json"],
                            cwd=ROOT, env=env, capture_output=True, text=True, timeout=15)
    assert result.returncode == 2
    assert result.stdout == ""
    assert "Traceback" not in result.stderr
    assert "no server started" in result.stderr


async def test_oversize_reply_is_error_not_silent_evidence_truncation(monkeypatch):
    import swegca_vrs_mcp.server as server
    monkeypatch.setattr(server, "MAX_RESPONSE_BYTES", 10)
    async with Client(create_server(runtime())) as client:
        result = await client.call_tool("system_status", {})
        assert result.is_error
        assert "response_too_large" in result.content[0].text
