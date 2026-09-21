# -*- coding: utf-8 -*-
"""Merge transaction — a session's proposal journal committed into main (2.2, 2026-09-21).

The SWEGCA bounded commit (ARCHITECTURE_SPEC §4.7) and the journaled promotion protocol (§4.8), applied to the
one mutation this layer makes to the Single-World state: the rows a producer (a session) proposed enter main's
journal as **one batch generation**, and a receipt binds

    before_pair / after_pair   the pair snapshot ids of main before and after (the state hashes)
    delta_digest               sha256 over the proposed rows' fingerprints in journal order (the applied delta)
    evidence_refs              the rows' request ids and source addresses
    producer                   the session id, its agent and project

The stages live in main's SQLite (``merge_journal``) so a daemon that dies mid-merge recovers on start:

    prepared            the journal was read and its digest fixed; nothing in main changed yet
    memory_committed    main's ingest_many transaction committed (all rows, or none)
    state_committed     every proposed row verified present in main under its (possibly re-issued) request id
    completed           the proposal journal's directory removed
    rolled_back         a prepared transaction whose journal vanished; main untouched

Arbitration (§4.6) at the slot level: a request id another producer already committed with other content is a
held slot — this proposal's row is re-issued under its own id (superseding the held row when it is a later
revision of the same source, as the transcript adapter does for re-cut turns) and the receipt lists it; the
store keeps both. A ``supersedes`` naming a record main does not have (and the batch does not produce) is
unlinked and listed — the successor rule cannot bind to nothing. Propositions that end up with support and
refutation from different producers are listed as ``conflicts``: the accumulator's decision there is the
architecture's abstention, not a truth. Nothing is averaged, nothing is dropped: status-unfiltered experience.
"""
from __future__ import annotations

import hashlib
import json
import sqlite3
import threading
import time
import uuid
import zlib
from pathlib import Path

from .compact_index import _digest
from .store import journal_entry, observation, ARCHIVE_MAGIC

INCOMPLETE = ('prepared', 'memory_committed', 'state_committed')


def _ensure_journal(db):
    db.execute('CREATE TABLE IF NOT EXISTS merge_journal (transaction_id TEXT PRIMARY KEY, session TEXT NOT NULL, '
               'stage TEXT NOT NULL, rows INTEGER NOT NULL, delta_digest TEXT NOT NULL, before_pair TEXT, after_pair TEXT, '
               'started REAL NOT NULL, updated REAL NOT NULL, receipt TEXT)')


def read_journal(directory):
    """The observation rows of a proposal journal in order, read straight from its SQLite (no owner, no index):
    (request_id, row, fingerprint, pair). Archive segments first, then live rows."""
    path = Path(directory) / 'memory.sqlite3'
    if not path.is_file():
        return []
    db = sqlite3.connect(f"file:{path.as_posix()}?mode=ro", uri=True, timeout=5)
    out = []
    try:
        names = {r[0] for r in db.execute("SELECT name FROM sqlite_master WHERE type='table'")}
        if 'journal_archive' in names:
            for blob, in db.execute('SELECT blob FROM journal_archive ORDER BY segment'):
                if blob[:2] != ARCHIVE_MAGIC:
                    raise ValueError('journal_archive_segment_corrupt')
                for line in zlib.decompress(blob[2:]).decode('utf-8').splitlines():
                    _, req, body, fingerprint, pair = json.loads(line)
                    kind, row = journal_entry(req, body, fingerprint)
                    if kind == 'observation':
                        out.append((req, row, fingerprint, pair))
        if 'observations' in names:
            for req, body, fingerprint, pair in db.execute('SELECT request_id, body, fingerprint, pair FROM observations ORDER BY seq'):
                kind, row = journal_entry(req, body, fingerprint)
                if kind == 'observation':
                    out.append((req, row, fingerprint, pair))
    finally:
        db.close()
    return out


def delta_digest(rows):
    return hashlib.sha256(json.dumps([f for _, _, f, _ in rows]).encode('ascii')).hexdigest()


def episode_id_of(arguments):
    """The record id main will give these arguments (content-derived: the same row gets the same id everywhere)."""
    row = observation(arguments)
    return 'memory:' + _digest({k: v for k, v in row.items() if k != 'request_id'})


