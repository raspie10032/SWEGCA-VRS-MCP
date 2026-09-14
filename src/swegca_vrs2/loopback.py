# -*- coding: utf-8 -*-
"""Resident standalone main on a loopback TCP port — one owner, many short-lived clients.

Local Windows adapter (2026-09-14). v2.1's ``Main`` holds the state directory's owner
lock for the life of its process. On Linux the Hermes service shares one main across
CLI sessions over a Unix socket; Windows Python has no ``AF_UNIX``, so this module does
the same over ``127.0.0.1``. The wire format is the resident protocol used by
``native_transport.ResidentClient``: one JSON line per request on a fresh connection,
one JSON line back. The six ``cognitive_dialogue_*``/``status`` commands are served by
``server.LocalResident`` unchanged, so ``native_memory.MemoryMCPServer`` (the stdio
bridge) works against this daemon as it would against a native main.

Extra commands exist for local hooks, which need a short packet rather than a paged
receipt: ``hook_recall`` (top candidates with source, revision, outcome, verdict and a
text snippet), ``ingest`` (``memory_store`` semantics; needs ``--allow-ingest``),
``checkpoint``, ``ping`` and ``shutdown``. Loopback only; this is a single-user machine
and the socket is not an access-control boundary (same caveat as upstream).

    python -m swegca_vrs2.loopback --state-dir DIR [--allow-ingest] [--port N] [--idle-hours H]

The port is written to ``DIR/loopback.port`` so clients can find it. The daemon exits
after ``idle-hours`` without requests (checkpointing first) or on ``shutdown``.
"""
from __future__ import annotations

import argparse
import os
import socket
import socketserver
import subprocess
import sys
import threading
import time
from pathlib import Path

from .native_transport import InterfaceError, encode, decode, MAX_BYTES

HOST = '127.0.0.1'
PORT_FILE = 'loopback.port'
RESIDENT_COMMANDS = {'status', 'cognitive_dialogue_start', 'cognitive_dialogue_continue',
                     'cognitive_dialogue_evidence_open', 'cognitive_dialogue_evidence',
                     'cognitive_dialogue_release'}
LOCAL_COMMANDS = {'hook_recall', 'ingest', 'checkpoint', 'ping', 'shutdown'}


# ── client ────────────────────────────────────────────────────────────

def port_of(state_dir):
    try:
        return int((Path(state_dir) / PORT_FILE).read_text(encoding='ascii').strip())
    except (OSError, ValueError):
        return None


class LoopbackClient:
    """Drop-in for ``native_transport.ResidentClient`` over loopback TCP."""

    def __init__(self, port, timeout_seconds=45):
        self.port, self.timeout_seconds = int(port), float(timeout_seconds)
        self._connection = self._stream = None      # reused across requests (line protocol)

    def _open(self):
        self._connection = socket.create_connection((HOST, self.port), timeout=self.timeout_seconds)
        self._stream = self._connection.makefile('rb')

    def close(self):
        for item in (self._stream, self._connection):
            try:
                if item is not None:
                    item.close()
            except OSError:
                pass
        self._connection = self._stream = None

    def request(self, command, **arguments):
        if command not in RESIDENT_COMMANDS | LOCAL_COMMANDS:
            raise InterfaceError('resident_operation_not_exported')
        payload = encode({'command': command, **arguments}) + b'\n'
        raw = b''
        for attempt in (0, 1):
            try:
                if self._connection is None:
                    self._open()
                self._connection.sendall(payload)
                raw = self._stream.readline(MAX_BYTES + 1)
                if not raw.endswith(b'\n'):
                    raise OSError('closed')
                break
            except OSError:
                self.close()
                if attempt:
                    raise InterfaceError('resident_request_failed') from None
        result = decode(raw)
        if result.get('status') == 'rejected':
            raise InterfaceError('resident_rejected: ' + str(result.get('reason', ''))[:120])
        return result


def ensure_daemon(state_dir, *, allow_ingest=True, python=None, wait_seconds=30):
    """Return a client for the daemon owning ``state_dir``, starting it if needed."""
    state_dir = Path(state_dir)
    port = port_of(state_dir)
    if port is not None:
        client = LoopbackClient(port, 5)
        try:
            client.request('ping')
            return client
        except InterfaceError:
            pass
    command = [python or sys.executable, '-m', 'swegca_vrs2.loopback', '--state-dir', str(state_dir)]
    if allow_ingest:
        command.append('--allow-ingest')
    flags = 0
    if os.name == 'nt':
        flags = subprocess.CREATE_NEW_PROCESS_GROUP | getattr(subprocess, 'DETACHED_PROCESS', 0)
    subprocess.Popen(command, creationflags=flags, stdin=subprocess.DEVNULL,
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, close_fds=True)
    deadline = time.time() + wait_seconds
    while time.time() < deadline:
        time.sleep(0.2)
        port = port_of(state_dir)
        if port is not None:
            client = LoopbackClient(port, 5)
            try:
                client.request('ping')
                return client
            except InterfaceError:
                continue
    raise InterfaceError('resident_daemon_did_not_start')


# ── daemon ────────────────────────────────────────────────────────────

def _asks_of(text):
    """The record's own "찾을 때 묻는 말" (verdict tail line or memory-doc section), else ''."""
    marker = '찾을 때 묻는 말'
    at = text.find(marker)
    if at < 0:
        return ''
    tail = text[at + len(marker):]
    tail = tail.split('\n## ', 1)[0]          # doc section ends at the next heading
    return tail.strip(': \n')[:800]


