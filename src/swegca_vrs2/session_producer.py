# -*- coding: utf-8 -*-
"""Session producer layer — 2.2 (2026-09-21), composed after the SWEGCA architecture.

SWEGCA draws one boundary: the main process owns the single persistent Cognitive State; models, tools and
sessions are *producers* that work on a detached snapshot and return transient proposals, never a second
persistent state (ARCHITECTURE_SPEC §3.1, §4.1, §4.3). Here that boundary is a directory:

    <state>/sessions/<session id>/      the producer's proposal journal — one small store of its own
                                         (journal + hot index + VRS graph, so Déjà vu → Recall → Replay →
                                         Re-evidence run over it unchanged), written to by that session alone
    <state>/                             main: the Single-World state, read lock-free, written only by a merge

While a session is active every observation it produces (its conversation turns, the memory docs it re-indexes,
its verdicts) enters its proposal journal in real time and nothing enters main. A judgment (``hook_recall``) reads
the proposal journal first — the session's own experience, with the bounded exact top-K judgment — and falls to
main only on a **complete miss** (no cue of the query has a posting in the session index). After the session
ends (``session_end`` from the host's SessionEnd hook, or the sweeper finding it idle) the merge transaction
(``merge.py``) commits the whole journal into main as one batch generation with a receipt, and the directory
goes. A proposal journal is never authoritative: main never reads it, and a judgment says which layer answered.

Residency: open proposal journals are hot (their own ``Main``); beyond ``hot_limit`` the least recently used is
checkpointed and closed (it stays on disk, unmerged, until its session ends). Threads: the daemon's handler
threads serialize writes under the daemon lock; a merge runs in its own thread and takes the daemon lock only
around main's ``ingest_many``. Memory: a session of a thousand turns is a few MB.
"""
from __future__ import annotations

import json
import os
import re
import shutil
import threading
import time
from collections import OrderedDict
from pathlib import Path

SESSIONS_DIR = 'sessions'
ENDED_FILE = 'ended.json'          # written by session_end: the producer declared itself finished
STATE_FILE = 'session.json'        # last write, agent, project — for the sweeper and status
SESSION_HOT = 4                    # open proposal journals kept hot (LRU); each is a few MB
SESSION_IDLE_S = 1800.0            # a session with no write for this long is taken as ended (the sweeper merges it)
_SAFE = re.compile(r'[^A-Za-z0-9._-]')


def safe_id(session_id):
    """A session id as a directory name (Claude Code uuids and Antigravity conversation ids pass unchanged)."""
    text = _SAFE.sub('_', str(session_id or '').strip())[:96]
    if not text or text in ('.', '..'):
        raise ValueError('invalid_session_id')
    return text


