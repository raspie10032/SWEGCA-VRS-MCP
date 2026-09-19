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
LOCAL_COMMANDS = {'hook_recall', 'evidence_of', 'origins', 'bundles', 'lookup', 'evict', 'ingest', 'ingest_many', 'checkpoint', 'compact', 'consolidate', 'refine', 'ping', 'shutdown', 'usage', 'alias'}


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


def ensure_daemon(state_dir, *, allow_ingest=True, python=None, wait_seconds=30, bundle_limit=None,
                  bundles=None, hot_bundles=None):
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
    if bundle_limit:
        command += ['--bundle-limit', str(int(bundle_limit))]
    for bundle_id, directory in (bundles or {}).items():      # G7 resident layer (2026-09-19): other bundles, warm
        if directory:
            command += ['--bundle', f'{bundle_id}={directory}']
    if hot_bundles is not None:
        command += ['--hot-bundles', str(int(hot_bundles))]
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


def evidence_of(main, arguments):
    """Live evidence rows of one proposition, optionally at one source (lock-free read, 2026-09-18).
    A producer re-measuring the same bench at the same source supersedes its earlier row with this."""
    memory = main.memory
    proposition = str(arguments.get('proposition') or '')
    source = arguments.get('source')
    rows = []
    for identifier in memory.propositions.get(proposition, ()):
        if identifier in memory.superseded:
            continue
        episode = memory.episode_light(identifier) if hasattr(memory, 'episode_light') else memory.episode(identifier)
        obs = episode.steps[0].observation
        if source and episode.source_addresses[0] != str(source):
            continue
        rows.append(dict(episode_id=identifier, source=episode.source_addresses[0], revision=episode.revision,
                         outcome=episode.steps[0].outcome, polarity=obs.get('evidence_polarity'),
                         producer=(obs.get('metadata') or {}).get('producer')))
    rows.sort(key=lambda r: r['revision'])
    return dict(status='ok', proposition=proposition, rows=rows)


def _record_digest(text):
    from .harness.origin import record_digest
    return record_digest(text)


def _plain(value):
    from .store import plain
    return plain(value)


FILE_KINDS = ('log_entry', 'doc', 'doc_section')


def origins(main, arguments):
    """Origin facts of every live file-backed record (G3, 2026-09-19; lock-free read): what the verifier
    needs to check each source without the text — path, kind, recorded origin (or the full text's
    digest for rows from before the binding), the head line and section title for re-locating."""
    memory = main.memory
    kinds = tuple(str(k) for k in (arguments.get('kinds') or FILE_KINDS))
    store = memory._store
    ids, kind_column = store['ids'], store.get('kinds') or []
    offset = max(0, int(arguments.get('offset') or 0))
    limit = max(1, min(int(arguments.get('limit') or 1000), 2000))      # a page stays under the transport's 1 MB line
    rows, count, next_row = [], memory.count, None
    for row in range(offset, count):
        if len(rows) >= limit:
            next_row = row
            break
        if row < len(kind_column) and kind_column[row] not in kinds:
            continue
        identifier = ids[row]
        if identifier in memory.superseded:
            continue
        episode = memory.episode_light(identifier)
        obs = episode.steps[0].observation
        meta = obs.get('metadata') or {}
        if meta.get('kind') not in kinds or not meta.get('path'):
            continue
        text = obs.get('text', '')
        rows.append(dict(episode_id=identifier, source=episode.source_addresses[0], revision=episode.revision,
                         kind=meta.get('kind'), path=meta.get('path'), section=meta.get('section') or '', index=meta.get('index'),
                         project=meta.get('project'), head=text.split('\n', 1)[0][:120],
                         origin=_plain(meta.get('origin')), text_sha256=None if meta.get('origin') else _record_digest(text)))
    return dict(status='ok', count=len(rows), rows=rows, next=next_row, total=count)


