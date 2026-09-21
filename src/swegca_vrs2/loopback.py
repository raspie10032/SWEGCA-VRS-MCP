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
START_FILE = 'loopback.starting'   # G8 (2026-09-19): written by the spawner, removed by the daemon once it serves
START_STALE = 300                    # seconds after which a start marker is ignored (a spawn that died)
RESIDENT_COMMANDS = {'status', 'cognitive_dialogue_start', 'cognitive_dialogue_continue',
                     'cognitive_dialogue_evidence_open', 'cognitive_dialogue_evidence',
                     'cognitive_dialogue_release'}
LOCAL_COMMANDS = {'hook_recall', 'evidence_of', 'origins', 'turns', 'bundles', 'lookup', 'operation', 'evict', 'ingest', 'ingest_many', 'checkpoint', 'compact', 'consolidate', 'refine', 'ping', 'shutdown', 'usage', 'alias',
                  'sessions', 'session_end', 'session_merge', 'merge_receipts'}    # 2.2: the session producer layer


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


class DaemonStarting(InterfaceError):
    """The daemon is loading (spawned by this or an earlier caller) and was not ready within the wait:
    the caller answers without memory now and names the miss; the next call attaches (G8)."""


def starting_since(state_dir):
    """Seconds since a live start marker was written, or None (no marker, stale, or the daemon is up)."""
    try:
        text = (Path(state_dir) / START_FILE).read_text(encoding='ascii').strip()
        pid, stamp = text.split()
        age = time.time() - float(stamp)
    except (OSError, ValueError):
        return None
    if age > START_STALE:
        return None
    return age


def _clear_start_marker(state_dir):
    try:
        (Path(state_dir) / START_FILE).unlink()
    except OSError:
        pass


def ensure_daemon(state_dir, *, allow_ingest=True, python=None, wait_seconds=30, bundle_limit=None,
                  bundles=None, hot_bundles=None):
    """Return a client for the daemon owning ``state_dir``, starting it if needed. A daemon that another
    caller already spawned is not spawned again (start marker); when it is not up within ``wait_seconds``
    ``DaemonStarting`` is raised — a miss the caller can name instead of a blocked prompt (G8)."""
    state_dir = Path(state_dir)
    port = port_of(state_dir)
    if port is not None:
        client = LoopbackClient(port, 5)
        try:
            client.request('ping')
            return client
        except InterfaceError:
            pass
    if starting_since(state_dir) is not None:
        return _wait_for_daemon(state_dir, wait_seconds)
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
    process = subprocess.Popen(command, **popen)
    try:
        (state_dir / START_FILE).write_text(f'{process.pid} {time.time():.3f}', encoding='ascii')
    except OSError:
        pass
    return _wait_for_daemon(state_dir, wait_seconds)


def _wait_for_daemon(state_dir, wait_seconds):
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
    if starting_since(state_dir) is not None:
        raise DaemonStarting('resident_daemon_starting')
    raise InterfaceError('resident_daemon_did_not_start')


# ── daemon ────────────────────────────────────────────────────────────

def _asks_of(text):
    """The record's own "찾을 때 묻는 말" (verdict tail line, memory-doc section, log-entry tail), else ''."""
    from .store import asks_of
    return asks_of(text)


def _decision_of(main, proposition):
    """G11 (2026-09-19): the accumulator's standing decision for a proposition from the stable generation (the
    aliases fold into the canonical id), with what it still lacks named. None before the first consolidation."""
    stable = getattr(main.graph, 'stable', None)
    decisions = getattr(stable, 'decisions', None) if stable is not None else None
    if not decisions:
        return None
    aliases = getattr(main.graph, 'aliases', None) or {}
    summary = decisions.get(aliases.get(proposition, proposition))
    if summary is None:
        return None
    from .vrs_evidence import gaps, gaps_text
    return dict(status=summary.get('status'), reason=summary.get('reason'), unresolved=summary.get('unresolved'),
                source_diversity=summary.get('source_diversity'), context_diversity=summary.get('context_diversity'),
                axes=summary.get('axes'), producers=summary.get('producers'), contexts=summary.get('contexts'),
                families=summary.get('families'), gaps=gaps(summary), text=gaps_text(summary))