class MergeTransaction:
    """One session's merge. ``lock`` is the daemon's lock (taken only around main's ingest)."""

    def __init__(self, main, sessions, lock=None):
        self.main, self.sessions = main, sessions
        self.lock = lock or threading.Lock()
        _ensure_journal(main.db)

    # ── journal ──────────────────────────────────────────────────────────────
    def _stage(self, transaction_id, expected, stage, **fields):
        sets = ['stage=?', 'updated=?'] + [f'{k}=?' for k in fields]
        params = [stage, time.time(), *fields.values(), transaction_id, expected]
        cur = self.main.db.execute(f'UPDATE merge_journal SET {", ".join(sets)} WHERE transaction_id=? AND stage=?', params)
        if cur.rowcount != 1:
            raise ValueError('merge_journal_stage_changed_concurrently')

    def incomplete(self):
        return [dict(transaction_id=t, session=s, stage=st, rows=r, delta_digest=d, before_pair=b)
                for t, s, st, r, d, b in self.main.db.execute(
                    'SELECT transaction_id, session, stage, rows, delta_digest, before_pair FROM merge_journal '
                    'WHERE stage IN (?,?,?) ORDER BY started', INCOMPLETE)]

    def receipts(self, limit=20):
        return [dict(transaction_id=t, session=s, stage=st, rows=r, receipt=json.loads(rc) if rc else None)
                for t, s, st, r, rc in self.main.db.execute(
                    'SELECT transaction_id, session, stage, rows, receipt FROM merge_journal ORDER BY started DESC LIMIT ?', (int(limit),))]

    # ── the transaction ──────────────────────────────────────────────────────
    def merge(self, session_id, *, transaction_id=None):
        """prepared → memory_committed → state_committed → completed, with the receipt. Idempotent: rows already in
        main are replays; a session with no rows completes with an empty delta."""
        sid = session_id
        if not self.sessions.take(sid):
            return dict(status='busy', session=sid)
        started = time.time()
        try:
            directory = self.sessions.path(sid)
            rows = read_journal(directory)
            state = self.sessions._state(sid)
            tid = transaction_id or uuid.uuid4().hex
            before = self.main.pair.snapshot_id
            if transaction_id is None:
                for other in self.incomplete():
                    if other['session'] == sid:
                        raise ValueError('incomplete_merge_requires_recovery')
                self.main.db.execute('INSERT INTO merge_journal (transaction_id, session, stage, rows, delta_digest, before_pair, started, updated) '
                                     'VALUES (?,?,?,?,?,?,?,?)', (tid, sid, 'prepared', len(rows), delta_digest(rows), before, started, started))
            receipt = dict(transaction_id=tid, session=sid, producer=dict(session=sid, agent=state.get('agent'), project=state.get('project')),
                           rows=len(rows), delta_digest=delta_digest(rows), before_pair=before, reissued=[], unlinked_supersedes=[],
                           conflicts=[], replayed=0, added=0, generations_in_proposal=len({p for *_, p in rows}))
            # commit: one generation for the whole proposal
            batch, own_ids = [], set()
            for req, row, fingerprint, _ in rows:
                args = dict(row)
                held = self.main.operations.get(req)
                if held is not None and held[0] == fingerprint:
                    batch.append(args)                      # main already holds this very row: an idempotent replay
                    continue
                previous = args.get('supersedes')
                if previous is not None and previous not in self.main.memory.records and previous not in own_ids:
                    receipt['unlinked_supersedes'].append(dict(request_id=req, supersedes=previous))
                    args['supersedes'] = None
                if held is not None:
                    args = self._reissue(args, held, receipt)
                own_ids.add(episode_id_of(args))
                batch.append(args)
            after = before
            if batch:
                with self.lock:
                    out = self.main.ingest_many(batch)
                after = out['pair_snapshot_id']
                receipt['replayed'] = sum(1 for r in out['results'] if r.get('idempotent_replay'))
                receipt['added'] = out['added']
                receipt['journaled'] = out['journaled']
            self._stage(tid, 'prepared', 'memory_committed', after_pair=after)
            receipt['after_pair'] = after
            # verify: every proposed row is in main under the id it went in with
            missing = [a['request_id'] for a in batch if self.main.operations.get(a['request_id']) is None]
            if missing:
                raise ValueError(f'merge_verification_failed:{missing[:3]}')
            receipt['conflicts'] = self._conflicts(batch)
            receipt['evidence_refs'] = [dict(request_id=a['request_id'], source=a['source'], revision=a['revision']) for a in batch][:2000]
            self._stage(tid, 'memory_committed', 'state_committed')
            dropped = self.sessions.drop(sid)
            receipt['dropped'] = dropped
            receipt['ms'] = int((time.time() - started) * 1000)
            self._stage(tid, 'state_committed', 'completed', receipt=json.dumps(receipt, ensure_ascii=False))
            receipt['status'] = 'completed'
            return receipt
        finally:
            self.sessions.release(sid)

    def _reissue(self, args, held, receipt):
        """The slot (request id) is held by another producer's commit with other content: this row goes in under
        its own id, superseding the held record when it is a later revision of the same source."""
        tag = hashlib.sha256(args['text'].encode('utf-8')).hexdigest()[:8]
        out = dict(args)
        out['request_id'] = f"{args['request_id']}+{tag}"[:128]
        out['revision'] = f"{args['revision']}+{tag}"[:128]
        held_id = held[1]
        try:
            old = self.main.memory.episode(held_id) if held_id else None
        except KeyError:
            old = None
        if old is not None and old.source_addresses == (args['source'],) and old.revision != out['revision'] and held_id not in self.main.memory.superseded:
            out['supersedes'] = held_id
        elif out.get('supersedes') is not None and out['supersedes'] not in self.main.memory.records:
            out['supersedes'] = None
        receipt['reissued'].append(dict(request_id=args['request_id'], as_request_id=out['request_id'], supersedes=out.get('supersedes'), held=held_id))
        return out

    def _conflicts(self, batch):
        """Propositions the merged rows touch that now carry both polarities from different producers."""
        memory = self.main.memory
        out = []
        for p in sorted({a['proposition'] for a in batch if a.get('proposition')}):
            polarities = {}
            for eid in memory.propositions.get(p, ()):
                if eid in memory.superseded:
                    continue
                ep = memory.episode_light(eid) if hasattr(memory, 'episode_light') else memory.episode(eid)
                obs = ep.steps[0].observation
                meta = obs.get('metadata') or {}
                polarities.setdefault(obs.get('evidence_polarity'), set()).add(str(meta.get('session') or meta.get('producer') or ep.source_addresses[0]))
            if polarities.get('support') and polarities.get('refute'):
                out.append(dict(proposition=p, support=sorted(polarities['support'])[:8], refute=sorted(polarities['refute'])[:8],
                                producers=len(polarities['support'] | polarities['refute']), decision='abstain'))
        return out

    # ── recovery ─────────────────────────────────────────────────────────────
    def recover(self):
        """Daemon start: finish or roll back every incomplete transaction. ``prepared`` with its journal still on
        disk re-runs the merge under the same transaction id (idempotent replays); ``memory_committed`` /
        ``state_committed`` verify and complete; a journal that vanished is ``rolled_back`` (main untouched)."""
        out = []
        for tx in self.incomplete():
            sid, tid, stage = tx['session'], tx['transaction_id'], tx['stage']
            if not self.sessions.exists(sid):
                if stage == 'prepared':
                    self._stage(tid, stage, 'rolled_back', receipt=json.dumps(dict(reason='proposal_journal_missing')))
                    out.append(dict(transaction_id=tid, session=sid, stage='rolled_back'))
                else:
                    self._stage(tid, stage, 'completed', receipt=json.dumps(dict(reason='recovered_without_journal', from_stage=stage)))
                    out.append(dict(transaction_id=tid, session=sid, stage='completed', recovered_from=stage))
                continue
            if stage == 'prepared':
                receipt = self.merge(sid, transaction_id=tid)
            else:
                # main holds the rows (its transaction committed); verify against the journal and finish
                rows = read_journal(self.sessions.path(sid))
                unknown = [req for req, *_ in rows if self.main.operations.get(req) is None
                           and not any(k.startswith(req + '+') for k in self._reissued_ids(req))]
                if unknown:
                    self._stage(tid, stage, 'prepared')     # not all there: run it again as a prepared merge
                    receipt = self.merge(sid, transaction_id=tid)
                else:
                    if stage == 'memory_committed':
                        self._stage(tid, 'memory_committed', 'state_committed')
                    self.sessions.drop(sid)
                    self._stage(tid, 'state_committed', 'completed', receipt=json.dumps(dict(reason='recovered', from_stage=stage)))
                    receipt = dict(transaction_id=tid, session=sid, stage='completed', recovered_from=stage)
            out.append(receipt)
        return out

    def _reissued_ids(self, request_id):
        return [k for k in self.main.operations if isinstance(k, str) and k.startswith(request_id + '+')]


