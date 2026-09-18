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
# .store (numpy, scipy, filelock: ~0.5 s) is imported only on the daemon side; a hook process needs the socket client alone

HOST = '127.0.0.1'
PORT_FILE = 'loopback.port'
RESIDENT_COMMANDS = {'status', 'cognitive_dialogue_start', 'cognitive_dialogue_continue',
                     'cognitive_dialogue_evidence_open', 'cognitive_dialogue_evidence',
                     'cognitive_dialogue_release'}
LOCAL_COMMANDS = {'hook_recall', 'ingest', 'ingest_many', 'checkpoint', 'compact', 'consolidate', 'refine', 'ping', 'shutdown', 'usage', 'alias'}


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
    popen = dict(stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, close_fds=True)
    if os.name == 'nt':
        popen['creationflags'] = subprocess.CREATE_NEW_PROCESS_GROUP | getattr(subprocess, 'DETACHED_PROCESS', 0)
    else:
        popen['start_new_session'] = True      # POSIX: a new session so the daemon outlives the hook that spawned it
    subprocess.Popen(command, **popen)
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
    """The record's own "찾을 때 묻는 말" (verdict tail line, memory-doc section, log-entry tail), else ''."""
    from .store import asks_of
    return asks_of(text)


def hook_recall(main, arguments):
    """Short packet for hooks: top candidates with provenance and a snippet."""
    query = str(arguments.get('query') or '')
    limit = max(1, min(int(arguments.get('limit', 5)), 50))
    snippet = max(80, min(int(arguments.get('snippet', 400)), 4000))
    exclude = tuple(str(k) for k in (arguments.get('exclude_kinds') or ()) if k)[:8]
    scope = arguments.get('region_scope') if arguments.get('region_scope') in ('all', 'regions', 'auto') else 'all'
    root = main.recall(query, None, exclude_kinds=exclude, region_scope=scope)      # current generation, no lock
    status = {'pair_snapshot_id': root['pair_snapshot_id']}
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
            matched_cues=list(candidate.matched_cues)[:64], cue_overlap=candidate.cue_overlap,   # 12 undercounted 9% of rows (2026-09-14)
            proposition=obs.get('proposition_id'), polarity=obs.get('evidence_polarity'),
            verdict=judgment.verdict if judgment else None,
            superseded_by=main.memory.superseded.get(candidate.episode_id),
            metadata=dict(obs.get('metadata') or {}),
            text=obs.get('text', '')[:snippet], text_chars=len(obs.get('text', '')),
            # verdict records end with their asks line; hooks match query cues against it
            asks=_asks_of(obs.get('text', '')),
            # vrs-regions: refined strength, promotion, state/stability, pending (not yet consolidated)
            vrs=main.graph.vrs_of(candidate.episode_id, episode.source_addresses[0]),
            # G6: how this candidate was reached — local region, via a candidate portal, or unbridged
            region=root['region_navigation']['paths'].get(candidate.episode_id)))
    # repeat counter (2026-09-18): how often the sessions repeated a verdict's mistake — observations from
    # producer 'gate' (a guard blocked the attempt) or a manual repeat note, one per session
    repeats = {}
    for row in rows:
        pid = row.get('proposition')
        if not pid or pid in repeats:
            continue
        sessions = set(); count = 0
        for eid in main.memory.propositions.get(pid, ()):
            if eid in main.memory.superseded:
                continue
            ep = main.memory.episode(eid); meta = ep.steps[0].observation.get('metadata') or {}
            if meta.get('producer') == 'gate' or ep.source_addresses[0].startswith('gate:'):
                count += 1; sessions.add(str(meta.get('project') or ep.source_addresses[0]))
        repeats[pid] = dict(observations=count, sessions=len(sessions))
    for row in rows:
        if row.get('proposition') in repeats:
            row['repeats'] = repeats[row['proposition']]
    controls = activation.re_evidence
    stable = main.graph.stable
    # current-vs-past collision (2026-09-17): for each proposition the re-evidence stage found in
    # conflict among the activated candidates, hand the hook both sides (source, producer, date,
    # polarity) and the accumulator's standing decision, so the main can see *what* collided and
    # answer it with a session observation (vrs2-confirm.py) instead of a bare "conflict" flag.
    conflicts = []
    for proposition in controls.conflicting_propositions:
        sides = dict(support=[], refute=[])
        for candidate in activation.recall.candidates:
            if candidate.episode_id in main.memory.superseded:
                continue
            episode = main.memory.episode(candidate.episode_id)
            obs = episode.steps[0].observation
            if obs.get('proposition_id') != proposition:
                continue
            meta = obs.get('metadata') or {}
            polarity = obs.get('evidence_polarity')
            if polarity in sides:
                sides[polarity].append(dict(source=episode.source_addresses[0][:120], producer=meta.get('producer'),
                                            date=meta.get('date') or (str(episode.revision)[:10] if str(episode.revision)[:4].isdigit() else ''),
                                            outcome=episode.steps[0].outcome,
                                            episode_id=candidate.episode_id))
        decision = (stable.decisions or {}).get(proposition) if stable is not None and getattr(stable, 'decisions', None) else None
        conflicts.append(dict(proposition=proposition, support=sides['support'][:4], refute=sides['refute'][:4],
                              decision=None if decision is None else {k: decision.get(k) for k in ('status', 'reason', 'unresolved', 'source_diversity')}))
    return dict(status='ok', query=query, pair_snapshot_id=status['pair_snapshot_id'], conflicts=conflicts,
                candidate_count=len(activation.recall.candidates), returned=len(rows), memories=rows,
                should_abstain=controls.should_abstain, unresolved_conflict=controls.unresolved_conflict,
                conflicting_propositions=list(controls.conflicting_propositions),
                insufficient_evidence=controls.insufficient_evidence,
                selection={k: root['memory_selection'][k] for k in ('function_word_cues', 'candidate_order', 'closure_rule')},
                fanout={c: n for c, n in root['memory_selection']['candidate_counts'].items() if n},
                record_count=root['record_count'],
                vrs_stable=None if stable is None else dict(version_id=stable.version_id, created=stable.created,
                                                            promoted=stable.promoted, converged=stable.converged,
                                                            stale=main.consolidation_stale()),
                region_navigation={k: root['region_navigation'][k] for k in ('active_regions', 'portals', 'unbridged_factor', 'scope')},
                rejected_paths=root['region_navigation']['rejected'][:20],
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
        # Reads run without the lock (2026-09-15): a generation is immutable and Main publishes the
        # next one as a single tuple, so a recall during an ingest sees the old or the new store,
        # never a mix, and a Stop hook's ingest burst no longer stalls another session's prompt hook.
        if command == 'ping':
            return dict(status='ok', pid=os.getpid(), state_dir=str(self.state_dir),
                        writes_enabled=self.main.allow_ingest)
        if command == 'hook_recall':
            return hook_recall(self.main, arguments)
        if command in ('consolidate', 'refine'):
            # manual consolidation: the refinement is lock-free (frozen generation); prepare/commit lock briefly
            with self.lock:
                prepared = self.main.consolidate_prepare()
            graph = self.main.consolidate_run(prepared, **{k: int(v) for k, v in arguments.items() if k in ('seed', 'cycles')})
            with self.lock:
                result = self.main.consolidate_commit(prepared, graph)
            return dict(status='ok', consolidate=result, raced=result is None)
        with self.lock:
            if command in RESIDENT_COMMANDS:
                return self.resident.request(command, **arguments)
            if command == 'ingest':
                return self.main.ingest(arguments)
            if command == 'ingest_many':
                # batch generations (2026-09-18): K observations -> one generation; all-or-nothing
                return self.main.ingest_many(list(arguments.get('rows') or []))
            if command == 'alias':
                # hypothesis registry (2026-09-18): bind alias propositions to a canonical one
                return self.main.alias_update(arguments.get('canonical'), arguments.get('aliases') or [])
            if command == 'usage':
                # usage re-evidence (2026-09-18): the Stop hook's ledger {source: [injected, opened]}
                return self.main.usage_update(arguments.get('counts') or {})
            if command == 'checkpoint':
                return dict(status='ok', checkpoint=self.main.checkpoint())
            if command == 'compact':
                return dict(status='ok', compact=self.main.compact())
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
    from .store import CHECKPOINT_IDLE
    from .vrs_refine import IDLE_CYCLES
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
            quiet = time.time() - daemon.last
            if quiet > daemon.idle_seconds:
                break
            # checkpoint after a quiet spell, not inside an ingest: a Stop hook's burst of
            # entries pays for one checkpoint, after it, instead of one per 8 ingests
            if daemon.main.dirty and quiet >= CHECKPOINT_IDLE:
                # serialize outside the lock (2-3 s at 5k records) so a concurrent hook recall does
                # not wait; only the short DB write holds the lock
                with daemon.lock:
                    prepared = daemon.main.checkpoint_prepare() if daemon.main.dirty else None
                if prepared is not None:
                    serialized = daemon.main.checkpoint_serialize(prepared)
                    with daemon.lock:
                        daemon.main.checkpoint_commit(prepared, serialized)
            # VRS consolidation (vrs-regions): once the daemon is quiet and the graph has edges the stable
            # version has not refined (or it has not converged), refine one chunk region by region — outside
            # the lock but for prepare/commit — and come back for the next chunk until converged.
            if (quiet >= CHECKPOINT_IDLE and daemon.main.allow_ingest and daemon.main.consolidation_stale()
                    and time.time() >= getattr(daemon, 'consolidation_backoff', 0)):
                with daemon.lock:
                    prepared = daemon.main.consolidate_prepare()
                try:
                    graph = daemon.main.consolidate_run(prepared, cycles=IDLE_CYCLES)
                    with daemon.lock:
                        daemon.main.consolidate_commit(prepared, graph)
                    time.sleep(0.5)                       # let a concurrent hook recall through between chunks
                except Exception as error:               # never let a consolidation stop the daemon
                    daemon.main.restore['consolidation'] = type(error).__name__ + ': ' + str(error)[:200]
                    daemon.consolidation_backoff = time.time() + 600
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
