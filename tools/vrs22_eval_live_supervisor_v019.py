"""Start and verify one native VRS conversation watcher for every Codex session.

This is evaluation orchestration for isolated Codex homes whose normal user
configuration and hooks are deliberately disabled. It contains no dialogue
store: transcript files remain ingress streams and each discovered session is
tailed by the installed product's ``conversation_watch.watch`` function.
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import time

from swegca_vrs2.session_capture import SessionCapture, codex_session
from swegca_vrs2.native_lock import FileLock, Timeout


POLL_SECONDS = 0.05


def atomic_status(path: Path, value: dict) -> None:
    path.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
    temporary = path.with_name(f".{path.name}-{os.getpid()}")
    temporary.write_text(json.dumps(value, sort_keys=True, separators=(",", ":")),
                         encoding="utf-8")
    os.replace(temporary, path)


def lock_held(path: Path) -> bool:
    lock = FileLock(str(path), thread_local=False)
    try:
        lock.acquire(timeout=0)
    except Timeout:
        return True
    else:
        lock.release()
        return False


def watcher_command(state: Path, session: str, transcript: Path) -> list[str]:
    code = (
        "import sys; from swegca_vrs2.conversation_watch import watch; "
        "raise SystemExit(0 if watch(sys.argv[1], 'codex', sys.argv[2], "
        "sys.argv[3], poll_seconds=0.05) else 3)"
    )
    return [sys.executable, "-c", code, str(state), session, str(transcript)]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--state-dir", type=Path, required=True)
    parser.add_argument("--codex-home", type=Path, required=True)
    parser.add_argument("--status", type=Path, required=True)
    parser.add_argument("--stop", type=Path, required=True)
    args = parser.parse_args()
    state = args.state_dir.expanduser().resolve()
    home = args.codex_home.expanduser().resolve()
    capture = SessionCapture(state)
    children: dict[str, dict] = {}
    started_ns = time.time_ns()
    scan_count = 0
    stopping = False
    try:
        while not args.stop.exists():
            scan_started_ns = time.time_ns()
            for transcript in sorted((home / "sessions").rglob("*.jsonl")):
                session = codex_session(transcript)
                if not session or session in children:
                    continue
                process = subprocess.Popen(
                    watcher_command(state, session, transcript),
                    stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                    stderr=sys.stderr, close_fds=True,
                    env=dict(os.environ, PYTHONDONTWRITEBYTECODE="1"))
                children[session] = {
                    "process": process,
                    "transcript": transcript.resolve(),
                    "discovered_ns": time.time_ns(),
                }
            watchers = {}
            for session, child in children.items():
                process = child["process"]
                returncode = process.poll()
                ended = capture.end_path("codex", session).exists()
                if returncode is not None and not ended:
                    raise RuntimeError(
                        f"watcher_exited_before_session_end:{session}:{returncode}")
                key = capture.session_key(session)
                lock_path = capture.meta / "watchers" / "codex" / (key + ".lock")
                watchers[key] = {
                    "session_id": session,
                    "transcript": str(child["transcript"]),
                    "pid": process.pid,
                    "alive": returncode is None,
                    "lock_held": returncode is None and lock_held(lock_path),
                    "ended": ended,
                    "discovered_ns": child["discovered_ns"],
                }
            scan_count += 1
            atomic_status(args.status, {
                "schema": "vrs22-eval-live-supervisor-v1",
                "status": "running",
                "pid": os.getpid(),
                "started_ns": started_ns,
                "heartbeat_ns": time.time_ns(),
                "scan_started_ns": scan_started_ns,
                "scan_count": scan_count,
                "database_module_loaded": any(
                    name == "sqlite3" or name.startswith("sqlite3.")
                    for name in sys.modules),
                "watchers": watchers,
            })
            time.sleep(POLL_SECONDS)
        stopping = True
        for child in children.values():
            if child["process"].poll() is None:
                child["process"].terminate()
        for child in children.values():
            process = child["process"]
            if process.poll() is None:
                try:
                    process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=5)
        atomic_status(args.status, {
            "schema": "vrs22-eval-live-supervisor-v1",
            "status": "stopped",
            "pid": os.getpid(),
            "started_ns": started_ns,
            "heartbeat_ns": time.time_ns(),
            "scan_count": scan_count,
            "database_module_loaded": any(
                name == "sqlite3" or name.startswith("sqlite3.")
                for name in sys.modules),
            "watchers": {
                capture.session_key(session): {
                    "session_id": session,
                    "transcript": str(child["transcript"]),
                    "pid": child["process"].pid,
                    "alive": False,
                    "returncode": child["process"].returncode,
                }
                for session, child in children.items()
            },
        })
        return 0
    except BaseException as error:
        atomic_status(args.status, {
            "schema": "vrs22-eval-live-supervisor-v1",
            "status": "failed",
            "pid": os.getpid(),
            "started_ns": started_ns,
            "heartbeat_ns": time.time_ns(),
            "scan_count": scan_count,
            "error": type(error).__name__,
            "detail": str(error)[:1000],
            "stopping": stopping,
        })
        for child in children.values():
            if child["process"].poll() is None:
                child["process"].terminate()
        raise


if __name__ == "__main__":
    raise SystemExit(main())
