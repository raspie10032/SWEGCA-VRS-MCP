# -*- coding: utf-8 -*-
"""One main owner serving complete immutable VRS generations from several shards.

Every shard read follows the same Déjà vu -> Recall -> Replay -> Re-evidence path.
The retired index-only warm view skipped VRS strength, regions, shared-experience
portal navigation and Re-evidence, so it was not a valid experience read.  A
closed shard is now reopened from its complete checkpoint as an immutable recall
generation.  Loading remains outside the judgment path; an unavailable shard is
reported as a named miss rather than answered by a weaker algorithm.
"""
import pickle
import sqlite3
import threading
import time
import zlib
from collections import OrderedDict
from pathlib import Path

from .store import CHECKPOINT_MAGIC


# ── immutable complete recall generation ─────────────────────────────────────

def _connect(directory):
    return sqlite3.connect(str(Path(directory) / 'memory.sqlite3'), isolation_level=None,
                           check_same_thread=False, timeout=5)


def _table_exists(db, name):
    return db.execute("SELECT 1 FROM sqlite_master WHERE type='table' AND name=?", (name,)).fetchone() is not None


class RecallGeneration:
    """Read-only Main-shaped owner for one verified checkpoint generation.

    ``Main.recall`` is deliberately reused rather than approximated.  It reads
    only the immutable generation tuple, so no database owner or write methods
    are needed here.
    """

    def __init__(self, state):
        from .engine.mosaic_memory_activation import FullCurrentMemoryVrsSnapshot
        from .store import Main
        memory, graph = state['memory'], state['graph']
        pair = FullCurrentMemoryVrsSnapshot(memory, graph.snapshot_id)
        if pair.snapshot_id != state['pair']:
            raise ValueError('checkpoint_generation_integrity_failed')
        self._generation = (memory, graph, pair, state.get('operations'))
        self.recall = Main.recall.__get__(self, RecallGeneration)
        self.consolidation_stale = Main.consolidation_stale.__get__(self, RecallGeneration)

    @property
    def memory(self):
        return self._generation[0]

    @property
    def graph(self):
        return self._generation[1]

    @property
    def pair(self):
        return self._generation[2]

    def _check(self):
        return None


def _journal_head(db):
    live = db.execute('SELECT seq, pair FROM observations ORDER BY seq DESC LIMIT 1').fetchone() \
        if _table_exists(db, 'observations') else None
    archived = db.execute('SELECT last_seq, last_pair FROM journal_archive ORDER BY segment DESC LIMIT 1').fetchone() \
        if _table_exists(db, 'journal_archive') else None
    rows = [row for row in (live, archived) if row is not None]
    return max(rows, key=lambda row: int(row[0])) if rows else None


def load_recall_generation(db):
    """Load a complete checkpoint only when it covers the journal head.

    Answering from an older checkpoint would silently omit experiences or VRS
    events.  The shard owner checkpoints before eviction, so a mismatch is an
    invariant failure and remains a named preparer miss until corrected.
    """
    if not _table_exists(db, 'checkpoint'):
        raise ValueError('complete_checkpoint_missing')
    head = db.execute('SELECT seq, pair, blob FROM checkpoint WHERE id=1').fetchone()
    journal = _journal_head(db)
    if head is None or journal is None:
        raise ValueError('complete_checkpoint_missing')
    seq, pair, blob = head
    if (int(seq), pair) != (int(journal[0]), journal[1]):
        raise ValueError('complete_checkpoint_not_at_journal_head')
    if blob[:2] == CHECKPOINT_MAGIC:
        blob = zlib.decompress(blob[2:])
    state = pickle.loads(blob)
    identity = db.execute('SELECT value FROM identity WHERE id=1').fetchone()
    if state.get('identity') != (identity[0] if identity else None) or state.get('pair') != pair:
        raise ValueError('checkpoint_identity_mismatch')
    return RecallGeneration(state), int(seq)


