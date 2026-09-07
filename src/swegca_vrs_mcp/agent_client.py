"""Dependency-free private Unix-socket client; safe to import in a host agent."""
import json
from pathlib import Path
import socket

MAX_WIRE = 2_097_152


class MemoryUnavailable(RuntimeError):
    pass


class MemoryClient:
    def __init__(self, path, timeout=5.0):
        self.path = str(Path(path))
        self.timeout = timeout

    def call(self, method, **args):
        wire = json.dumps({"method": method, "args": args}, ensure_ascii=False,
                          allow_nan=False, separators=(",", ":")).encode("utf-8") + b"\n"
        if len(wire) > MAX_WIRE:
            raise MemoryUnavailable("request_too_large")
        try:
            with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as connection:
                connection.settimeout(self.timeout)
                connection.connect(self.path)
                connection.sendall(wire)
                with connection.makefile("rb") as stream:
                    line = stream.readline(MAX_WIRE + 1)
            if len(line) > MAX_WIRE or not line.endswith(b"\n"):
                raise MemoryUnavailable("invalid_service_response")
            response = json.loads(line)
            if not response["ok"]:
                raise MemoryUnavailable(response["error"])
            return response["result"]
        except (OSError, ValueError, KeyError) as exc:
            raise MemoryUnavailable("memory_transport_unavailable") from exc
