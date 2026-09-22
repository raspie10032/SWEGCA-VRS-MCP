"""Dictionary-coded hot index with lossless bounded resident memory.

The index keeps the full memory activation contract
(``HotMemoryIndex``: ``episode``, ``episode_ids_for_cue``, ``episode_count``,
``outcome_counts``, ``snapshot_id``; plus ``propositions``/``superseded``/``append``
used by the composition) with a compact representation:

* **vocabulary** — each cue string is stored once, id = insertion order; ids are packed
  in zlib blocks of ``BLOCK`` strings so one lookup decodes one block, not the whole
  dictionary (partial decompression); string→id goes through a sorted 64-bit hash table
  verified against the decoded string;
* **records** — cue ids as a ``uint32`` array per record; original text, metadata and
  provenance as one zlib blob per record, decoded only when that record is replayed;
* **postings** — per cue id, a sorted ``uint32`` array of record rows.

Generations stay immutable by construction: every structure is append-only and each
generation carries the record count it can see, so an older generation never observes
rows or cue ids added after it. ``episode()`` rebuilds a ``MemoryEpisode`` on demand and
keeps a bounded cache; cue normalization (``_cue``) is applied once at ingress so the
rebuilt object equals what the engine would have built.
"""
from __future__ import annotations

import hashlib
import json
import re
import sys
import threading
from time import perf_counter_ns
import zlib
from collections import OrderedDict

import numpy as np
from immutables import Map

from .engine.mosaic_memory_activation import MemoryEpisode, MemoryStep, OUTCOMES, _cue, RecallCandidate, RecallResult

_DESCRIPTION = re.compile(r'(?m)^description:[ \t]*(.+)$')
_ASKS_MARK = re.compile(r'(?:^|\n)[ \t]*(?:##[ \t]*)?찾을 때 묻는 말[ \t]*[:：]?|\(찾을 때 묻는 말[ \t]*[:：]')


def asks_and_description(text):
    """(asks, description): the explicit 「찾을 때 묻는 말」 and, for a memory doc, its front-matter
    description — the author's one-line phrasing of what the doc answers, written in task words
    (23 of 295 docs have an asks section; every doc has a description). Either may be ''."""
    asks = ''
    m = _ASKS_MARK.search(text)
    if m is not None:
        tail = text[m.end():]
        tail = tail.split('\n## ', 1)[0]          # doc section ends at the next heading
        asks = tail.strip(': \n')[:800]
    description = ''
    if text.startswith('---'):
        head = text[3:].split('\n---', 1)[0]
        d = _DESCRIPTION.search(head)
        if d is not None:
            description = d.group(1).strip().strip('"\'')[:400]
    return asks, description


def _recall_columns(text, proposition):
    """Per-row recall columns (2026-09-18): the proposition id ('' when none) and the casefolded
    (asks, description) pair. Recall reads these instead of materializing the episode: at 10k records
    the proposition closure alone built ~4k episodes per query (measured 2.8 s of a 3.4 s recall)."""
    asks, description = asks_and_description(text or '')
    return proposition or '', (asks.casefold(), description.casefold())


RECALL_COLUMNS = ('props', 'asks', 'revs', 'outs')


class LightView:
    """The engine's replay/re-evidence stages need each candidate's steps, sources and revision — never its
    cue strings, which are what an episode build spends its time on (~330 vocabulary lookups per record).
    This view answers ``episode`` with the same generation's light episode (cues empty)."""
    __slots__ = ('_index', 'snapshot_id')

    def __init__(self, index):
        self._index, self.snapshot_id = index, index.snapshot_id

    def episode(self, identifier):
        return self._index.episode_light(identifier)

BLOCK = 128           # cue strings per vocabulary block
BLOCK_CACHE = 1 << 20  # decoded vocabulary blocks kept resident: strings are interned and shared, so the
                       # whole decoded vocabulary is cheaper than re-decoding blocks on every episode rebuild
LIGHT_CACHE = 16384   # light episodes (no cue strings) kept resident for replay/re-evidence (2026-09-18)
EPISODE_CACHE = 4096  # rebuilt MemoryEpisode objects kept resident (cue strings are interned, so a cached
                      # episode costs one reference per cue, not one string per cue)


def _hash64(text):
    return int.from_bytes(hashlib.blake2b(text.encode('utf-8'), digest_size=8).digest(), 'little')


