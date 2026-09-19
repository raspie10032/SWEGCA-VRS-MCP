# -*- coding: utf-8 -*-
"""Resident layer — the minimal G7 (2026-09-19): one daemon, several bundles.

A bundle is one store (journal + checkpoint; docs/SIZING.md says ~60k records each). The daemon owns its
primary bundle as before — HOT: index and VRS graph resident, the engine's recall with regions and
promotion. Every other registered bundle is answered from a WARM view: the index alone (cues, postings,
recall columns, compressed records), loaded read-only from that bundle's own checkpoint (the warm blob a
checkpoint now writes beside the full one) plus the journal rows written after it, refreshed whenever the
bundle's journal moves. A warm bundle costs its index, not its graph. An ingest routed to another bundle
opens it HOT (owned, its own journal and consolidation); beyond ``hot_limit`` the least recently used hot
bundle is checkpointed and closed back to warm. Residency is a rank, not a strength: where a bundle sits
says nothing about its VRS, and a record is addressable in every state. The directory (record id -> bundle)
is main-owned and resident.

What a warm bundle cannot do: consolidate, promote, scope by region, judge re-evidence — its rows come
back in index order (BM25 over the informative cues, the primary's order without VRS) and say so
(``vrs=None``, ``bundle=<id>``). What it can: every record, every revision, every proposition of that
bundle is one request away without a second process.
"""
import math
import pickle
import sqlite3
import threading
import time
import zlib
from collections import OrderedDict
from pathlib import Path

from .compact_index import CompactIndex
from .store import CHECKPOINT_MAGIC
BM25_K1, BM25_B = 1.2, 0.3            # the primary's candidate order (store.recall), measured 2026-09-14


# ── warm view: a bundle's index without its graph ─────────────────────────────

def _connect(directory):
    return sqlite3.connect(str(Path(directory) / 'memory.sqlite3'), isolation_level=None,
                           check_same_thread=False, timeout=5)


def _table_exists(db, name):
    return db.execute("SELECT 1 FROM sqlite_master WHERE type='table' AND name=?", (name,)).fetchone() is not None


def load_warm_memory(db):
    """(memory, journal seq the memory covers). The warm blob when the checkpoint wrote one, else the memory
    half of the full checkpoint (the graph is unpickled and dropped — a one-time cost for old checkpoints);
    then the observation rows written after the checkpoint."""
    from .store import HotIndex, journal_entry
    identity = db.execute('SELECT value FROM identity WHERE id=1').fetchone() if _table_exists(db, 'identity') else None
    memory, seq = CompactIndex.empty(identity[0] if identity else 'warm'), 0
    head = db.execute('SELECT seq, pair FROM checkpoint WHERE id=1').fetchone() if _table_exists(db, 'checkpoint') else None
    if head is not None:
        blob = None
        if _table_exists(db, 'checkpoint_warm'):
            warm = db.execute('SELECT seq, pair, blob FROM checkpoint_warm WHERE id=1').fetchone()
            if warm is not None and warm[0] == head[0] and warm[1] == head[1]:
                blob = warm[2]
        if blob is not None:
            memory = pickle.loads(zlib.decompress(blob[2:]) if blob[:2] == CHECKPOINT_MAGIC else blob)
        else:
            full = db.execute('SELECT blob FROM checkpoint WHERE id=1').fetchone()[0]
            state = pickle.loads(zlib.decompress(full[2:]) if full[:2] == CHECKPOINT_MAGIC else full)
            memory = state['memory']
        if isinstance(memory, HotIndex):
            memory = CompactIndex.from_hot(memory)
        seq = int(head[0])
    memory, seq = replay_tail(db, memory, seq)
    return memory, seq


def replay_tail(db, memory, after):
    """Append the observation rows with seq > ``after`` (alias/usage/consolidation rows are graph-only)."""
    from .store import journal_entry
    if not _table_exists(db, 'observations'):
        return memory, after
    last = after
    for seq, req, body, fingerprint, _ in db.execute(
            'SELECT seq, request_id, body, fingerprint, pair FROM observations WHERE seq>? ORDER BY seq', (after,)):
        kind, row = journal_entry(req, body, fingerprint)
        if kind == 'observation':
            memory, _ = memory.append(row)
        last = int(seq)
    return memory, last


