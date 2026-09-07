"""Real stdio smoke check against the bundled synthetic diagnostic dataset."""
import argparse
import asyncio
import json

from mcp import Client, StdioServerParameters


async def run(args):
    params = StdioServerParameters(command=args.server, args=["--dataset", args.dataset])
    async with Client(params, mode=args.mode, read_timeout_seconds=15) as client:
        listed = await client.list_tools()
        status = await client.call_tool("system_status", {})
        if status.is_error or status.structured_content.get("synthetic") is not True:
            raise RuntimeError("Smoke check requires the bundled synthetic dataset")
        result = await client.call_tool("evaluate_vrs", {
            "query": "demo", "current_cues": ["demo"], "assessments": [{
                "episode_id": "vrs-edge-group:7", "proposition": "demo-outcome",
                "verdict": "support", "rationale": "Synthetic protocol check",
                "current_evidence_refs": ["fixture:current"]}]})
        if result.is_error:
            raise RuntimeError("MCP evaluation failed")
        data = result.structured_content
        decisions = [arm["decision"] for arm in data["arms"]]
        if decisions != ["success", "abstain", "abstain", "abstain", "success"]:
            raise RuntimeError("Unexpected synthetic decisions")
        if any(data["authority"].values()):
            raise RuntimeError("Authority boundary changed")
        summary = {"status": "PASS", "transport": "stdio", "mode": args.mode,
                   "tools": [tool.name for tool in listed.tools],
                   "dataset_id": data["dataset_id"], "decisions": decisions,
                   "authority": data["authority"], "growth_claimed": False}
    # Print only after the client context has closed its subprocess transport.
    print(json.dumps(summary, indent=2))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--server", required=True, help="Absolute installed console executable")
    parser.add_argument("--dataset", required=True, help="Absolute synthetic.json path")
    parser.add_argument("--mode", choices=["auto", "legacy"], default="auto")
    asyncio.run(run(parser.parse_args()))