def _digest(value):
    return hashlib.sha256(json.dumps(value, ensure_ascii=False, sort_keys=True,
                                     separators=(',', ':')).encode('utf-8')).hexdigest()


class Vocab:
    """Append-only cue dictionary with block compression and hashed lookup.

    Readers (recall) run without the owner's lock while one writer appends (2026-09-15), so the
    lookup table is one ``(hashes, ids)`` tuple replaced atomically, the block cache is guarded by
    a small lock, and ``_block`` reads ``tail`` before ``blocks`` (the writer appends the flushed
    block before it resets the tail, so a reader never sees a fresh empty tail with old blocks).
    """
    __slots__ = ('blocks', 'tail', '_table', 'count', '_cache', '_lock')

    def __init__(self):
        self.blocks, self.tail = [], []
        self._table = (np.empty(0, dtype=np.uint64), np.empty(0, dtype=np.uint32))
        self.count = 0
        self._cache = OrderedDict()
        self._lock = threading.Lock()

    @property
    def hashes(self):
        return self._table[0]

    @property
    def ids(self):
        return self._table[1]

    def __getstate__(self):
        hashes, ids = self._table
        return dict(blocks=self.blocks, tail=self.tail, hashes=hashes, ids=ids, count=self.count)

    def __setstate__(self, state):
        self.blocks, self.tail, self.count = state['blocks'], state['tail'], state['count']
        self._table = (state['hashes'], state['ids'])
        self._cache = OrderedDict()
        self._lock = threading.Lock()

    def _block(self, number):
        tail = self.tail                      # before blocks: see class docstring
        blocks = self.blocks
        if number >= len(blocks):
            return tail
        with self._lock:
            strings = self._cache.get(number)
            if strings is not None:
                return strings
        # interned: every episode that carries this cue shares one string object
        strings = [sys.intern(t) for t in zlib.decompress(blocks[number]).decode('utf-8').split('\n')]
        with self._lock:
            self._cache[number] = strings
            if len(self._cache) > BLOCK_CACHE:
                self._cache.popitem(last=False)
        return strings

    def string_of(self, cue_id):
        return self._block(cue_id // BLOCK)[cue_id % BLOCK]

    def id_of(self, text, limit=None):
        """Id of ``text`` below ``limit`` (a generation's cue count) or None."""
        hashes, ids = self._table             # one atomic read of the pair
        if not len(hashes):
            return None
        h = np.uint64(_hash64(text))
        lo = int(np.searchsorted(hashes, h, side='left'))
        hi = int(np.searchsorted(hashes, h, side='right'))
        for cue_id in ids[lo:hi]:
            cue_id = int(cue_id)
            if (limit is None or cue_id < limit) and self.string_of(cue_id) == text:
                return cue_id
        return None

    def add(self, texts):
        """Ids for ``texts`` (all distinct, none present yet), appended in order."""
        first = self.count
        for text in texts:
            self.tail.append(sys.intern(text))
            if len(self.tail) == BLOCK:
                self.blocks.append(zlib.compress('\n'.join(self.tail).encode('utf-8'), 6))
                self.tail = []
        self.count += len(texts)
        new_hashes = np.fromiter((_hash64(t) for t in texts), dtype=np.uint64, count=len(texts))
        new_ids = np.arange(first, self.count, dtype=np.uint32)
        hashes, ids = self._table
        hashes = np.concatenate([hashes, new_hashes])
        ids = np.concatenate([ids, new_ids])
        order = np.argsort(hashes, kind='stable')
        self._table = (hashes[order], ids[order])     # single assignment: readers see old or new, never mixed
        return list(range(first, self.count))


class _Records:
    """len()/in view over the visible records of one generation."""
    __slots__ = ('index',)

    def __init__(self, index):
        self.index = index

    def __len__(self):
        return self.index.count

    def __contains__(self, identifier):
        row = self.index._store['row_of'].get(identifier)
        return row is not None and row < self.index.count

    def __iter__(self):
        return self.index.iter_episode_ids()


class CompactIndex:
    """One generation over shared append-only storage (see module docstring)."""
    lookup_requires_io = False
    semantic_families = ()

    def __init__(self, store, count, snapshot_id, outcome_counts, propositions, superseded, cue_total):
        self._store = store            # shared: vocab, ids, row_of, cues, postings, blobs, cache
        self.count = count
        self.snapshot_id = snapshot_id
        self.outcome_counts = outcome_counts
        self.propositions = propositions
        self.superseded = superseded
        self.cue_total = int(cue_total)

    @classmethod
    def empty(cls, identity):
        store = dict(vocab=Vocab(), ids=[], row_of={}, cues=[], postings={},
            blobs=[], cache=OrderedDict(), kinds=[], props=[], asks=[], revs=[],
            outs=[], light_cache=OrderedDict())
        return cls(store, 0, _digest(['memory', identity]), Map({o: 0 for o in OUTCOMES}), Map(), Map(), 0)

    # ── HotMemoryIndex contract ─────────────────────────────────────────
    @property
    def episode_count(self):
        return self.count

    @property
    def records(self):
        return _Records(self)

    def iter_episode_ids(self):
        return iter(self._store['ids'][:self.count])

    def revision_outcome(self, identifier):
        """Return the original's indexed revision and outcome in this generation."""
        row = self._store['row_of'].get(identifier)
        if row is None or row >= self.count:
            raise KeyError(identifier)
        return self._store['revs'][row], self._store['outs'][row]

    def episode_ids_for_cue(self, cue):
        store = self._store
        cue_id = store['vocab'].id_of(cue)
        if cue_id is None:
            return ()
        rows = store['postings'].get(cue_id)
        if rows is None:
            return ()
        visible = rows[:int(np.searchsorted(rows, self.count))]
        ids = store['ids']
        return tuple(ids[int(r)] for r in visible)

    def episode(self, identifier):
        store = self._store
        row = store['row_of'].get(identifier)
        if row is None or row >= self.count:
            raise KeyError(identifier)
        cache = store['cache']
        lock = store.setdefault('cache_lock', threading.Lock())
        stats = store.setdefault('stats', {'hits': 0, 'decodes': 0})     # G8 (2026-09-19): hot hits vs blob decodes
        with lock:
            episode = cache.get(row)
            if episode is not None:
                cache.move_to_end(row)
                stats['hits'] += 1
                return episode
        stats['decodes'] += 1
        episode = self._build(row)
        with lock:
            cache[row] = episode
            if len(cache) > EPISODE_CACHE:
                cache.popitem(last=False)
        return episode

    def _build(self, row):
        store = self._store
        payload = json.loads(zlib.decompress(store['blobs'][row]).decode('utf-8'))
        vocab = store['vocab']
        cues = tuple(vocab.string_of(int(c)) for c in store['cues'][row])
        step = MemoryStep('external_observation', {'text': payload['text'], 'metadata': payload['metadata'],
            'proposition_id': payload['proposition'], 'evidence_polarity': payload['polarity'],
            'supersedes': payload['supersedes'], 'current_truth_claimed': False}, (),
            'Recorded external observation; truth and action authority not granted.',
            payload['outcome'], (payload['source'],))
        # cues were normalized with the engine's _cue at ingress: skip the per-cue re-normalization
        episode = object.__new__(MemoryEpisode)
        for name, value in (('episode_id', store['ids'][row]), ('cues', cues), ('steps', (step,)),
                            ('source_addresses', (payload['source'],)), ('revision', payload['revision']),
                            ('verification_state', 'unverified')):
            object.__setattr__(episode, name, value)
        return episode

    # ── ingress ──────────────────────────────────────────────────────────
    def append(self, row):
        from .store import keys
        identifier = 'memory:' + _digest({k: v for k, v in row.items() if k != 'request_id'})
        store = self._store
        if identifier in self.records:
            return self, identifier
        previous = row['supersedes']
        if previous is not None:
            old = self.episode(previous)
            if old.source_addresses != (row['source'],) or old.revision == row['revision'] or previous in self.superseded:
                raise ValueError('invalid_source_revision_successor')
        cues = tuple(dict.fromkeys(_cue(c) for c in (*keys(row['text']), *(c.casefold() for c in row['cues']))))
        if row['proposition'] is not None:
            cues = (*cues, _cue('proposition:' + row['proposition']))
        if not cues:
            cues = (_cue('source:' + row['source']),)
        cues = tuple(dict.fromkeys((*cues, identifier)))
        if len(store['ids']) != self.count:
            raise ValueError('compact_index_generation_is_not_the_head')
        vocab = store['vocab']
        cue_ids, fresh = [], []
        for cue in cues:
            found = vocab.id_of(cue)
            if found is None:
                fresh.append(cue)
                cue_ids.append(None)
            else:
                cue_ids.append(found)
        if fresh:
            new_ids = iter(vocab.add(fresh))
            cue_ids = [next(new_ids) if c is None else c for c in cue_ids]
        new_row = self.count
        store['ids'].append(identifier)
        store['row_of'][identifier] = new_row
        store['cues'].append(np.asarray(cue_ids, dtype=np.uint32))
        postings = store['postings']
        for cue_id in cue_ids:
            old_rows = postings.get(cue_id)
            postings[cue_id] = (np.array([new_row], dtype=np.uint32) if old_rows is None
                                else np.append(old_rows, np.uint32(new_row)))
        payload = dict(text=row['text'], metadata=row['metadata'], proposition=row['proposition'],
                       polarity=row['polarity'], supersedes=previous, outcome=row['outcome'],
                       source=row['source'], revision=row['revision'])
        store['blobs'].append(zlib.compress(json.dumps(payload, ensure_ascii=False, separators=(',', ':')).encode('utf-8'), 6))
        store.setdefault('kinds', []).append(str((row['metadata'] or {}).get('kind') or ''))
        prop, asks = _recall_columns(row['text'], row['proposition'])
        store.setdefault('props', []).append(prop); store.setdefault('asks', []).append(asks)
        store.setdefault('revs', []).append(row['revision']); store.setdefault('outs', []).append(row['outcome'])
        propositions = self.propositions
        if row['proposition']:
            p = row['proposition']
            propositions = propositions.set(p, propositions.get(p, frozenset()) | {identifier})
        successor = CompactIndex(store, new_row + 1, _digest([self.snapshot_id, identifier]),
                                 self.outcome_counts.set(row['outcome'], self.outcome_counts[row['outcome']] + 1),
                                 propositions,
                                 self.superseded if previous is None else self.superseded.set(previous, identifier),
                                 self.cue_total + len(cue_ids))
        return successor, identifier

    def truncate_to(self, count):
        """Undo appends beyond ``count`` on the shared store (a successor that never committed).

        Vocabulary entries added by the discarded append stay: they have no postings, so
        no generation can observe them, and a later append simply reuses them.
        """
        store = self._store
        while len(store['ids']) > count:
            row = len(store['ids']) - 1
            identifier = store['ids'].pop()
            for column in ('kinds', *RECALL_COLUMNS):
                if store.get(column):
                    store[column].pop()
            store['row_of'].pop(identifier, None)
            store['blobs'].pop()
            for cue_id in store['cues'].pop():
                rows = store['postings'].get(int(cue_id))
                if rows is not None:
                    kept = rows[rows < row]
                    if len(kept):
                        store['postings'][int(cue_id)] = kept
                    else:
                        del store['postings'][int(cue_id)]
            store['cache'].pop(row, None)
            store.get('light_cache', {}).pop(row, None)

    def __getstate__(self):
        state = dict(self.__dict__)
        shared = dict(state['_store'])
        shared['cache'] = None
        shared.pop('light_cache', None)
        shared.pop('stats', None)
        shared.pop('kind_masks', None)
        shared.pop('cache_lock', None)
        state['_store'] = shared
        return state

    def __setstate__(self, state):
        if type(state) is not dict or set(state) != {
                '_store', 'count', 'snapshot_id', 'outcome_counts',
                'propositions', 'superseded', 'cue_total'}:
            raise ValueError('checkpoint_compact_index_schema_invalid')
        self.__dict__.update(state)
        required = {'vocab', 'ids', 'row_of', 'cues', 'postings', 'blobs',
                    'cache', 'kinds', *RECALL_COLUMNS}
        if not required <= self._store.keys():
            raise ValueError('checkpoint_compact_store_schema_invalid')
        columns = ('ids', 'cues', 'blobs', 'kinds', *RECALL_COLUMNS)
        if any(len(self._store[name]) != self.count for name in columns):
            raise ValueError('checkpoint_compact_store_cardinality_invalid')
        if self._store.get('cache') is None:
            self._store['cache'] = OrderedDict()
        self._store['light_cache'] = OrderedDict()

    def recall_candidates(self, signal, navigation_cues=()):
        """The engine's ``recall_memory`` on cue ids (2026-09-18): the same ``RecallResult`` — the same
        candidate set (every record carrying a matched or navigation cue; this index has no semantic
        families), the same ``matched_cues`` in record cue order, the same Jaccard ``cue_overlap``
        (|matched| / |record cues ∪ current cues|) and the same order — without decoding any record's
        ~330 cue strings: only the matched ones are looked up. Measured at 10k records: recall 0.9 s -> ?"""
        if signal.snapshot_id != self.snapshot_id:
            raise ValueError("déjà vu snapshot changed before recall")
        store = self._store
        vocab = store['vocab']
        navigation = tuple(dict.fromkeys(_cue(cue) for cue in navigation_cues))
        current = set(signal.current_cues).union(navigation)
        current_ids = {}
        for cue in current:
            cue_id = vocab.id_of(cue)
            if cue_id is not None:
                current_ids[cue_id] = cue
        rows = set()
        for cue in dict.fromkeys((*signal.matched_cues, *navigation)):
            rows.update(int(r) for r in self.rows_for_cue(cue))
        ids, cue_rows = store['ids'], store['cues']
        revs, outs = store.get('revs'), store.get('outs')
        size = len(current)
        candidates = []
        for row in rows:
            row_cues = cue_rows[row]
            matched = tuple(current_ids[int(c)] for c in row_cues if int(c) in current_ids)
            if revs is not None and row < len(revs):
                revision, outcome = revs[row], outs[row]
            else:
                episode = self.episode(ids[row]); revision, outcome = episode.revision, episode.steps[0].outcome
            candidates.append(RecallCandidate(episode_id=ids[row], matched_cues=matched,
                cue_overlap=len(matched) / (len(row_cues) + size - len(matched)),
                revision=revision, verification_state='unverified', historical_outcomes=(outcome,)))
        candidates.sort(key=lambda c: (-c.cue_overlap, c.episode_id))
        return RecallResult(signal.query, tuple(candidates), self.snapshot_id, source_dependencies=())

    def episode_light(self, identifier):
        """The record without its cue strings (steps, sources, revision) — what replay and re-evidence
        read. Cached separately from full episodes (a light one is a few hundred bytes of payload)."""
        store = self._store
        row = store['row_of'].get(identifier)
        if row is None or row >= self.count:
            raise KeyError(identifier)
        cache = store.setdefault('light_cache', OrderedDict())
        lock = store.setdefault('cache_lock', threading.Lock())
        stats = store.setdefault('stats', {'hits': 0, 'decodes': 0})
        with lock:
            episode = cache.get(row)
            if episode is not None:
                cache.move_to_end(row)
                stats['hits'] += 1
                return episode
            full = store['cache'].get(row)          # a resident full episode: no blob decode at all
        if full is not None:
            stats['hits'] += 1
            steps, source, revision = full.steps, full.source_addresses[0], full.revision
        else:
            stats['decodes'] += 1
            payload = json.loads(zlib.decompress(store['blobs'][row]).decode('utf-8'))
            steps = (MemoryStep('external_observation', {'text': payload['text'], 'metadata': payload['metadata'],
                'proposition_id': payload['proposition'], 'evidence_polarity': payload['polarity'],
                'supersedes': payload['supersedes'], 'current_truth_claimed': False}, (),
                'Recorded external observation; truth and action authority not granted.',
                payload['outcome'], (payload['source'],)),)
            source, revision = payload['source'], payload['revision']
        episode = object.__new__(MemoryEpisode)
        for name, value in (('episode_id', store['ids'][row]), ('cues', ()), ('steps', steps),
                            ('source_addresses', (source,)), ('revision', revision),
                            ('verification_state', 'unverified')):
            object.__setattr__(episode, name, value)
        with lock:
            cache[row] = episode
            if len(cache) > LIGHT_CACHE:
                cache.popitem(last=False)
        return episode

    def stats(self):
        """Blob accounting since load (G8, 2026-09-19): ``hits`` answered from a resident episode, ``decodes``
        paid a zlib+json decode. Shared by every generation of this store; a request reads the delta."""
        return dict(self._store.setdefault('stats', {'hits': 0, 'decodes': 0}))

    def prefetch_light(self, limit=None, budget_ns=None):
        """Decode light episodes newest-first into the resident cache — the preparer's job at start-up so the
        first judgments run from RAM (G8). Stops at the cache size, ``limit`` rows or ``budget_ns``."""
        store = self._store
        cache = store.setdefault('light_cache', OrderedDict())
        room = LIGHT_CACHE - len(cache)
        if limit is not None:
            room = min(room, int(limit))
        started = perf_counter_ns() if budget_ns else None
        done = 0
        ids = store['ids']
        for row in range(self.count - 1, -1, -1):
            if done >= room:
                break
            if budget_ns and perf_counter_ns() - started > budget_ns:
                break
            if row in cache:
                continue
            self.episode_light(ids[row])
            done += 1
        return done

    def rows_for_cue(self, cue):
        """Visible row numbers carrying ``cue`` (the id-free form of ``episode_ids_for_cue``)."""
        store = self._store
        cue_id = store['vocab'].id_of(cue)
        if cue_id is None:
            return ()
        rows = store['postings'].get(cue_id)
        if rows is None:
            return ()
        return rows[:int(np.searchsorted(rows, self.count))]

    def proposition_of_row(self, row):
        """The row's explicit proposition id or None — a list read, no episode build."""
        return self._store['props'][row] or None

    def proposition_of(self, identifier):
        row = self._store['row_of'].get(identifier)
        if row is None or row >= self.count:
            raise KeyError(identifier)
        return self.proposition_of_row(row)

    def asks_of(self, identifier):
        """Casefolded (asks, description) of the record — the ask/description gate's input."""
        row = self._store['row_of'].get(identifier)
        if row is None or row >= self.count:
            raise KeyError(identifier)
        return self._store['asks'][row]

    def masked(self, exclude_kinds):
        """A view of this generation whose postings skip records of the given kinds.

        Local adapter (2026-09-15): folder-listing records (kind ``fs_listing``) carry hundreds of
        file-name tokens and doubled the candidate set of ordinary prompts; the hook asks for them
        only on location-shaped prompts. Everything else (episode, propositions, superseded,
        snapshot) is the same generation, so receipts and ids are unchanged.
        """
        exclude = tuple(exclude_kinds or ())
        if not exclude:
            return self
        store = self._store
        key = (self.count, exclude)
        masks = store.get('kind_masks') or {}
        mask = masks.get(key)
        if mask is None:
            kinds = store['kinds']
            mask = np.fromiter((kind not in exclude for kind in kinds[:self.count]),
                               dtype=bool, count=self.count)
            store['kind_masks'] = {key: mask}      # atomic replace; readers hold old or new dict
        return self.masked_rows(mask)

    def masked_rows(self, mask):
        """A view whose postings skip rows where ``mask`` is False (same generation; ids and receipts
        unchanged). A view of a view intersects the masks (region scope on top of kind exclusion)."""
        mask = np.asarray(mask, dtype=bool)
        if len(mask) < self.count:                          # rows appended after the mask was built: keep them
            mask = np.concatenate([mask, np.ones(self.count - len(mask), dtype=bool)])
        current = getattr(self, '_mask', None)
        if current is not None:
            mask = mask[:len(current)] & current if len(mask) >= len(current) else mask & current[:len(mask)]
        view = MaskedIndex.__new__(MaskedIndex)
        view.__dict__.update(self.__dict__)
        view._mask = mask
        return view

    def footprint(self):
        store = self._store
        return dict(records=self.count, cues=store['vocab'].count,
                    vocab_bytes=sum(len(b) for b in store['vocab'].blocks),
                    cue_id_bytes=sum(a.nbytes for a in store['cues']),
                    posting_bytes=sum(a.nbytes for a in store['postings'].values()),
                    text_bytes=sum(len(b) for b in store['blobs']))


class MaskedIndex(CompactIndex):
    """CompactIndex generation with postings filtered by a row mask (see ``CompactIndex.masked``)."""

    def episode_ids_for_cue(self, cue):
        store = self._store
        cue_id = store['vocab'].id_of(cue)
        if cue_id is None:
            return ()
        rows = store['postings'].get(cue_id)
        if rows is None:
            return ()
        visible = rows[:int(np.searchsorted(rows, self.count))]
        visible = visible[self._mask[visible]]
        ids = store['ids']
        return tuple(ids[int(r)] for r in visible)

    def rows_for_cue(self, cue):
        rows = CompactIndex.rows_for_cue(self, cue)
        return rows[self._mask[rows]] if len(rows) else rows

    def __getstate__(self):
        raise TypeError('a masked view is not persisted')