class WarmView:
    """Read-only index view of a bundle another owner may be writing: refreshed from its journal on demand."""

    def __init__(self, bundle_id, directory):
        self.id, self.directory = bundle_id, Path(directory)
        self.memory, self.seq, self.checkpoint_seq = None, 0, None
        self.loaded_at, self.load_ns = None, 0
        self.db = None
        self.lock = threading.Lock()

    def _db(self):
        if self.db is None:
            self.db = _connect(self.directory)
        return self.db

    def refresh(self):
        """Load once; afterwards follow the journal tail, or reload when the owner wrote a newer checkpoint
        (its warm blob is the cheaper base)."""
        with self.lock:
            db = self._db()
            head = db.execute('SELECT seq FROM checkpoint WHERE id=1').fetchone() if _table_exists(db, 'checkpoint') else None
            checkpoint_seq = int(head[0]) if head else 0
            started = time.perf_counter_ns()
            if self.memory is None or checkpoint_seq > self.seq:
                self.memory, self.seq = load_warm_memory(db)
                self.checkpoint_seq = checkpoint_seq
                self.loaded_at, self.load_ns = time.time(), time.perf_counter_ns() - started
            else:
                self.memory, self.seq = replay_tail(db, self.memory, self.seq)
            return self.memory

    def close(self):
        with self.lock:
            if self.db is not None:
                self.db.close()
                self.db = None
            self.memory = None

    def status(self):
        return dict(id=self.id, state='warm', records=self.memory.episode_count if self.memory is not None else None,
                    seq=self.seq, load_ms=self.load_ns // 1_000_000)


# ── index-only recall ────────────────────────────────────────────────────────

def warm_recall(memory, query, exclude_kinds=(), limit=10, snippet=400):
    """The engine's candidate set on this index (records carrying a matched cue) in the primary's order — BM25
    over the informative cues — without VRS strengths, regions or re-evidence. Returns the hook's row shape."""
    from .store import keys, asks_of, plain
    if exclude_kinds:
        memory = memory.masked(tuple(exclude_kinds))
    cues = keys(query)
    fanout = {c: len(memory.episode_ids_for_cue(c)) for c in cues}
    selected = tuple(c for c in cues if fanout[c])
    total = max(1, memory.episode_count)
    informative = tuple(c for c in selected if fanout[c] * 2 <= total)
    idf = {c: math.log(1.0 + (total - fanout[c] + 0.5) / (fanout[c] + 0.5)) for c in informative}
    store = memory._store
    cue_rows, ids = store['cues'], store['ids']
    average = (sum(len(a) for a in cue_rows) / max(1, len(cue_rows))) if len(cue_rows) else 1.0
    scores, matched = {}, {}
    for c in selected:                                   # every matched cue admits a candidate (nothing dropped) …
        weight = idf.get(c, 0.0)                         # … a function-word cue (half the store or more) weighs nothing
        for r in memory.rows_for_cue(c):
            r = int(r)
            n = len(cue_rows[r])
            scores[r] = scores.get(r, 0.0) + weight * (BM25_K1 + 1) / (1 + BM25_K1 * (1 - BM25_B + BM25_B * n / average))
            matched.setdefault(r, []).append(c)
    rows = []
    for r, score in sorted(scores.items(), key=lambda kv: (-kv[1], ids[kv[0]])):
        identifier = ids[r]
        if identifier in memory.superseded:
            continue
        episode = memory.episode_light(identifier)
        obs = episode.steps[0].observation
        text = obs.get('text', '')
        hit = matched[r]
        rows.append(dict(episode_id=identifier, source=episode.source_addresses[0], revision=episode.revision,
                         outcome=episode.steps[0].outcome, matched=len(hit), matched_cues=hit[:64],
                         cue_overlap=len(hit) / max(1, len(cue_rows[r]) + len(selected) - len(hit)),
                         proposition=obs.get('proposition_id'), polarity=obs.get('evidence_polarity'),
                         verdict=None, superseded_by=None, metadata=plain(obs.get('metadata') or {}),
                         text=text[:snippet], text_chars=len(text), asks=asks_of(text),
                         vrs=None, region=None, score=round(score, 4)))
        if len(rows) >= limit:
            break
    return dict(rows=rows, fanout={c: fanout[c] for c in selected}, record_count=total, candidate_count=len(scores))


# ── the resident: primary hot, others hot-by-use or warm ─────────────────────

class Resident:
    def __init__(self, primary, bundles=None, hot_limit=1, bundle_limit=None):
        self.primary = primary                            # the daemon's own Main (always hot)
        self.bundles = {str(k): Path(v) for k, v in (bundles or {}).items() if v}
        self.hot_limit = max(0, int(hot_limit))
        self.bundle_limit = bundle_limit
        self.hot = OrderedDict()                          # bundle id -> Main (owned), LRU order
        self.warm = {}                                    # bundle id -> WarmView
        self.closing = {}                                 # bundle id -> thread checkpointing + closing it
        self.lock = threading.Lock()

    def _close_later(self, bundle_id, main):
        """Checkpoint + close off the request thread (seconds at 5k records; the hook client times out at 5 s).
        The journal is the truth, so a warm view opened meanwhile reads the last checkpoint plus the tail."""
        def run():
            try:
                main.close()
            finally:
                with self.lock:
                    if self.closing.get(bundle_id) is thread:
                        del self.closing[bundle_id]
        thread = threading.Thread(target=run, name=f'vrs2-close-{bundle_id}', daemon=True)
        self.closing[bundle_id] = thread
        thread.start()
        return thread

    def settle(self):
        """Wait for every background close (tests, shutdown)."""
        for thread in list(self.closing.values()):
            thread.join()

    def _wait_closed(self, bundle_id):
        thread = self.closing.get(bundle_id)
        if thread is not None:
            thread.join()

    # bundles
    def ids(self):
        return list(self.bundles)

    def view(self, bundle_id):
        """The bundle's current index: the hot Main's memory, else the warm view (refreshed)."""
        with self.lock:                                   # the dicts only; a refresh (seconds) runs outside
            main = self.hot.get(bundle_id)
            if main is not None:
                return main.memory
            view = self.warm.get(bundle_id)
            if view is None:
                view = self.warm[bundle_id] = WarmView(bundle_id, self.bundles[bundle_id])
        return view.refresh()

    def main_for(self, bundle_id):
        """The Main that owns ``bundle_id`` — the primary for None/'main'; another bundle is opened hot (its warm
        view dropped), evicting the least recently used hot bundle beyond ``hot_limit`` (checkpoint + close)."""
        from .store import Main
        if bundle_id in (None, '', 'main'):
            return self.primary
        if bundle_id not in self.bundles:
            raise ValueError('unknown_bundle')
        with self.lock:
            main = self.hot.get(bundle_id)
            if main is not None:
                self.hot.move_to_end(bundle_id)
                return main
            view = self.warm.pop(bundle_id, None)
            if view is not None:
                view.close()
            self._wait_closed(bundle_id)                  # a previous eviction of this bundle still checkpointing
            main = Main(self.bundles[bundle_id], allow_ingest=True, bundle_limit=self.bundle_limit)
            self.hot[bundle_id] = main
            while len(self.hot) > self.hot_limit:
                evicted_id, evicted = self.hot.popitem(last=False)
                self._close_later(evicted_id, evicted)   # checkpoints when dirty (writes the warm blob too)
            return main

    def evict(self, bundle_id):
        """Hot -> closing (background checkpoint) -> warm on the next read. True when it was hot."""
        with self.lock:
            main = self.hot.pop(bundle_id, None)
            if main is not None:
                self._close_later(bundle_id, main)
                return True
            return False

    # recall across bundles
    def recall_others(self, query, exclude_kinds=(), limit=10, snippet=400):
        """Rows from every non-primary bundle, tagged with their bundle and its state."""
        out = []
        for bundle_id in self.bundles:
            try:
                memory = self.view(bundle_id)
            except Exception as error:                    # a bundle that cannot be read is reported, not fatal
                out.append(dict(bundle=bundle_id, error=type(error).__name__))
                continue
            state = 'hot' if bundle_id in self.hot else 'warm'
            result = warm_recall(memory, query, exclude_kinds, limit, snippet)
            for row in result['rows']:
                row.update(bundle=bundle_id, bundle_state=state, fanout={c: result['fanout'].get(c, 0) for c in row['matched_cues']},
                           record_count=result['record_count'])
            out.append(dict(bundle=bundle_id, state=state, records=memory.episode_count, candidate_count=result['candidate_count'],
                            rows=result['rows']))
        return out

    def lookup(self, episode_id):
        """Which bundle holds a record id (the primary first, then every other bundle's index)."""
        if episode_id in self.primary.memory.records:
            return 'main'
        for bundle_id in self.bundles:
            try:
                if episode_id in self.view(bundle_id).records:
                    return bundle_id
            except Exception:
                continue
        return None

    def status(self):
        rows = [dict(id='main', state='hot', **self.primary.bundle())]
        for bundle_id in self.bundles:
            main = self.hot.get(bundle_id)
            if main is not None:
                rows.append(dict(id=bundle_id, state='hot', **main.bundle()))
            elif bundle_id in self.closing:
                rows.append(dict(id=bundle_id, state='closing', records=None, seq=None))
            else:
                view = self.warm.get(bundle_id)
                rows.append(view.status() if view is not None and view.memory is not None
                            else dict(id=bundle_id, state='cold', records=None, seq=None))
        return rows

    def close(self):
        with self.lock:
            while self.hot:
                _, main = self.hot.popitem(last=False)
                main.close()
            for view in self.warm.values():
                view.close()
            self.warm.clear()
            pending = list(self.closing.values())
        for thread in pending:
            thread.join()