def hook_recall(main, arguments):
    """Short packet for hooks: top candidates with provenance and a snippet."""
    query = str(arguments.get('query') or '')
    limit = max(1, min(int(arguments.get('limit', 5)), 50))
    snippet = max(80, min(int(arguments.get('snippet', 400)), 4000))
    status = main.status()
    root = main.recall(query, status['pair_snapshot_id'])
    activation = root['receipt']['activation']
    judgments = {j.episode_id: j for j in activation.re_evidence.judgments}
    rows = []
    for candidate in activation.recall.candidates[:limit]:
        episode = main.memory.episode(candidate.episode_id)
        obs = episode.steps[0].observation
        judgment = judgments.get(candidate.episode_id)
        rows.append(dict(
            episode_id=candidate.episode_id, source=episode.source_addresses[0], revision=episode.revision,
            outcome=episode.steps[0].outcome, matched=len(candidate.matched_cues),
            matched_cues=list(candidate.matched_cues)[:12], cue_overlap=candidate.cue_overlap,
            proposition=obs.get('proposition_id'), polarity=obs.get('evidence_polarity'),
            verdict=judgment.verdict if judgment else None,
            superseded_by=main.memory.superseded.get(candidate.episode_id),
            metadata=dict(obs.get('metadata') or {}),
            text=obs.get('text', '')[:snippet], text_chars=len(obs.get('text', '')),
            # verdict records end with their asks line; hooks match query cues against it
            asks=_asks_of(obs.get('text', ''))))
    controls = activation.re_evidence
    return dict(status='ok', query=query, pair_snapshot_id=status['pair_snapshot_id'],
                candidate_count=len(activation.recall.candidates), returned=len(rows), memories=rows,
                should_abstain=controls.should_abstain, unresolved_conflict=controls.unresolved_conflict,
                conflicting_propositions=list(controls.conflicting_propositions),
                insufficient_evidence=controls.insufficient_evidence,
                selection={k: root['memory_selection'][k] for k in ('function_word_cues', 'candidate_order', 'closure_rule')},
                fanout={c: n for c, n in root['memory_selection']['candidate_counts'].items() if n},
                record_count=status['hot_episode_count'],
                grants_authority=False)


class Daemon:
    def __init__(self, state_dir, *, allow_ingest, idle_seconds):
        from .server import LocalResident
        from .store import Main
        self.state_dir = Path(state_dir)
        self.main = Main(self.state_dir, allow_ingest=allow_ingest)
        self.resident = LocalResident(self.main)
        self.lock = threading.Lock()
        self.idle_seconds = idle_seconds
        self.last = time.time()
        self.stop = threading.Event()

    def handle(self, message):
        command = message.get('command')
        arguments = {k: v for k, v in message.items() if k != 'command'}
        self.last = time.time()
        with self.lock:
            if command in RESIDENT_COMMANDS:
                return self.resident.request(command, **arguments)
            if command == 'ping':
                return dict(status='ok', pid=os.getpid(), state_dir=str(self.state_dir),
                            writes_enabled=self.main.allow_ingest)
            if command == 'hook_recall':
                return hook_recall(self.main, arguments)
            if command == 'ingest':
                return self.main.ingest(arguments)
            if command == 'checkpoint':
                return dict(status='ok', checkpoint=self.main.checkpoint())
            if command == 'shutdown':
                # Drop the port file now, not after the closing checkpoint: a client that
                # arrives meanwhile must spawn a fresh daemon, not attach to this dying one.
                try:
                    (self.state_dir / PORT_FILE).unlink()
                except OSError:
                    pass
                self.stop.set()
                return dict(status='stopping')
        raise InterfaceError('resident_operation_not_exported')

    def close(self):
        try:
            self.main.close()          # checkpoints when dirty
        finally:
            try:
                (self.state_dir / PORT_FILE).unlink()
            except OSError:
                pass


def serve(state_dir, *, port=0, allow_ingest=False, idle_hours=8.0):
    daemon = Daemon(state_dir, allow_ingest=allow_ingest, idle_seconds=idle_hours * 3600)

    class Handler(socketserver.StreamRequestHandler):
        def handle(self):
            # one connection may carry many requests, one JSON line each, until EOF
            while True:
                raw = self.rfile.readline(MAX_BYTES + 1)
                if not raw:
                    return
                try:
                    if not raw.endswith(b'\n'):
                        raise InterfaceError('resident_frame_invalid')
                    reply = daemon.handle(decode(raw))
                except (InterfaceError, ValueError, KeyError, TypeError) as error:
                    reply = dict(status='rejected', reason=str(error)[:200])
                except Exception as error:      # never take the daemon down for one request
                    reply = dict(status='rejected', reason='internal: ' + type(error).__name__)
                self.wfile.write(encode(reply) + b'\n')
                self.wfile.flush()
                if reply.get('status') == 'stopping':
                    return

    class Server(socketserver.ThreadingMixIn, socketserver.TCPServer):
        daemon_threads = True
        allow_reuse_address = False

    server = Server((HOST, port), Handler)
    actual = server.server_address[1]
    (daemon.state_dir / PORT_FILE).write_text(str(actual), encoding='ascii')
    thread = threading.Thread(target=server.serve_forever, kwargs={'poll_interval': 0.5}, daemon=True)
    thread.start()
    try:
        while not daemon.stop.is_set():
            time.sleep(1.0)
            if time.time() - daemon.last > daemon.idle_seconds:
                break
    finally:
        server.shutdown()
        server.server_close()
        daemon.close()
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--state-dir', required=True)
    parser.add_argument('--port', type=int, default=0)
    parser.add_argument('--allow-ingest', action='store_true')
    parser.add_argument('--idle-hours', type=float, default=8.0)
    options = parser.parse_args()
    try:
        return serve(options.state_dir, port=options.port, allow_ingest=options.allow_ingest,
                     idle_hours=options.idle_hours)
    except (ValueError, OSError) as error:
        print('VRS2 loopback daemon error: ' + str(error), file=sys.stderr)
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
