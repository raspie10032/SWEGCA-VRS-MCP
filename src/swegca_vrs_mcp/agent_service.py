"""Local-only, single-owner memory service for agent lifecycle adapters.

This transport boundary performs JSON/socket I/O. The core hot activation does
not. All sessions in ONE service share ONE operator-selected trust domain.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import logging
import os
from pathlib import Path
import signal
import socket
import socketserver
import stat
import threading
import time
import uuid

from pydantic import Field
from filelock import FileLock, Timeout

from .observations import Observation, canonical
from .runtime import StrictModel, Text
from .stateful import StatefulCore, CoreError

MAX_WIRE = 2_097_152
LOG = logging.getLogger(__name__)


class Turn(StrictModel):
    session_id: Text
    turn_id: Text
    user: str = Field(max_length=262144)
    assistant: str = Field(max_length=262144)
    observed_at_ns: int = Field(ge=0)


class Recall(StrictModel):
    query: Text
    offset: int = Field(default=0, ge=0, strict=True)
    limit: int = Field(default=20, ge=1, le=200, strict=True)


class Episode(StrictModel):
    event_id: Text


class AgentMemoryService:
    def __init__(self, directory: Path, *, vrs_interval=30.0):
        if not 0.1 <= vrs_interval <= 86400:
            raise ValueError("vrs_interval must be between 0.1 and 86400 seconds")
        directory.mkdir(parents=True, exist_ok=True, mode=0o700)
        if directory.stat().st_uid != os.getuid() or directory.stat().st_mode & 0o077:
            raise ValueError("state directory must be owner-only (0700)")
        self.core = StatefulCore(directory, writable=True)
        self.lock = threading.RLock()
        self.stopping = threading.Event()
        self.interval = vrs_interval
        self.last_vrs_error = None
        self.worker = threading.Thread(target=self._maintenance, name="swegca-vrs", daemon=True)
        self.worker.start()

    def capture(self, args):
        turn = Turn.model_validate(args)
        # IDs include session + host turn identity, not text alone: identical
        # genuine turns remain separate; retries of one turn do not grow memory.
        key = hashlib.sha256(canonical([turn.session_id, turn.turn_id])).hexdigest()
        row = Observation(event_id="turn:" + key, hypothesis_id="conversation:" + key,
            producer_id="agent:hermes", context_id=turn.session_id,
            axis="observational", outcome="pending",
            observation={"kind": "conversation_turn", "user": turn.user,
                         "assistant": turn.assistant, "turn_id": turn.turn_id,
                         "actual_task_outcome": "not_observed",
                         "tool_payloads_captured": False},
            evidence_refs=["hermes:session:" + turn.session_id + ":turn:" + turn.turn_id],
            observed_at_ns=turn.observed_at_ns)
        with self.lock:
            return self.core.ingest([row], request_id="capture:" + key,
                                    expected_revision=self.core.revision)

    def flush(self):
        with self.lock:
            state = self.core.status()
            if not state["dirty_hypotheses"]:
                return {"status": "no_dirty_experience", "revision": state["revision"]}
            result = self.core.converge(request_id="vrs:" + str(uuid.uuid4()),
                expected_revision=state["revision"], max_rounds=48)
            self.last_vrs_error = None
            return result

    def _maintenance(self):
        while not self.stopping.wait(self.interval):
            try:
                self.flush()
            except Exception as exc:
                # No stored text, source paths or credentials in service logs.
                self.last_vrs_error = type(exc).__name__
                LOG.error("VRS maintenance failed (%s); raw experience retained", type(exc).__name__)

    def dispatch(self, method, args):
        if not isinstance(args, dict):
            raise ValueError("arguments must be an object")
        if method == "capture_turn":
            return self.capture(args)
        if method == "recall":
            return self.core.recall_context(**Recall.model_validate(args).model_dump())
        if method == "get_episode":
            return self.core.get_episode(Episode.model_validate(args).event_id)
        if method == "status" and not args:
            return {**self.core.status(), "vrs_interval_seconds": self.interval,
                    "last_vrs_error": self.last_vrs_error, "agent_adapter": "hermes",
                    "capture": "unsigned_user_assistant_turns_only"}
        if method == "flush" and not args:
            return self.flush()
        raise ValueError("unsupported method or arguments")

    def close(self):
        self.stopping.set()
        self.worker.join()
        self.core.close()


class Handler(socketserver.StreamRequestHandler):
    def handle(self):
        self.connection.settimeout(10)
        try:
            line = self.rfile.readline(MAX_WIRE + 1)
            if len(line) > MAX_WIRE or not line.endswith(b"\n"):
                raise ValueError("invalid frame")
            request = json.loads(line)
            if set(request) != {"method", "args"}:
                raise ValueError("invalid request")
            result = self.server.memory.dispatch(request["method"], request["args"])
            response = {"ok": True, "result": result}
        except (ValueError, TypeError, KeyError, CoreError) as exc:
            response = {"ok": False, "error": str(exc) if isinstance(exc, CoreError) else "invalid_request"}
        except Exception as exc:
            LOG.error("Memory request failed (%s)", type(exc).__name__)
            response = {"ok": False, "error": "service_error"}
        wire = canonical(response) + b"\n"
        if len(wire) > MAX_WIRE:
            wire = b'{"ok":false,"error":"response_too_large_reduce_page_size"}\n'
        try:
            self.wfile.write(wire)
        except OSError:
            pass  # A lost capture acknowledgement is safely retried by turn ID.


class MemorySocketServer(socketserver.ThreadingMixIn, socketserver.UnixStreamServer):
    daemon_threads = False
    block_on_close = True

    def __init__(self, socket_path: Path, memory: AgentMemoryService):
        parent = socket_path.parent
        parent.mkdir(parents=True, exist_ok=True, mode=0o700)
        if parent.stat().st_uid != os.getuid() or parent.stat().st_mode & 0o077:
            raise ValueError("socket directory must be owner-only (0700)")
        self.memory = memory
        self.socket_path = socket_path
        self._bound_inode = None
        self._socket_lease = FileLock(str(socket_path) + ".lock")
        try:
            self._socket_lease.acquire(timeout=0)
        except Timeout:
            raise ValueError("socket already exists and is owned") from None
        try:
            if socket_path.exists() or socket_path.is_symlink():
                info = socket_path.lstat()
                if not stat.S_ISSOCK(info.st_mode) or info.st_uid != os.getuid():
                    raise ValueError("refusing to remove a non-socket or another owner's path")
                with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as probe:
                    probe.settimeout(0.2)
                    try:
                        probe.connect(str(socket_path))
                    except ConnectionRefusedError:
                        socket_path.unlink()  # Exclusive lease + dead same-owner socket only.
                    else:
                        raise ValueError("socket already exists and is live")
            super().__init__(str(socket_path), Handler)
            os.chmod(socket_path, 0o600)
        except BaseException:
            self._socket_lease.release()
            raise

    def server_bind(self):
        super().server_bind()
        self._bound_inode = self.socket_path.lstat().st_ino

    def server_close(self):
        super().server_close()
        if self._bound_inode is not None and self.socket_path.exists():
            if self.socket_path.lstat().st_ino == self._bound_inode:
                self.socket_path.unlink()
        self._socket_lease.release()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--state-dir", type=Path, required=True)
    parser.add_argument("--socket", type=Path, required=True)
    parser.add_argument("--vrs-interval", type=float, default=30)
    args = parser.parse_args()
    os.umask(0o077)
    logging.basicConfig(level=logging.INFO)
    memory = AgentMemoryService(args.state_dir, vrs_interval=args.vrs_interval)
    server = None
    try:
        server = MemorySocketServer(args.socket, memory)
        def stop(*_):
            threading.Thread(target=server.shutdown, daemon=True).start()
        signal.signal(signal.SIGTERM, stop)
        signal.signal(signal.SIGINT, stop)
        server.serve_forever(poll_interval=0.2)
    finally:
        if server:
            server.server_close()
        memory.close()


if __name__ == "__main__":
    main()
