"""Reproducible real-Hermes lifecycle smoke with isolated subprocesses/storage."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import uuid

from swegca_vrs_mcp.agent_client import MemoryClient, MemoryUnavailable
from prepare_hermes_profile import prepare


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--hermes-source", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    started = time.monotonic()
    processes = []
    with tempfile.TemporaryDirectory(prefix="swegca-hermes-") as directory:
        root = Path(directory)
        profile = root / "profile"
        socket_path = root / "run/m.sock"
        prepare(profile, socket_path)
        env = {**os.environ, "HERMES_HOME": str(profile), "SWEGCA_MEMORY_SOCKET": str(socket_path)}
        client = MemoryClient(socket_path)
        def start_service():
            process = subprocess.Popen([sys.executable, "-m", "swegca_vrs_mcp.agent_service",
                "--state-dir", str(root / "store"), "--socket", str(socket_path), "--vrs-interval", "0.1"],
                stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True, encoding="utf-8")
            processes.append(process)
            deadline = time.monotonic() + 15
            while time.monotonic() < deadline:
                try:
                    client.call("status")
                    return process
                except MemoryUnavailable:
                    if process.poll() is not None:
                        raise RuntimeError(process.stderr.read())
                    time.sleep(0.05)
            raise TimeoutError("service startup")
        def run_host(session, remember=None):
            cmd = [sys.executable, str(Path(__file__).with_name("smoke_hermes_memory.py")),
                   "--hermes-source", str(args.hermes_source), "--session", session,
                   "--query", "ambercompiler"]
            if remember:
                cmd += ["--remember", remember]
            result = subprocess.run(cmd, env=env, capture_output=True, text=True, encoding="utf-8", timeout=30)
            (args.output / (session + ".stdout.json")).write_text(result.stdout, encoding="utf-8")
            (args.output / (session + ".stderr.txt")).write_text(result.stderr, encoding="utf-8")
            if result.returncode:
                raise RuntimeError("Hermes lifecycle failed; retained subprocess logs")
            return json.loads(result.stdout)
        try:
            first_service = start_service()
            marker = "synthetic-marker-" + uuid.uuid4().hex
            first = run_host("first", marker)
            assert marker in first["after_context"]
            assert first["status"]["episode_count"] == 1
            second = run_host("second")
            assert marker in second["before_context"]
            client.call("flush")
            before_restart = client.call("status")
            first_service.terminate()
            first_service.wait(timeout=15)
            assert first_service.returncode == 0
            start_service()
            third = run_host("after-restart")
            assert marker in third["before_context"]
            assert third["status"]["episode_count"] == 1
            assert third["status"]["owner_id"] == before_restart["owner_id"]
            assert third["status"]["memory_snapshot_id"] == before_restart["memory_snapshot_id"]
            assert third["status"]["world_hash"] == first["status"]["world_hash"]
            report = {"passed": True, "live_llm_called": False, "host_processes": 3,
                      "service_processes": 2, "episode_count": 1, "model_controlled_tool_calls": 0,
                      "native_memory_callbacks": True, "cross_session_recall": True,
                      "service_restart_recall": True, "automatic_vrs": third["status"]["graph_converged"],
                      "elapsed_seconds": time.monotonic() - started}
            (args.output / "summary.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
            print(json.dumps(report, indent=2))
        finally:
            for process in processes:
                if process.poll() is None:
                    process.terminate()
                    process.wait(timeout=15)
                process.stderr.close()


if __name__ == "__main__":
    main()