class WarmView:
    """Read-only complete VRS view loaded by the preparer, never by a judgment."""

    def __init__(self, bundle_id, directory):
        self.id, self.directory = bundle_id, Path(directory)
        self.generation, self.seq, self.checkpoint_seq = None, 0, None
        self.loaded_at, self.load_ns, self.refreshed_at = None, 0, None
        self.db = None
        self.lock = threading.Lock()

    def _db(self):
        if self.db is None:
            self.db = _connect(self.directory)
        return self.db

    def moved(self):
        """True when the bundle's journal or checkpoint is past what this view covers (one cheap query)."""
        with self.lock:
            if self.generation is None:
                return True
            db = self._db()
            head = db.execute('SELECT MAX(seq) FROM observations').fetchone() if _table_exists(db, 'observations') else None
            return bool(head and head[0] and int(head[0]) > self.seq)

    def refresh(self):
        """Load or replace one complete checkpoint outside the judgment path."""
        with self.lock:
            db = self._db()
            head = db.execute('SELECT seq FROM checkpoint WHERE id=1').fetchone() if _table_exists(db, 'checkpoint') else None
            checkpoint_seq = int(head[0]) if head else 0
            journal = _journal_head(db)
            journal_seq = int(journal[0]) if journal else 0
            started = time.perf_counter_ns()
            if journal_seq > checkpoint_seq:
                raise ValueError('complete_checkpoint_not_at_journal_head')
            if self.generation is None or checkpoint_seq > self.seq:
                self.generation, self.seq = load_recall_generation(db)
                self.checkpoint_seq = checkpoint_seq
                self.loaded_at, self.load_ns = time.time(), time.perf_counter_ns() - started
            self.refreshed_at = time.time()
            return self.generation

    @property
    def memory(self):
        return None if self.generation is None else self.generation.memory

    def close(self):
        with self.lock:
            if self.db is not None:
                self.db.close()
                self.db = None
            self.generation = None

    def status(self):
        return dict(id=self.id, state='warm', records=self.memory.episode_count if self.memory is not None else None,
                    seq=self.seq, load_ms=self.load_ns // 1_000_000,
                    age_s=None if self.refreshed_at is None else round(time.time() - self.refreshed_at, 1))


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
        self.wanted = set()                               # bundles a judgment missed: the preparer loads them next
        self.prepared = {}                                # bundle id -> receipt of the last prepare (ms, seq, when)
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

    def ready(self, bundle_id):
        """(owner, state) pinned for one judgment without any loading: the hot Main, or a complete warm view that
        the preparer has loaded; ``(None, 'preparing')`` is a miss the packet names (G8)."""
        with self.lock:
            main = self.hot.get(bundle_id)
            if main is not None:
                return main, 'hot'
            if bundle_id in self.closing:
                self.wanted.add(bundle_id)
                return None, 'closing'
            view = self.warm.get(bundle_id)
            if view is not None and view.generation is not None:
                return view.generation, 'warm'
            self.wanted.add(bundle_id)
            return None, 'preparing'

    def prepare(self, bundle_id, force=False):
        """Load or refresh one warm bundle off the request path (the preparer's call). Returns a receipt or None
        when there was nothing to do."""
        with self.lock:
            if bundle_id in self.hot or bundle_id in self.closing:
                self.wanted.discard(bundle_id)
                return None
            view = self.warm.get(bundle_id)
            if view is None:
                view = self.warm[bundle_id] = WarmView(bundle_id, self.bundles[bundle_id])
            wanted = bundle_id in self.wanted
        if not (force or wanted or view.moved()):
            return None
        started = time.perf_counter_ns()
        generation = view.refresh()
        receipt = dict(bundle=bundle_id, ms=(time.perf_counter_ns() - started) // 1_000_000, seq=view.seq,
                       records=generation.memory.episode_count, pair_snapshot_id=generation.pair.snapshot_id,
                       complete_vrs=True, when=time.time())
        with self.lock:
            self.wanted.discard(bundle_id)
            self.prepared[bundle_id] = receipt
        return receipt

    def prepare_all(self, force=False):
        """Prepare requested shards only; ``force`` explicitly prepares every shard.

        Complete VRS generations are larger than the retired index-only views, so
        the periodic preparer must not load every registered shard merely because
        time passed.
        """
        with self.lock:
            ids = list(self.bundles) if force else list(self.wanted)
        return [receipt for bundle_id in ids for receipt in [self._safe_prepare(bundle_id, force)] if receipt]

    def _safe_prepare(self, bundle_id, force):
        try:
            return self.prepare(bundle_id, force)
        except Exception as error:                        # a bundle that cannot be read is reported, not fatal
            return dict(bundle=bundle_id, error=type(error).__name__, when=time.time())

    def view(self, bundle_id):
        """The bundle's current complete generation. Loads inside the
        caller — for tools and tests; a judgment uses ``ready`` and lets the preparer load (G8)."""
        with self.lock:                                   # the dicts only; a refresh (seconds) runs outside
            main = self.hot.get(bundle_id)
            if main is not None:
                return main
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
            main = Main(self.bundles[bundle_id], allow_ingest=True, bundle_limit=self.bundle_limit,
                        defer_checkpoints=True)
            self.hot[bundle_id] = main
            while len(self.hot) > self.hot_limit:
                evicted_id, evicted = self.hot.popitem(last=False)
                self._close_later(evicted_id, evicted)   # checkpoints the complete immutable generation
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
        """Full four-stage reads from every ready non-primary shard.

        Rows are tagged with their shard and state; a shard that is not ready is
        a named miss.  The exact primary hook renderer is reused so a warm shard
        retains VRS, regions, portals, uncertainty, contradictions and
        Re-evidence rather than falling back to lexical rows.
        """
        out = []
        for bundle_id in self.bundles:
            owner, state = self.ready(bundle_id)
            if owner is None:
                out.append(dict(bundle=bundle_id, state=state, miss=True, rows=[]))
                continue
            from .loopback import hook_recall
            packet = hook_recall(owner, dict(query=query, exclude_kinds=exclude_kinds,
                                              limit=limit, snippet=snippet), None)
            for row in packet['memories']:
                row.update(bundle=bundle_id, bundle_state=state,
                           record_count=packet['record_count'])
            view = self.warm.get(bundle_id)
            out.append(dict(bundle=bundle_id, state=state, records=owner.memory.episode_count,
                            candidate_count=packet['candidate_count'], pair_snapshot_id=packet['pair_snapshot_id'],
                            stage_order=['deja_vu', 'recall', 'replay', 're_evidence'], complete_vrs=True,
                            should_abstain=packet['should_abstain'], conflicts=packet['conflicts'],
                            seq=None if view is None else view.seq,
                            age_s=None if view is None or view.refreshed_at is None else round(time.time() - view.refreshed_at, 1),
                            rows=packet['memories']))
        return out

    def lookup(self, episode_id):
        """Which bundle holds a record id (the primary first, then every other bundle's index)."""
        if episode_id in self.primary.memory.records:
            return 'main'
        for bundle_id in self.bundles:
            try:
                if episode_id in self.view(bundle_id).memory.records:
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
                rows.append(view.status() if view is not None and view.generation is not None
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