class SessionLayer:
    def __init__(self, state_dir, *, hot_limit=SESSION_HOT, bundle_limit=None, idle_seconds=SESSION_IDLE_S):
        self.root = Path(state_dir) / SESSIONS_DIR
        self.root.mkdir(parents=True, exist_ok=True)
        self.hot_limit = max(1, int(hot_limit))
        self.bundle_limit = bundle_limit
        self.idle_seconds = float(idle_seconds)
        self.hot = OrderedDict()                          # session id -> Main (proposal journal), LRU order
        self.last_write = {}                              # session id -> time of the last ingest this process saw
        self.merging = set()                              # session ids a merge transaction holds right now
        self.lock = threading.Lock()

    # ── directories ──────────────────────────────────────────────────────────
    def path(self, session_id):
        return self.root / safe_id(session_id)

    def exists(self, session_id):
        try:
            return (self.path(session_id) / 'memory.sqlite3').is_file()
        except ValueError:
            return False

    def ids(self):
        """Every proposal journal on disk (open or closed, ended or not)."""
        try:
            return sorted(p.name for p in self.root.iterdir() if (p / 'memory.sqlite3').is_file())
        except OSError:
            return []

    def _state(self, session_id):
        try:
            return json.loads((self.path(session_id) / STATE_FILE).read_text(encoding='utf-8'))
        except (OSError, ValueError):
            return {}

    def _write_state(self, session_id, **fields):
        path = self.path(session_id) / STATE_FILE
        state = self._state(session_id)
        state.update(fields)
        tmp = path.with_suffix('.tmp')
        tmp.write_text(json.dumps(state, ensure_ascii=False), encoding='utf-8')
        os.replace(tmp, path)

    def ended(self, session_id):
        return (self.path(session_id) / ENDED_FILE).is_file()

    def written_at(self, session_id):
        """When the journal was last written: this process's own record, else the store file's mtime."""
        seen = self.last_write.get(safe_id(session_id))
        if seen:
            return seen
        state = self._state(session_id)
        if state.get('written'):
            return float(state['written'])
        try:
            return max(os.stat(self.path(session_id) / name).st_mtime
                       for name in ('memory.sqlite3', 'memory.sqlite3-wal') if (self.path(session_id) / name).exists())
        except (OSError, ValueError):
            return 0.0

    # ── the producer's store ─────────────────────────────────────────────────
    def main_for(self, session_id, *, create=True):
        """The session's proposal journal, hot (opened on first use; the LRU beyond ``hot_limit`` is checkpointed
        and closed). None when it does not exist and ``create`` is False."""
        from .store import Main
        sid = safe_id(session_id)
        with self.lock:
            main = self.hot.get(sid)
            if main is not None:
                self.hot.move_to_end(sid)
                return main
            if not create and not (self.path(sid) / 'memory.sqlite3').is_file():
                return None
            if sid in self.merging:
                raise ValueError('session_is_being_merged')
            main = Main(self.path(sid), allow_ingest=True, bundle_limit=self.bundle_limit, proposal=True)
            self.hot[sid] = main
            evicted = []
            while len(self.hot) > self.hot_limit:
                evicted.append(self.hot.popitem(last=False))
        for _, old in evicted:
            old.close()                                   # checkpoints when dirty; stays on disk, unmerged
        return main

    def ingest_many(self, session_id, rows, *, agent=None, project=None):
        """Rows enter the session's proposal journal — never main — as one generation."""
        main = self.main_for(session_id)
        out = main.ingest_many(rows)
        sid = safe_id(session_id)
        now = time.time()
        self.last_write[sid] = now
        self._write_state(sid, written=now, agent=agent or self._state(sid).get('agent'),
                          project=project or self._state(sid).get('project'), rows=int(main.memory.episode_count))
        out['layer'] = 'session'; out['session'] = sid
        return out

    def ingest(self, session_id, arguments, **kw):
        out = self.ingest_many(session_id, [arguments], **kw)
        result = out['results'][0]
        result['layer'] = 'session'; result['session'] = out['session']
        return result

    # ── judgment: the session first ──────────────────────────────────────────
    def recall(self, session_id, query, *, exclude_kinds=(), limit=10, region_scope='all'):
        """The bounded judgment over the session's proposal journal: ``None`` is a complete miss (no journal, or
        no cue of the query has a posting in it) — the caller then reads main. Otherwise main.recall's result with
        Replay and Re-evidence for the top ``limit`` candidates (every candidate listed, ``judged`` says K)."""
        main = self.main_for(session_id, create=False)
        if main is None or main.memory.episode_count == 0:
            return None
        root = main.recall(query, None, exclude_kinds=exclude_kinds, region_scope=region_scope, judge_limit=limit)
        if not root['receipt']['activation'].recall.candidates:
            return None
        return main, root

    def live_for(self, agent=None, project=None, now=None):
        """The session a hint resolves to (2.2, for producers whose read channel does not carry the session id — the
        MCP bridge of a hookless agent such as Antigravity): the most recently written journal of that agent (and
        project when given) that has not ended and was written inside the idle window. None when there is none."""
        now = time.time() if now is None else now
        best, best_when = None, 0.0
        for sid in self.ids():
            if self.ended(sid) or sid in self.merging:
                continue
            state = self._state(sid)
            if agent and state.get('agent') != agent:
                continue
            if project and state.get('project') != project:
                continue
            when = self.written_at(sid)
            if (now - when) < self.idle_seconds and when > best_when:
                best, best_when = sid, when
        return best

    # ── lifecycle ────────────────────────────────────────────────────────────
    def end(self, session_id, *, reason='session_end'):
        """The producer declared itself finished: mark it (the merge follows; a late write still lands in the
        journal and merges with it). True when a journal exists."""
        if not self.exists(session_id):
            return False
        sid = safe_id(session_id)
        (self.path(sid) / ENDED_FILE).write_text(json.dumps(dict(reason=reason, when=time.time())), encoding='utf-8')
        return True

    def close_session(self, session_id):
        """Checkpoint and close a hot journal (before a merge reads it, or on eviction)."""
        sid = safe_id(session_id)
        with self.lock:
            main = self.hot.pop(sid, None)
        if main is not None:
            main.close()
        return main is not None

    def due(self, now=None):
        """Sessions the merge should take now: ended, or idle longer than ``idle_seconds``; not already merging."""
        now = time.time() if now is None else now
        out = []
        for sid in self.ids():
            if sid in self.merging:
                continue
            if self.ended(sid) or (now - self.written_at(sid)) >= self.idle_seconds:
                out.append(sid)
        return out

    def take(self, session_id):
        """Reserve a session for one merge transaction (closed, hot no more). False when already taken."""
        sid = safe_id(session_id)
        with self.lock:
            if sid in self.merging:
                return False
            self.merging.add(sid)
        self.close_session(sid)
        return True

    def release(self, session_id):
        with self.lock:
            self.merging.discard(safe_id(session_id))

    def drop(self, session_id):
        """Remove a merged journal's directory (the rows live in main's journal now)."""
        sid = safe_id(session_id)
        self.close_session(sid)
        self.last_write.pop(sid, None)
        shutil.rmtree(self.path(sid), ignore_errors=True)
        return not self.path(sid).exists()

    def status(self):
        rows = []
        for sid in self.ids():
            main = self.hot.get(sid)
            state = self._state(sid)
            rows.append(dict(id=sid, state='merging' if sid in self.merging else 'hot' if main is not None else 'closed',
                             records=int(main.memory.episode_count) if main is not None else state.get('rows'),
                             ended=self.ended(sid), written=self.written_at(sid), agent=state.get('agent'), project=state.get('project')))
        return rows

    def close(self):
        with self.lock:
            hot = list(self.hot.items()); self.hot.clear()
        for _, main in hot:
            try:
                main.close()
            except Exception:
                pass