def evidence_of(main, arguments):
    """Live evidence rows of one proposition, optionally at one source (lock-free read, 2026-09-18).
    A producer re-measuring the same bench at the same source supersedes its earlier row with this.
    G11: the reply also carries the accumulator's standing decision with its gaps (``decision``)."""
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
        meta = obs.get('metadata') or {}
        rows.append(dict(episode_id=identifier, source=episode.source_addresses[0], revision=episode.revision,
                         outcome=episode.steps[0].outcome, polarity=obs.get('evidence_polarity'),
                         producer=meta.get('producer'), axes=list(meta.get('axes') or ()),
                         context=meta.get('project'), run=_plain(meta.get('run')) if meta.get('run') else None))
    rows.sort(key=lambda r: r['revision'])
    return dict(status='ok', proposition=proposition, rows=rows, decision=_decision_of(main, proposition))


def _record_digest(text):
    from .harness.origin import record_digest
    return record_digest(text)


def _verify_producer(row):
    """Signed producers (2026-09-19): a row from a registered producer is stamped ``metadata.verified`` by its
    signature; an unregistered producer's row is left as it is. Never blocks an ingest."""
    try:
        from .harness.identity import verify_row
        return verify_row(row)
    except Exception:
        return None


def _light(memory, identifier):
    return memory.episode_light(identifier) if hasattr(memory, 'episode_light') else memory.episode(identifier)


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
        if meta.get('kind') not in kinds or (meta.get('kind') in FILE_KINDS and not meta.get('path')):
            continue                                       # evidence rows (signed-producer audit) have no path
        text = obs.get('text', '')
        rows.append(dict(episode_id=identifier, source=episode.source_addresses[0], revision=episode.revision,
                         kind=meta.get('kind'), path=meta.get('path'), section=meta.get('section') or '', index=meta.get('index'),
                         project=meta.get('project'), head=text.split('\n', 1)[0][:120],
                         producer=meta.get('producer'), verified=meta.get('verified'), key_id=meta.get('key_id'),   # signed producers
                         origin=_plain(meta.get('origin')), text_sha256=None if meta.get('origin') else _record_digest(text)))
    return dict(status='ok', count=len(rows), rows=rows, next=next_row, total=count)


def turns(main, arguments):
    """The last ``limit`` conversation turns of one session as the store holds them (real-time transcript rows,
    2026-09-21; lock-free read): what the SessionStart hook injects after a compaction — the experience of the
    cut turn with its place in the log, from the store, not from the log. ``before_line`` keeps only rows whose
    span starts before that log line (the boundary), so a row the tail sent after the compaction is not
    mistaken for what was lost."""
    memory = main.memory
    session = str(arguments.get('session') or '')
    limit = max(1, min(int(arguments.get('limit') or 3), 20))
    snippet = max(200, min(int(arguments.get('snippet') or 1200), 6000))
    before = arguments.get('before_line')
    store = memory._store
    ids, kind_column = store['ids'], store.get('kinds') or []
    found = []
    for row in range(memory.count):
        if row < len(kind_column) and kind_column[row] != 'transcript':
            continue
        identifier = ids[row]
        if identifier in memory.superseded:
            continue
        episode = memory.episode_light(identifier)
        obs = episode.steps[0].observation
        meta = obs.get('metadata') or {}
        if meta.get('kind') != 'transcript' or (session and str(meta.get('session') or '') != session):
            continue
        lines = list((meta.get('origin') or {}).get('lines') or meta.get('lines') or [0, 0])
        if before is not None and lines and int(lines[0]) >= int(before):
            continue
        found.append((int(meta.get('turn') or 0), int(lines[0] or 0), identifier, meta, obs.get('text', '')))
    found.sort(key=lambda f: (f[0], f[1]))
    rows = []
    for turn, first, identifier, meta, text in found[-limit:]:
        rows.append(dict(episode_id=identifier, turn=turn, part=meta.get('part'), lines=list((meta.get('origin') or {}).get('lines') or meta.get('lines') or []),
                         path=meta.get('path'), date=meta.get('date'), agent=meta.get('agent'), session=meta.get('session'),
                         text=text[:snippet], text_chars=len(text), origin=_plain(meta.get('origin')), snapshot=_plain(meta.get('snapshot'))))
    return dict(status='ok', session=session, count=len(rows), rows=rows, total=len(found))