class Merger:
    """Runs merges off the request path: one worker at a time, queued by session id."""

    def __init__(self, transaction):
        self.transaction = transaction
        self.queue, self.running, self.done = [], None, []
        self.lock = threading.Lock()
        self.wake = threading.Event()
        self.stop = threading.Event()
        self.thread = threading.Thread(target=self._run, name='vrs2-merger', daemon=True)
        self.thread.start()

    def submit(self, session_id):
        with self.lock:
            if session_id in self.queue or self.running == session_id:
                return False
            self.queue.append(session_id)
        self.wake.set()
        return True

    def submit_due(self):
        return [sid for sid in self.transaction.sessions.due() if self.submit(sid)]

    def _run(self):
        while not self.stop.is_set():
            self.wake.wait(timeout=1.0)
            self.wake.clear()
            while True:
                with self.lock:
                    if not self.queue:
                        self.running = None
                        break
                    self.running = sid = self.queue.pop(0)
                try:
                    receipt = self.transaction.merge(sid)
                except Exception as error:                      # the transaction stays in its stage for recovery
                    receipt = dict(session=sid, status='error', error=f'{type(error).__name__}: {error}'[:200])
                    try:
                        self.transaction.sessions.release(sid)
                    except Exception:
                        pass
                with self.lock:
                    self.done.append(receipt); self.done = self.done[-32:]

    def wait_idle(self, timeout=60.0):
        """Tests and shutdown: until the queue is drained and nothing runs."""
        deadline = time.time() + timeout
        while time.time() < deadline:
            with self.lock:
                if not self.queue and self.running is None:
                    return True
            time.sleep(0.02)
        return False

    def status(self):
        with self.lock:
            return dict(queue=list(self.queue), running=self.running, done=list(self.done[-8:]))

    def close(self):
        self.stop.set(); self.wake.set()
        self.thread.join(timeout=5)
