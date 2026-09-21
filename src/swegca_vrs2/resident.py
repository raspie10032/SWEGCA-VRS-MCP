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
import os
import json
import sqlite3
import threading
import time
import zlib
from collections import OrderedDict
from pathlib import Path

from .store import CHECKPOINT_MAGIC, digest, plain
from .exact_replay import ExactReplayStore
from .cue_shards import CueShardDirectory
from .read_projection import ReadProjectionStore, projected_portals

AUTO_SHARD_RECORDS = 8_192
MAX_STORAGE_BYTES = 500 * 1024 ** 3
MAX_RSS_BYTES = 4 * 1024 ** 3
STORAGE_SCAN_TTL_SECONDS = 1.0
AUTO_SHARD_PREFIX = 'shard-'


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
        self.bundle_limit = min(int(bundle_limit or AUTO_SHARD_RECORDS), AUTO_SHARD_RECORDS)
        self.primary.bundle_limit = self.bundle_limit
        self.auto_root = self.primary.directory / 'shards'
        self.auto_root.mkdir(mode=0o700, parents=True, exist_ok=True)
        discovered = sorted(path for path in self.auto_root.glob(AUTO_SHARD_PREFIX + '*')
                            if (path / 'memory.sqlite3').is_file())
        for path in discovered:
            self.bundles.setdefault(path.name, path)
        self.auto_ids = sorted(identifier for identifier in self.bundles
                               if identifier.startswith(AUTO_SHARD_PREFIX)
                               and self.bundles[identifier].parent == self.auto_root)
        self.exact = ExactReplayStore(self.primary.directory / 'exact-replay')
        self.cue_shards = CueShardDirectory(self.exact.directory / 'cue-shards')
        self.exact_progress_path = self.exact.directory / 'backfill.json'
        self.exact_progress_lock = threading.Lock()
        self.exact_backfill = self._load_exact_progress()
        self.exact_backfill.setdefault('main', dict(count=0, tail=None,
            cue_total=0, complete=(self.primary.memory.episode_count == 0)))
        self.pair_ids = {'main': self.primary.pair.snapshot_id}
        self.record_counts = {'main': self.primary.memory.episode_count}
        self.cue_totals = {'main': self.primary.memory.cue_total}
        for identifier, directory in self.bundles.items():
            try:
                db = _connect(directory)
                row = db.execute('SELECT pair FROM checkpoint WHERE id=1').fetchone() \
                    if _table_exists(db, 'checkpoint') else None
                db.close()
                if row is not None:
                    self.pair_ids[identifier] = row[0]
                progress = self.exact_backfill.get(identifier, {})
                if progress.get('complete'):
                    self.record_counts[identifier] = int(progress.get('count', 0))
                    self.cue_totals[identifier] = int(progress.get('cue_total', 0))
            except sqlite3.Error:
                continue
        self._pair_lock = threading.Lock()
        self._pair_xor = bytearray(32)
        for identifier, pair in self.pair_ids.items():
            self._xor_pair_component(identifier, pair)
        self._logical_snapshot_id = self._snapshot_from_pair_xor()
        self._storage_lock = threading.Lock()
        self._storage_cache = (0.0, 0)
        self.hot = OrderedDict()                          # bundle id -> Main (owned), LRU order
        self.warm = {}                                    # bundle id -> WarmView
        self.closing = {}                                 # bundle id -> thread checkpointing + closing it
        self.wanted = set()                               # bundles a judgment missed: the preparer loads them next
        self.prepared = {}                                # bundle id -> receipt of the last prepare (ms, seq, when)
        self.projection_errors = {}
        self.projection_views = OrderedDict()
        self.projection_lock = threading.Lock()
        self.lock = threading.Lock()

    def _load_exact_progress(self):
        try:
            body = json.loads(self.exact_progress_path.read_text(encoding='utf-8'))
            if body.get('schema') != 'swegca-vrs2-read-index-backfill-v2':
                return {}
            progress = {}
            for shard, row in body.get('shards', {}).items():
                count = max(0, int(row.get('count', 0)))
                tail = row.get('tail')
                progress[str(shard)] = dict(count=count,
                    tail=tail if isinstance(tail, str) else None,
                    cue_total=max(0, int(row.get('cue_total', 0))),
                    complete=bool(row.get('complete', False)))
            return progress
        except (OSError, ValueError, TypeError, json.JSONDecodeError):
            return {}

    def _save_exact_progress(self):
        body = dict(schema='swegca-vrs2-read-index-backfill-v2', shards=self.exact_backfill)
        temporary = self.exact_progress_path.with_suffix('.tmp')
        temporary.write_text(json.dumps(body, ensure_ascii=False, sort_keys=True,
                                        separators=(',', ':')), encoding='utf-8')
        os.replace(temporary, self.exact_progress_path)

    def _exact_cursor(self, shard, owner):
        row = self.exact_backfill.get(shard, {})
        count = max(0, int(row.get('count', 0)))
        ids = owner.memory._store['ids']
        if count > owner.memory.episode_count:
            return 0
        if count and (count > len(ids) or ids[count - 1] != row.get('tail')):
            return 0
        return count

    def _advance_exact_cursor(self, shard, owner, count):
        count = max(0, min(int(count), owner.memory.episode_count))
        ids = owner.memory._store['ids']
        with self.exact_progress_lock:
            self.exact_backfill[shard] = dict(
                count=count, tail=(ids[count - 1] if count else None),
                cue_total=owner.memory.cue_total,
                complete=(count >= owner.memory.episode_count))
            self._save_exact_progress()

    def _storage_bytes(self, *, force=False):
        """Logical bytes with a short cache so ingest does not walk the tree.

        At the declared 5 Gbps device ceiling, the one-second cache window can
        hide at most 625 MB of growth.  Admission forces a fresh scan inside
        that remaining margin, so the 500 GB ceiling stays fail-closed.
        """
        now = time.monotonic()
        with self._storage_lock:
            stamp, cached = self._storage_cache
            if not force and stamp and now - stamp < STORAGE_SCAN_TTL_SECONDS:
                return cached
            total = 0
            for root, _, files in os.walk(self.primary.directory):
                for name in files:
                    try:
                        total += os.stat(Path(root) / name).st_size
                    except OSError:
                        continue
            self._storage_cache = (now, total)
            return total

    @staticmethod
    def _rss_bytes():
        try:
            pages = int(Path('/proc/self/statm').read_text(encoding='ascii').split()[1])
            return pages * os.sysconf('SC_PAGE_SIZE')
        except (OSError, ValueError, IndexError):
            return None

    def budget(self, *, force_storage=False):
        rss = self._rss_bytes()
        storage = self._storage_bytes(force=force_storage)
        return dict(rss_bytes=rss, rss_limit_bytes=MAX_RSS_BYTES,
                    rss_within_limit=(rss is None or rss <= MAX_RSS_BYTES),
                    storage_bytes=storage, storage_limit_bytes=MAX_STORAGE_BYTES,
                    storage_within_limit=storage <= MAX_STORAGE_BYTES,
                    exact_replay_logical_bytes=self.exact.logical_bytes(),
                    exact_replay_allocated_bytes=self.exact.allocated_bytes())

    def _admit_resources(self):
        rss = self._rss_bytes()
        if rss is not None and rss > MAX_RSS_BYTES:
            raise ValueError('vrs_memory_budget_exceeded')
        storage = self._storage_bytes()
        if storage > MAX_STORAGE_BYTES:
            raise ValueError('vrs_storage_budget_exceeded')
        # One cached second at the declared 5 Gbps ceiling is 625 MB.  Near
        # the boundary, replace the cached value before admitting a write.
        if storage >= MAX_STORAGE_BYTES - 625_000_000:
            if self._storage_bytes(force=True) >= MAX_STORAGE_BYTES:
                raise ValueError('vrs_storage_budget_exceeded')

    def admit_read_resources(self):
        """Fail closed before a read can grow this process beyond its RAM cap."""
        rss = self._rss_bytes()
        if rss is not None and rss > MAX_RSS_BYTES - 64 * 1024 * 1024:
            raise ValueError('vrs_memory_budget_exceeded')

    def _new_auto_shard(self):
        if self._storage_bytes(force=True) >= MAX_STORAGE_BYTES:
            raise ValueError('vrs_storage_budget_exceeded')
        number = max((int(identifier[len(AUTO_SHARD_PREFIX):]) for identifier in self.auto_ids),
                     default=0) + 1
        identifier = f'{AUTO_SHARD_PREFIX}{number:06d}'
        directory = self.auto_root / identifier
        directory.mkdir(mode=0o700, parents=True, exist_ok=False)
        self.bundles[identifier] = directory
        self.auto_ids.append(identifier)
        self.exact_backfill[identifier] = dict(count=0, tail=None, cue_total=0, complete=True)
        self.record_counts[identifier] = 0
        self.cue_totals[identifier] = 0
        return identifier

    def _xor_pair_component(self, shard, pair):
        component = bytes.fromhex(digest((str(shard), str(pair))))
        for index, value in enumerate(component):
            self._pair_xor[index] ^= value

    def _snapshot_from_pair_xor(self):
        return digest(('complete-vrs-sharded-generation-v1',
                       len(self.pair_ids), self._pair_xor.hex()))

    def refresh_pair(self, shard, owner):
        shard, pair = str(shard), owner.pair.snapshot_id
        with self._pair_lock:
            self.record_counts[shard] = owner.memory.episode_count
            self.cue_totals[shard] = owner.memory.cue_total
            previous = self.pair_ids.get(shard)
            if previous == pair:
                return
            if previous is not None:
                self._xor_pair_component(shard, previous)
            self.pair_ids[shard] = pair
            self.record_counts[shard] = owner.memory.episode_count
            self._xor_pair_component(shard, pair)
            self._logical_snapshot_id = self._snapshot_from_pair_xor()

    def logical_snapshot(self):
        with self._pair_lock:
            return self._logical_snapshot_id

    def logical_record_count(self):
        return sum(self.record_counts.values())

    def logical_cue_total(self):
        return sum(self.cue_totals.values())

    def read_directory_complete(self):
        return all(bool(self.exact_backfill.get(shard, {}).get('complete'))
                   for shard in ('main', *self.ids())
                   if shard == 'main' or (self.bundles[shard] / 'memory.sqlite3').is_file())

    def shards_for_cues(self, cues):
        if not self.read_directory_complete():
            raise ValueError('cue_shard_directory_not_ready')
        result = set()
        for cue in cues:
            result.update(self.cue_shards.shards_for(cue))
        return result

    def _owner_with_episode(self, identifier):
        if not identifier:
            return None
        if isinstance(identifier, str) and identifier.startswith('memory:'):
            exact = self.exact_replay(identifier)
            if exact is not None:
                return self.main_for(exact['shard'])
        if identifier in self.primary.memory.records:
            return self.primary
        for shard in self.bundles:
            owner = self.hot.get(shard)
            if owner is not None and identifier in owner.memory.records:
                return owner
            view = self.warm.get(shard)
            if view is not None and view.memory is not None and identifier in view.memory.records:
                return self.main_for(shard)
            if (owner is None and (view is None or view.memory is None)
                    and (self.bundles[shard] / 'memory.sqlite3').is_file()):
                cold = WarmView(shard, self.bundles[shard])
                try:
                    if identifier in cold.refresh().memory.records:
                        return self.main_for(shard)
                finally:
                    cold.close()
        return None

    def _owner_with_source(self, source):
        if not source:
            return None
        routed = self.exact.source_shard(str(source))
        if routed is not None:
            return self.main_for(routed)
        cue = 'source:' + str(source)
        if self.primary.memory.episode_ids_for_cue(cue):
            return self.primary
        for shard in self.bundles:
            owner = self.hot.get(shard)
            memory = owner.memory if owner is not None else (
                self.warm[shard].memory if shard in self.warm else None)
            if memory is not None and memory.episode_ids_for_cue(cue):
                return owner if owner is not None else self.main_for(shard)
            if memory is None and (self.bundles[shard] / 'memory.sqlite3').is_file():
                cold = WarmView(shard, self.bundles[shard])
                try:
                    if cold.refresh().memory.episode_ids_for_cue(cue):
                        return self.main_for(shard)
                finally:
                    cold.close()
        return None

    def main_for_ingest(self, row, requested=None):
        """Route one observation without breaking original-address lineage.

        Explicit routing wins.  A superseding revision follows the original
        episode; another revision of an existing source follows that source.
        Only a previously unseen source uses the current append shard.
        """
        self._admit_resources()
        if requested not in (None, '', 'main'):
            return self.main_for(requested)
        owner = self._owner_with_episode(row.get('supersedes'))
        if owner is None:
            owner = self._owner_with_source(row.get('source'))
        if owner is not None:
            return owner
        if not self.auto_ids and self.primary.memory.episode_count < self.bundle_limit:
            return self.primary
        if not self.auto_ids:
            self._new_auto_shard()
        active = self.main_for(self.auto_ids[-1])
        if active.memory.episode_count >= self.bundle_limit:
            active = self.main_for(self._new_auto_shard())
        return active

    def ingest(self, row, requested=None):
        owner = self.main_for_ingest(row, requested)
        receipt = owner.ingest(row)
        self.refresh_pair(self._shard_of(owner), owner)
        self._register_exact(owner, receipt['episode_id'])
        return receipt

    def ingest_many(self, rows, requested=None):
        """Batch ingest with automatic capacity boundaries and stable source routing."""
        rows = list(rows)
        if not rows:
            return self.primary.ingest_many(rows)
        self._admit_resources()
        if requested not in (None, '', 'main'):
            owner = self.main_for(requested)
            receipt = owner.ingest_many(rows)
            self.refresh_pair(self._shard_of(owner), owner)
            for result in receipt['results']:
                self._register_exact(owner, result['episode_id'])
            return receipt
        results, groups, journaled = [], [], 0
        index = 0
        while index < len(rows):
            owner = self.main_for_ingest(rows[index])
            capacity = max(1, self.bundle_limit - owner.memory.episode_count)
            chunk = [rows[index]]
            index += 1
            while index < len(rows) and len(chunk) < capacity:
                existing = (self._owner_with_episode(rows[index].get('supersedes'))
                            or self._owner_with_source(rows[index].get('source')))
                if existing is not None and existing is not owner:
                    break
                chunk.append(rows[index]); index += 1
            receipt = owner.ingest_many(chunk)
            self.refresh_pair(self._shard_of(owner), owner)
            for result in receipt['results']:
                self._register_exact(owner, result['episode_id'])
            results.extend(receipt['results'])
            journaled += receipt['journaled']
            groups.append(dict(shard='main' if owner is self.primary else
                               next(key for key, value in self.hot.items() if value is owner),
                               count=len(chunk), pair_snapshot_id=receipt['pair_snapshot_id']))
        last = groups[-1]['pair_snapshot_id'] if groups else self.primary.pair.snapshot_id
        return dict(status='observations_recorded', count=len(rows), journaled=journaled,
                    added=sum(int(result.get('distinct_source_episode_added', 0)) for result in results),
                    pair_snapshot_id=last, results=results, shards=groups,
                    bundle=self.primary.bundle())

    def _shard_of(self, owner):
        if owner is self.primary:
            return 'main'
        for identifier, candidate in self.hot.items():
            if candidate is owner:
                return identifier
        raise ValueError('exact_replay_owner_not_registered')

    def _register_exact(self, owner, identifier):
        memory = owner.memory
        episode = memory.episode(identifier)
        shard = self._shard_of(owner)
        row = memory._store['row_of'][identifier]
        self.exact.put(identifier, shard, row, episode)
        self.cue_shards.put_many(episode.cues, shard, identifier)
        for source in episode.source_addresses:
            self.exact.put_source(source, shard)
        proposition = episode.steps[0].observation.get('proposition_id')
        if proposition:
            self.exact.put_proposition(proposition, shard, identifier)

    def exact_replay(self, identifier):
        return self.exact.get(identifier)

    def backfill_exact(self, budget=256):
        """Incrementally add pre-repair experiences without delaying startup."""
        remaining, added = max(0, int(budget)), 0
        owners = [('main', self.primary)]
        temporary = None
        for identifier in self.ids():
            owner = self.hot.get(identifier)
            if owner is None:
                view = self.warm.get(identifier)
                owner = view.generation if view is not None else None
            if owner is not None:
                owners.append((identifier, owner))
        ready = {shard for shard, _ in owners}
        if remaining:
            for shard in self.ids():
                if (shard in ready or self.exact_backfill.get(shard, {}).get('complete')
                        or not (self.bundles[shard] / 'memory.sqlite3').is_file()):
                    continue
                temporary = WarmView(shard, self.bundles[shard])
                owners.append((shard, temporary.refresh()))
                break
        try:
            for shard, owner in owners:
                if not remaining:
                    break
                start = self._exact_cursor(shard, owner)
                ids = owner.memory._store['ids']
                stop = min(owner.memory.episode_count, start + remaining)
                for row in range(start, stop):
                    identifier = ids[row]
                    episode = owner.memory.episode(identifier)
                    added += int(self.exact.put(identifier, shard, row, episode))
                    self.cue_shards.put_many(episode.cues, shard, identifier)
                    for source in episode.source_addresses:
                        self.exact.put_source(source, shard)
                    proposition = episode.steps[0].observation.get('proposition_id')
                    if proposition:
                        self.exact.put_proposition(proposition, shard, identifier)
                if (stop != start or (stop >= owner.memory.episode_count
                                      and not self.exact_backfill.get(shard, {}).get('complete'))):
                    self._advance_exact_cursor(shard, owner, stop)
                remaining -= stop - start
        finally:
            if temporary is not None:
                temporary.close()
        complete = all(bool(self.exact_backfill.get(shard, {}).get('complete'))
                       for shard in ('main', *self.ids())
                       if shard == 'main' or (self.bundles[shard] / 'memory.sqlite3').is_file())
        return dict(scanned=budget - remaining, added=added, complete=complete)

    def backfill_projections(self, budget=1):
        """Derive current read projections from existing complete VRS shards.

        This is a bounded reconstruction, not a compatibility read.  One cold
        complete generation is opened, projected from its real graph/regions,
        and closed before the next shard is considered.
        """
        remaining, rows = max(0, int(budget)), []
        for shard in self.ids():
            if not remaining:
                break
            if shard in self.hot or shard in self.closing:
                continue
            expected = self.pair_ids.get(shard)
            store = ReadProjectionStore(self.bundles[shard], shard)
            if expected and store.ready(expected):
                continue
            view = self.warm.pop(shard, None)
            temporary = view if view is not None else WarmView(shard, self.bundles[shard])
            try:
                owner = temporary.refresh()
                self.admit_read_resources()
                receipt = store.write(owner)
                with self.projection_lock:
                    cached = self.projection_views.pop(shard, None)
                    if cached is not None:
                        cached.close()
                    self.projection_errors.pop(shard, None)
                rows.append(dict(status='projected', **receipt))
            except Exception as error:
                message = type(error).__name__ + ': ' + str(error)[:160]
                with self.projection_lock:
                    self.projection_errors[shard] = message
                rows.append(dict(status='failed', shard=shard, error=message))
            finally:
                temporary.close()
            remaining -= 1
        complete = all(self.read_ready(shard) for shard in self.ids())
        return dict(scanned=len(rows), projections=rows, complete=complete)

    _SEQUENCE_BITS = 48
    _SEQUENCE_MASK = (1 << _SEQUENCE_BITS) - 1

    @classmethod
    def _sequence(cls, shard_index, local_sequence):
        return (int(shard_index) << cls._SEQUENCE_BITS) | int(local_sequence)

    def export_observations(self, after_sequence=0, *, max_records=512,
                            max_bytes=768 * 1024):
        """Session-end export across the main and every automatic shard.

        The cursor encodes ``(shard index, shard journal sequence)``.  Each row
        still comes from that shard's VRS journal through ``Main`` and retains
        its exact original envelope and episode address.  This path is used only
        for ended-session assimilation, never for recall.
        """
        after_sequence = max(0, int(after_sequence))
        max_records = max(1, min(int(max_records), 4096))
        max_bytes = max(4096, min(int(max_bytes), 896 * 1024))
        shard_index = after_sequence >> self._SEQUENCE_BITS
        local_after = after_sequence & self._SEQUENCE_MASK
        shard_ids = ['main', *self.auto_ids]
        if shard_index >= len(shard_ids):
            return dict(status='experience_export', rows=[], after_sequence=after_sequence,
                        next_sequence=after_sequence, head_sequence=after_sequence,
                        complete=True, grants_authority=False)
        rows, used, next_sequence = [], 64, after_sequence
        complete = False
        while shard_index < len(shard_ids):
            if rows and max_bytes - used < 4096:
                break
            identifier = shard_ids[shard_index]
            owner = self.primary if identifier == 'main' else self.main_for(identifier)
            page = owner.export_observations(local_after,
                max_records=max_records - len(rows), max_bytes=max_bytes - used)
            for item in page['rows']:
                size = len(json.dumps(item['observation'], ensure_ascii=False,
                                      separators=(',', ':')).encode('utf-8')) + 64
                item = dict(item)
                item['sequence'] = self._sequence(shard_index, item['sequence'])
                rows.append(item); used += size
            next_sequence = self._sequence(shard_index, page['next_sequence'])
            if len(rows) >= max_records or used >= max_bytes or not page['complete']:
                break
            shard_index += 1
            local_after = 0
            if shard_index < len(shard_ids):
                next_sequence = self._sequence(shard_index, 0)
            else:
                complete = True
        head_sequence = next_sequence if complete else self._sequence(
            len(shard_ids) - 1, self._SEQUENCE_MASK)
        return dict(status='experience_export', rows=rows,
                    after_sequence=after_sequence, next_sequence=next_sequence,
                    head_sequence=head_sequence, complete=complete,
                    grants_authority=False)

    def _close_later(self, bundle_id, main):
        """Checkpoint + close off the request thread (seconds at 5k records; the hook client times out at 5 s).
        The journal is the truth, so a warm view opened meanwhile reads the last checkpoint plus the tail."""
        def run():
            try:
                try:
                    ReadProjectionStore(self.bundles[bundle_id], bundle_id).write(main)
                    with self.projection_lock:
                        previous = self.projection_views.pop(bundle_id, None)
                        if previous is not None:
                            previous.close()
                        self.projection_errors.pop(bundle_id, None)
                except Exception as error:
                    with self.projection_lock:
                        self.projection_errors[bundle_id] = type(error).__name__ + ': ' + str(error)[:160]
                    raise
                finally:
                    main.close()
            finally:
                with self.lock:
                    if self.closing.get(bundle_id) is thread:
                        del self.closing[bundle_id]
        thread = threading.Thread(target=run, name=f'vrs2-close-{bundle_id}', daemon=True)
        self.closing[bundle_id] = thread
        thread.start()
        return thread

    def _projection_unlocked(self, bundle_id):
        if bundle_id in (None, '', 'main'):
            return None
        error = self.projection_errors.get(bundle_id)
        if error is not None:
            raise ValueError('read_projection_build_failed:' + error)
        expected = self.pair_ids.get(bundle_id)
        cached = self.projection_views.get(bundle_id)
        if cached is not None and cached.pair_snapshot_id == expected:
            self.projection_views.move_to_end(bundle_id)
            return cached
        if cached is not None:
            cached.close()
            self.projection_views.pop(bundle_id, None)
        view = ReadProjectionStore(self.bundles[bundle_id], bundle_id).open(expected)
        if view is not None:
            self.projection_views[bundle_id] = view
            while len(self.projection_views) > 64:
                _, retired = self.projection_views.popitem(last=False)
                retired.close()
        return view

    def projection(self, bundle_id):
        with self.projection_lock:
            return self._projection_unlocked(bundle_id)

    def read_ready(self, bundle_id):
        """Whether one complete current VRS generation is readable without loading it."""
        if bundle_id in (None, '', 'main'):
            return True
        owner, _ = self.peek_ready(bundle_id)
        if owner is not None:
            return True
        if bundle_id in self.closing or bundle_id in self.projection_errors:
            return False
        expected = self.pair_ids.get(bundle_id)
        return bool(expected and ReadProjectionStore(
            self.bundles[bundle_id], bundle_id).ready(expected))

    def current_vrs(self, exact):
        """Current VRS facts for an exact capsule without opening a cold checkpoint."""
        shard, identifier, row = exact['shard'], exact['replay'].episode_id, exact['shard_row']
        if shard == 'main':
            owner = self.primary
        else:
            owner, _ = self.peek_ready(shard)
        if owner is not None:
            stable = owner.graph.stable
            portals = projected_portals(owner)
            node = owner.graph.nodes.episode_node.get(identifier)
            pending = stable is None or node is None or node >= stable.node_count
            weights = getattr(stable, 'record_weight', None) if stable is not None else None
            source = exact['replay'].source_addresses[0]
            center = owner.graph.nodes.episode_node.get(identifier)
            edge_strength = {}
            if center is not None:
                lo, hi = int(owner.graph.flat.out_ptr[center]), int(owner.graph.flat.out_ptr[center + 1])
                edge_strength = {int(owner.graph.flat.dst[edge]): float(owner.graph.flat.strength[edge])
                                 for edge in owner.graph.flat.out_edge[lo:hi]}
            cue_strengths = []
            for cue_id in owner.memory._store['cues'][row]:
                node = owner.graph.nodes.cue(int(cue_id))
                cue_strengths.append(edge_strength.get(node, 0.0) if node >= 0 else 0.0)
            return dict(shard=shard, pair_snapshot_id=owner.pair.snapshot_id,
                graph_snapshot_id=owner.graph.snapshot_id,
                stable_version_id=(stable.version_id if stable is not None else None),
                strength=owner.graph.strength(identifier), region=owner.graph.region_of(identifier),
                weight=None if pending or weights is None else float(weights[node]),
                state=None if pending else float(stable.state[node]),
                stability=None if pending else float(stable.stability[node]), pending=pending,
                usage=owner.graph.usage.get(source),
                memberships=owner.graph.memberships_of(identifier),
                cue_strengths=tuple(cue_strengths),
                superseded_by=owner.memory.superseded.get(identifier), portals=tuple(portals))
        with self.projection_lock:
            projection = self._projection_unlocked(shard)
            return None if projection is None else projection.current(identifier, row)

    def region_for_cue(self, shard, cue):
        if shard == 'main':
            owner = self.primary
        else:
            owner, _ = self.peek_ready(shard)
        if owner is not None:
            cue_id = owner.memory._store['vocab'].id_of(cue)
            if cue_id is None:
                return None
            node = owner.graph.nodes.cue(cue_id)
            labels = owner.graph.labels()
            return None if node < 0 or node >= len(labels) or labels[node] < 0 else int(labels[node])
        with self.projection_lock:
            projection = self._projection_unlocked(shard)
            return None if projection is None else projection.region_for_cue(cue)

    def decision_for_proposition(self, shard, proposition):
        if shard == 'main':
            owner = self.primary
        else:
            owner, _ = self.peek_ready(shard)
        if owner is not None:
            stable = owner.graph.stable
            return None if stable is None else (getattr(stable, 'decisions', None) or {}).get(proposition)
        with self.projection_lock:
            projection = self._projection_unlocked(shard)
            return None if projection is None else projection.decision(proposition)

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

    def peek_ready(self, bundle_id):
        """Read readiness without scheduling a load (status must stay side-effect free)."""
        with self.lock:
            main = self.hot.get(bundle_id)
            if main is not None:
                return main, 'hot'
            if bundle_id in self.closing:
                return None, 'closing'
            view = self.warm.get(bundle_id)
            if view is not None and view.generation is not None:
                return view.generation, 'warm'
            return None, 'cold'

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
        if (self._rss_bytes() or 0) > MAX_RSS_BYTES:
            view.close()
            raise ValueError('vrs_memory_budget_exceeded')
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
        # A close thread removes itself while holding ``self.lock``.  Join it
        # before entering that lock or an immediate reopen deadlocks.
        self._wait_closed(bundle_id)
        with self.lock:
            main = self.hot.get(bundle_id)
            if main is not None:
                self.hot.move_to_end(bundle_id)
                return main
            view = self.warm.pop(bundle_id, None)
            if view is not None:
                view.close()
            existed = (self.bundles[bundle_id] / 'memory.sqlite3').is_file()
            main = Main(self.bundles[bundle_id], allow_ingest=True, bundle_limit=self.bundle_limit,
                        defer_checkpoints=True)
            if not existed:
                self.exact_backfill[bundle_id] = dict(count=0, tail=None, cue_total=0, complete=True)
                self.record_counts[bundle_id] = 0
                self.cue_totals[bundle_id] = 0
            self.hot[bundle_id] = main
            self.wanted.discard(bundle_id)
            self.refresh_pair(bundle_id, main)
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
        exact = self.exact_replay(episode_id) if episode_id.startswith('memory:') else None
        if exact is not None:
            return exact['shard']
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
        with self.projection_lock:
            for view in self.projection_views.values():
                view.close()
            self.projection_views.clear()