def hook_recall(main, arguments, resident=None, sessions=None):
    """Short packet for hooks: top candidates with provenance and a snippet. With a resident (G7, 2026-09-19)
    the primary's rows come first (VRS, regions, re-evidence), then every other bundle's rows in its own
    index order, each tagged ``bundle``/``bundle_state`` — the hook says which bundle a memory came from.

    2.2 (2026-09-21): with a session layer and a ``session`` argument the judgment reads the session's proposal
    journal first (bounded exact top-K: Replay and Re-evidence for the packet's rows) and main only on a complete
    miss — no cue of the query has a posting there. The packet says which layer answered (``layer``) and, on a
    fall-through, names the miss. The packet is a selection receipt: it grants no authority (SWEGCA §3.1)."""
    query = str(arguments.get('query') or '')
    limit = max(1, min(int(arguments.get('limit', 5)), 50))
    snippet = max(80, min(int(arguments.get('snippet', 400)), 4000))
    exclude = tuple(str(k) for k in (arguments.get('exclude_kinds') or ()) if k)[:8]
    scope = arguments.get('region_scope') if arguments.get('region_scope') in ('all', 'regions', 'auto') else 'all'
    session = str(arguments.get('session') or '')
    # G8 (2026-09-19): the judgment is timed apart from what follows it, and what it had to decode is counted —
    # a hot hit and a miss are not the same number
    began = time.perf_counter_ns()
    stats0 = main.memory.stats() if hasattr(main.memory, 'stats') else {}
    layer, root, layer_miss = 'main', None, None
    if sessions is not None and session:
        try:
            hit = sessions.recall(session, query, exclude_kinds=exclude, limit=limit, region_scope='all')
        except ValueError:
            hit = None
        if hit is not None:
            main, root = hit                          # the session's own store answers; main is not read
            layer = 'session'
        else:
            layer_miss = dict(kind='session_miss', session=session[:12],
                              state='absent' if not sessions.exists(session) else 'complete_miss')
    if root is None:
        root = main.recall(query, None, exclude_kinds=exclude, region_scope=scope)      # current generation, no lock
    judged = time.perf_counter_ns()
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
        episode = _light(main.memory, candidate.episode_id)      # G8: the packet needs no cue strings — light, prefetched
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
            ep = _light(main.memory, eid); meta = ep.steps[0].observation.get('metadata') or {}
            if meta.get('producer') == 'gate' or ep.source_addresses[0].startswith('gate:'):
                count += 1; sessions.add(str(meta.get('project') or ep.source_addresses[0]))
        repeats[pid] = dict(observations=count, sessions=len(sessions))
    decisions = {}
    for row in rows:
        if row.get('proposition') in repeats:
            row['repeats'] = repeats[row['proposition']]
        row['bundle'] = 'main'
        row['layer'] = layer
        pid = row.get('proposition')
        if pid:
            # G11: the accumulator's standing decision and its named gaps, so the hook can say what the claim
            # still lacks (which axis, how many producers/contexts) instead of a bare abstain
            if pid not in decisions:
                decisions[pid] = _decision_of(main, pid)
            row['decision'] = decisions[pid]
    rowed = time.perf_counter_ns()
    others = []
    if resident is not None and resident.ids():
        others = resident.recall_others(query, exclude, limit, snippet)
        for bundle in others:
            rows.extend(bundle.get('rows') or [])
    stats1 = main.memory.stats() if hasattr(main.memory, 'stats') else {}
    decodes = stats1.get('decodes', 0) - stats0.get('decodes', 0)
    hits = stats1.get('hits', 0) - stats0.get('hits', 0)
    misses = [dict(kind='bundle_not_ready', bundle=b['bundle'], state=b['state']) for b in others if b.get('miss')]
    if decodes:
        misses.append(dict(kind='blob_decodes', count=decodes))
    if layer_miss is not None:
        misses.append(layer_miss)
    timing = dict(judgment_ms=(judged - began) // 1_000_000, rows_ms=(rowed - judged) // 1_000_000,
                  bundles_ms=(time.perf_counter_ns() - rowed) // 1_000_000)
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
            episode = _light(main.memory, candidate.episode_id)
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
                candidate_count=root['memory_selection'].get('candidate_count', len(activation.recall.candidates)), returned=len(rows), memories=rows,
                superseded_skipped=superseded_skipped,
                layer=layer, session=session[:12] or None,                        # 2.2: which layer answered
                judged=root['memory_selection'].get('judged'), judge_bound=root['memory_selection'].get('judge_bound'),
                bundles=[{k: v for k, v in b.items() if k != 'rows'} for b in others],
                misses=misses, timing=timing, blob_hits=hits, blob_decodes=decodes,   # G8: hit and miss, named
                should_abstain=controls.should_abstain, unresolved_conflict=controls.unresolved_conflict,
                conflicting_propositions=list(controls.conflicting_propositions),
                insufficient_evidence=controls.insufficient_evidence,
                selection={k: root['memory_selection'][k] for k in ('function_word_cues', 'candidate_order', 'closure_rule')},
                fanout={c: n for c, n in root['memory_selection']['candidate_counts'].items() if n},
                record_count=root['record_count'],
                vrs_stable=None if stable is None else dict(version_id=stable.version_id, created=stable.created,
                                                            promoted=stable.promoted, converged=stable.converged,
                                                            stale=main.consolidation_stale()),
                region_navigation={k: root['region_navigation'].get(k) for k in ('active_regions', 'portals', 'unbridged_factor', 'scope',
                                                                             'path_counts', 'crossings_keyed', 'shared')},   # G5: paths by kind, crossings through a shared experience
                rejected_paths=root['region_navigation']['rejected'][:20],
                grants_authority=False)