def hook_recall(main, arguments, resident=None):
    """Short packet for hooks: top candidates with provenance and a snippet. With a resident (G7, 2026-09-19)
    the primary's rows come first (VRS, regions, re-evidence), then every other bundle's rows in its own
    index order, each tagged ``bundle``/``bundle_state`` — the hook says which bundle a memory came from."""
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
    superseded_skipped = 0
    for candidate in activation.recall.candidates:
        if len(rows) >= limit:
            break
        # live candidates fill the packet (2026-09-19): an older revision of a source stays recallable in the
        # store, but the hook never shows it, so it must not take a slot — measured on 8 prompts: 63/80 live,
        # one prompt 2/10 (a doc with 7 revisions took 6 slots). `superseded_by` stays in the row schema.
        if candidate.episode_id in main.memory.superseded:
            superseded_skipped += 1
            continue
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
            metadata=_plain(obs.get('metadata') or {}),      # nested maps (origin) are frozen in the store: plain for JSON
            text=obs.get('text', '')[:snippet], text_chars=len(obs.get('text', '')),
            # G3 origin binding (2026-09-19): rows ingested before it carry no origin; the hook verifies the
            # source against the full text's digest instead (hashing here is outside main.recall's hot path)
            text_sha256=None if (obs.get('metadata') or {}).get('origin') else _record_digest(obs.get('text', '')),
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
        row['bundle'] = 'main'
    others = []
    if resident is not None and resident.ids():
        others = resident.recall_others(query, exclude, limit, snippet)
        for bundle in others:
            rows.extend(bundle.get('rows') or [])
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
                superseded_skipped=superseded_skipped,
                bundles=[{k: v for k, v in b.items() if k != 'rows'} for b in others],
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
    def __init__(self, state_dir, *, allow_ingest, idle_seconds, bundle_limit=None, bundles=None, hot_bundles=1):
        from .server import LocalResident
        from .store import Main
        from .resident import Resident
        self.state_dir = Path(state_dir)
        self.main = Main(self.state_dir, allow_ingest=allow_ingest, bundle_limit=bundle_limit)
        self.resident = LocalResident(self.main)
        # G7 resident layer (2026-09-19): the primary bundle hot, other registered bundles warm (index only)
        # or hot by use; an ingest names its bundle, a recall reaches all of them
        self.bundles = Resident(self.main, bundles or {}, hot_limit=hot_bundles, bundle_limit=bundle_limit)
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
                        writes_enabled=self.main.allow_ingest, bundles=self.bundles.ids())
        if command == 'hook_recall':
            return hook_recall(self.main, arguments, self.bundles)
        if command == 'bundles':
            return dict(status='ok', bundles=self.bundles.status(), hot_limit=self.bundles.hot_limit)
        if command == 'lookup':
            return dict(status='ok', episode_id=arguments.get('episode_id'), bundle=self.bundles.lookup(str(arguments.get('episode_id') or '')))
        if command == 'origins':
            return origins(self.main, arguments)
        if command == 'evidence_of':
            return evidence_of(self.main, arguments)
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
                result = self.resident.request(command, **arguments)
                if command == 'status':
                    result['bundles'] = self.bundles.status()
                return result
            if command == 'ingest':
                target = self.bundles.main_for(arguments.pop('bundle', None))     # G7: routed by bundle id
                return target.ingest(arguments)
            if command == 'ingest_many':
                # batch generations (2026-09-18): K observations -> one generation; all-or-nothing
                target = self.bundles.main_for(arguments.get('bundle'))
                return target.ingest_many(list(arguments.get('rows') or []))
            if command == 'evict':
                return dict(status='ok', evicted=self.bundles.evict(str(arguments.get('bundle') or '')))
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
            self.bundles.close()       # other hot bundles: checkpoint + close; warm views dropped
        except Exception:
            pass
        try:
            self.main.close()          # checkpoints when dirty
        finally:
            try:
                (self.state_dir / PORT_FILE).unlink()
            except OSError:
                pass


def serve(state_dir, *, port=0, allow_ingest=False, idle_hours=8.0, bundle_limit=None, bundles=None, hot_bundles=1):
    from .store import CHECKPOINT_IDLE
    from .vrs_refine import IDLE_CYCLES
    daemon = Daemon(state_dir, allow_ingest=allow_ingest, bundle_limit=bundle_limit, bundles=bundles, hot_bundles=hot_bundles, idle_seconds=idle_hours * 3600)

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
            # G7 (2026-09-19): other hot bundles checkpoint after the same quiet spell — their warm blob is what a
            # warm view (this or another process) reads; consolidation of a secondary bundle waits for its own turn
            # as primary (it is not run here)
            if quiet >= CHECKPOINT_IDLE:
                for bundle_id, other in list(daemon.bundles.hot.items()):
                    if other.dirty:
                        with daemon.lock:
                            prepared = other.checkpoint_prepare() if other.dirty else None
                        if prepared is not None:
                            serialized = other.checkpoint_serialize(prepared)
                            with daemon.lock:
                                other.checkpoint_commit(prepared, serialized)
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
    parser.add_argument('--bundle-limit', type=int, default=0, help='recommended records per bundle (soft; docs/SIZING.md)')
    parser.add_argument('--bundle', action='append', default=[], metavar='ID=DIR', help='another bundle this daemon answers for (warm; hot when ingested into)')
    parser.add_argument('--hot-bundles', type=int, default=1, help='how many other bundles may stay hot (owned) at once')
    options = parser.parse_args()
    bundles = dict(item.split('=', 1) for item in options.bundle if '=' in item)
    try:
        return serve(options.state_dir, port=options.port, allow_ingest=options.allow_ingest,
                     idle_hours=options.idle_hours, bundle_limit=options.bundle_limit or None,
                     bundles=bundles, hot_bundles=options.hot_bundles)
    except (ValueError, OSError) as error:
        print('VRS2 loopback daemon error: ' + str(error), file=sys.stderr)
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