class Daemon:
    def __init__(self, state_dir, *, allow_ingest, idle_seconds, bundle_limit=None, bundles=None, hot_bundles=1,
                 session_layer=None, session_idle=None, session_hot=None):
        from .server import LocalResident
        from .store import Main
        from .resident import Resident
        self.state_dir = Path(state_dir)
        self.main = Main(self.state_dir, allow_ingest=allow_ingest, bundle_limit=bundle_limit)
        self.resident = LocalResident(self.main)
        # G7 resident layer (2026-09-19): the primary bundle hot, other registered bundles warm (index only)
        # or hot by use; an ingest names its bundle, a recall reaches all of them
        self.bundles = Resident(self.main, bundles or {}, hot_limit=hot_bundles, bundle_limit=bundle_limit)
        self.prepared = []                                # G8: the preparer's receipts (prefetch, bundle loads)
        self.lock = threading.Lock()
        self.idle_seconds = idle_seconds
        self.last = time.time()
        self.stop = threading.Event()
        # 2.2 (2026-09-21): the session producer layer — an ingest that names its session writes the session's
        # proposal journal, never main; a hook_recall that names its session reads it first; the merge transaction
        # commits a finished session into main as one generation. Off with VRS2_SESSION_LAYER=0 (main-only, as 2.1).
        if session_layer is None:
            session_layer = os.environ.get('VRS2_SESSION_LAYER', '1') not in ('0', 'off', 'false')
        self.sessions = self.merge = self.merger = None
        self.recovered = []
        if session_layer and allow_ingest:
            from .session_producer import SessionLayer, SESSION_IDLE_S, SESSION_HOT
            from .merge import MergeTransaction, Merger
            self.sessions = SessionLayer(self.state_dir, hot_limit=session_hot or SESSION_HOT, bundle_limit=bundle_limit,
                                         idle_seconds=SESSION_IDLE_S if session_idle is None else session_idle)
            self.merge = MergeTransaction(self.main, self.sessions, self.lock)
            self.recovered = self.merge.recover()      # a merge the last daemon died inside finishes or rolls back
            self.merger = Merger(self.merge)

    def _journal_of(self, session):
        """The open proposal journal of ``session`` for a read, or None (no layer, no session, no journal, merging)."""
        if self.sessions is None or not session or not self.sessions.exists(session):
            return None
        try:
            return self.sessions.main_for(session, create=False)
        except ValueError:
            return None

    def handle(self, message):
        command = message.get('command')
        arguments = {k: v for k, v in message.items() if k != 'command'}
        self.last = time.time()
        # Reads run without the lock (2026-09-15): a generation is immutable and Main publishes the
        # next one as a single tuple, so a recall during an ingest sees the old or the new store,
        # never a mix, and a Stop hook's ingest burst no longer stalls another session's prompt hook.
        if command == 'ping':
            return dict(status='ok', pid=os.getpid(), state_dir=str(self.state_dir),
                        writes_enabled=self.main.allow_ingest, bundles=self.bundles.ids(),
                        session_layer=self.sessions is not None)
        if command == 'hook_recall':
            return hook_recall(self.main, arguments, self.bundles, self.sessions)
        if command == 'sessions':
            return dict(status='ok', enabled=self.sessions is not None,
                        sessions=self.sessions.status() if self.sessions else [],
                        merger=self.merger.status() if self.merger else None, recovered=list(self.recovered)[:8],
                        idle_seconds=self.sessions.idle_seconds if self.sessions else None)
        if command == 'merge_receipts':
            return dict(status='ok', receipts=self.merge.receipts(int(arguments.get('limit', 20))) if self.merge else [])
        if command in ('session_end', 'session_merge'):
            # the producer says it is finished (SessionEnd hook) — or a merge is asked for outright; the merge runs
            # off the request path unless ``wait`` is set (tests, tools)
            if self.sessions is None:
                return dict(status='ok', enabled=False)
            session = str(arguments.get('session') or '')
            if not session:
                raise InterfaceError('session_required')
            marked = self.sessions.end(session, reason=command) if self.sessions.exists(session) else False
            if not marked:
                return dict(status='ok', session=session[:12], journal=False, queued=False)
            if arguments.get('wait'):
                receipt = self.merge.merge(self.sessions.path(session).name)
                return dict(status='ok', session=session[:12], journal=True, merged=receipt)
            return dict(status='ok', session=session[:12], journal=True, queued=self.merger.submit(self.sessions.path(session).name))
        if command == 'bundles':
            return dict(status='ok', bundles=self.bundles.status(), hot_limit=self.bundles.hot_limit,
                        prepared=list(self.prepared[-16:]), wanted=sorted(self.bundles.wanted),
                        blob=self.main.memory.stats() if hasattr(self.main.memory, 'stats') else None)
        if command == 'lookup':
            return dict(status='ok', episode_id=arguments.get('episode_id'), bundle=self.bundles.lookup(str(arguments.get('episode_id') or '')))
        if command == 'operation':
            # 2026-09-21: what a request id already stands for — the reindex recovers a manifest it lost (a hook
            # killed after the daemon accepted) instead of re-sending the id with other content forever
            request_id = str(arguments.get('request_id') or '')
            existing, layer = self.main.operations.get(request_id), 'main'
            session = arguments.get('session')
            if self.sessions is not None and session and self.sessions.exists(session):
                try:                                       # 2.2: what the session's own journal holds under the id
                    journal = self.sessions.main_for(session, create=False)
                    held = journal.operations.get(request_id) if journal is not None else None
                    if held is not None:
                        existing, layer = held, 'session'
                except ValueError:
                    pass
            return dict(status='ok', request_id=arguments.get('request_id'), known=existing is not None, layer=layer,
                        episode_id=existing[1] if existing else None, pair_snapshot_id=existing[2] if existing else None)
        if command == 'origins':
            out = origins(self.main, arguments)
            # 2.2: a session's rows still in its proposal journal are listed after main's (the verifier sees both)
            journal = self._journal_of(arguments.get('session'))
            if journal is not None:
                more = origins(journal, dict(arguments, offset=0))
                for row in more['rows']:
                    row['layer'] = 'session'
                out['rows'].extend(more['rows']); out['count'] = len(out['rows']); out['total'] += more['total']
            return out
        if command == 'turns':
            out = turns(self.main, arguments)
            # 2.2: the session's own turns live in its proposal journal until it merges — after a compaction the
            # SessionStart hook asks for exactly those; main's rows (an earlier merge of the same session) join them
            journal = self._journal_of(arguments.get('session'))
            if journal is not None:
                more = turns(journal, arguments)
                for row in more['rows']:
                    row['layer'] = 'session'
                merged = sorted(out['rows'] + more['rows'], key=lambda r: (r['turn'], (r['lines'] or [0])[0]))
                limit = max(1, min(int(arguments.get('limit') or 3), 20))
                out.update(rows=merged[-limit:], count=min(limit, len(merged)), total=out['total'] + more['total'])
            return out
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
                session = arguments.pop('session', None)
                target = self.bundles.main_for(arguments.pop('bundle', None))     # G7: routed by bundle id
                _verify_producer(arguments)                                        # signed producers: verified True/False
                if self.sessions is not None and session and target is self.main:
                    # 2.2: an active session's observation goes to its proposal journal, not main
                    meta = arguments.get('metadata') or {}
                    return self.sessions.ingest(session, arguments, agent=meta.get('agent'), project=meta.get('project'))
                return target.ingest(arguments)
            if command == 'ingest_many':
                # batch generations (2026-09-18): K observations -> one generation; all-or-nothing
                session = arguments.get('session')
                target = self.bundles.main_for(arguments.get('bundle'))
                rows = list(arguments.get('rows') or [])
                for row in rows:
                    _verify_producer(row)
                if self.sessions is not None and session and target is self.main:
                    meta = (rows[0].get('metadata') or {}) if rows else {}
                    return self.sessions.ingest_many(session, rows, agent=meta.get('agent'), project=meta.get('project'))
                return target.ingest_many(rows)
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
            if self.merger is not None:
                self.merger.close()    # a running merge finishes its stage; the journal recovers the rest on start
            if self.sessions is not None:
                self.sessions.close()  # open proposal journals: checkpoint + close (unmerged — their sessions live on)
        except Exception:
            pass
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


PREPARE_EVERY = 5.0      # seconds between preparer passes over the warm bundles (a view is at most this stale)
MERGE_SWEEP_EVERY = 15.0  # 2.2: seconds between the preparer's looks for ended / idle sessions to merge


def _preparer(daemon):
    """G8: storage access, decoding and warm loads happen here, never inside a judgment. At start-up the primary's
    light cache is prefetched newest-first (the first judgments then run from RAM); afterwards every warm bundle
    is loaded once and refreshed when its journal moved or a judgment missed it. Receipts in ``daemon.prepared``."""
    started = time.perf_counter_ns()
    try:
        memory = daemon.main.memory
        done = memory.prefetch_light(budget_ns=20_000_000_000) if hasattr(memory, 'prefetch_light') else 0
        daemon.prepared.append(dict(kind='prefetch_light', rows=done, ms=(time.perf_counter_ns() - started) // 1_000_000, when=time.time()))
    except Exception as error:
        daemon.prepared.append(dict(kind='prefetch_light', error=type(error).__name__, when=time.time()))
    last_pass = last_merge = 0.0
    while not daemon.stop.is_set():
        if daemon.bundles.wanted or time.time() - last_pass >= PREPARE_EVERY:
            for receipt in daemon.bundles.prepare_all(force=False):
                receipt['kind'] = 'bundle'
                daemon.prepared.append(receipt)
                del daemon.prepared[:-64]
            last_pass = time.time()
        if daemon.merger is not None and time.time() - last_merge >= MERGE_SWEEP_EVERY:
            # 2.2: sessions that ended (SessionEnd) or fell idle merge into main from here, off every request path
            try:
                submitted = daemon.merger.submit_due()
                if submitted:
                    daemon.prepared.append(dict(kind='merge_due', sessions=[x[:12] for x in submitted], when=time.time()))
            except Exception as error:
                daemon.prepared.append(dict(kind='merge_due', error=type(error).__name__, when=time.time()))
            last_merge = time.time()
        time.sleep(0.25)


TAIL_SWEEP_EVERY = 60.0   # seconds between the daemon's sweeps of tailed / registered conversation logs


def _tail_sweeper(daemon, port, every):
    """Real time without a hook (2026-09-21, items 9·20): every ``every`` seconds the quiet logs that grew since
    their tail state — a session that died mid-turn, an agent without hooks whose log glob was registered with
    ``vrs2-tail.py --register`` — are tailed into the store through this daemon's own port, the path a hook
    takes. A client is opened only when a log has something to send, so an idle daemon still idles out. Only
    the daemon that owns the configured live store sweeps (a test daemon on a temporary store never touches
    this machine's tail states)."""
    try:
        from .harness import transcripts
        from .harness.paths import STATE as LIVE_STATE
        if Path(daemon.state_dir).resolve() != Path(LIVE_STATE).resolve():
            return
    except Exception:
        return
    while not daemon.stop.is_set():
        # a live agent registered with a short idle (Antigravity, --idle 15) is swept at that pace: the period is
        # the shortest registered idle, never below 5 s and never above ``every`` (2026-09-21 live test: 9 s
        # with a foreground --watch; the sweep alone must not be a minute behind)
        try:
            idles = [float(e.get('idle')) for e in transcripts.load_watch() if e.get('idle')]
            period = max(5.0, min([float(every)] + idles))
        except Exception:
            period = float(every)
        deadline = time.time() + period
        while time.time() < deadline:
            if daemon.stop.is_set():
                return
            time.sleep(1.0)
        try:
            transcripts.sweep_all(trigger='daemon', client_factory=lambda: LoopbackClient(port, 120))
        except Exception as error:
            try:
                transcripts.receipt(trigger='daemon:sweep', error=repr(error)[:160])
            except Exception:
                pass


def serve(state_dir, *, port=0, allow_ingest=False, idle_hours=8.0, bundle_limit=None, bundles=None, hot_bundles=1, tail_sweep=TAIL_SWEEP_EVERY):
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
    _clear_start_marker(daemon.state_dir)
    thread = threading.Thread(target=server.serve_forever, kwargs={'poll_interval': 0.5}, daemon=True)
    thread.start()
    preparer = threading.Thread(target=_preparer, args=(daemon,), name='vrs2-preparer', daemon=True)
    preparer.start()
    if tail_sweep and allow_ingest:
        sweeper = threading.Thread(target=_tail_sweeper, args=(daemon, actual, float(tail_sweep)), name='vrs2-tail-sweeper', daemon=True)
        sweeper.start()
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
    parser.add_argument('--tail-sweep', type=float, default=TAIL_SWEEP_EVERY, help='seconds between sweeps of tailed/registered conversation logs (0 = off)')
    options = parser.parse_args()
    bundles = dict(item.split('=', 1) for item in options.bundle if '=' in item)
    try:
        return serve(options.state_dir, port=options.port, allow_ingest=options.allow_ingest,
                     idle_hours=options.idle_hours, bundle_limit=options.bundle_limit or None,
                     bundles=bundles, hot_bundles=options.hot_bundles, tail_sweep=options.tail_sweep)
    except (ValueError, OSError) as error:
        print('VRS2 loopback daemon error: ' + str(error), file=sys.stderr)
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
