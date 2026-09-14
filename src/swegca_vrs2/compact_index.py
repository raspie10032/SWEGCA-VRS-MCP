# -*- coding: utf-8 -*-
"""Dictionary-coded hot index — lossless hard compression of the hot memory (2026-09-14).

The ported ``HotIndex`` keeps every record as a resident ``MemoryEpisode`` (a tuple of
cue strings per record) and every cue as a ``frozenset`` of 71-character episode ids.
With substring cues that is millions of string references: about 700 MB resident and
a 150 MB checkpoint for 2,600 records. This index keeps the same contract
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
import sys
import zlib
from collections import OrderedDict

import numpy as np
from immutables import Map

from .engine.mosaic_memory_activation import MemoryEpisode, MemoryStep, OUTCOMES, _cue

BLOCK = 128           # cue strings per vocabulary block
BLOCK_CACHE = 1 << 20  # decoded vocabulary blocks kept resident: strings are interned and shared, so the
                       # whole decoded vocabulary is cheaper than re-decoding blocks on every episode rebuild
EPISODE_CACHE = 4096  # rebuilt MemoryEpisode objects kept resident (cue strings are interned, so a cached
                      # episode costs one reference per cue, not one string per cue)


def _hash64(text):
    return int.from_bytes(hashlib.blake2b(text.encode('utf-8'), digest_size=8).digest(), 'little')


def _digest(value):
    return hashlib.sha256(json.dumps(value, ensure_ascii=False, sort_keys=True,
                                     separators=(',', ':')).encode('utf-8')).hexdigest()


class Vocab:
    """Append-only cue dictionary with block compression and hashed lookup."""
    __slots__ = ('blocks', 'tail', 'hashes', 'ids', 'count', '_cache')

    def __init__(self):
        self.blocks, self.tail = [], []
        self.hashes = np.empty(0, dtype=np.uint64)
        self.ids = np.empty(0, dtype=np.uint32)
        self.count = 0
        self._cache = OrderedDict()

    def __getstate__(self):
        return dict(blocks=self.blocks, tail=self.tail, hashes=self.hashes, ids=self.ids, count=self.count)

    def __setstate__(self, state):
        for key, value in state.items():
            setattr(self, key, value)
        self._cache = OrderedDict()

    def _block(self, number):
        strings = self._cache.get(number)
        if strings is None:
            if number == len(self.blocks):
                return self.tail
            # interned: every episode that carries this cue shares one string object
            strings = [sys.intern(t) for t in zlib.decompress(self.blocks[number]).decode('utf-8').split('\n')]
            self._cache[number] = strings
            if len(self._cache) > BLOCK_CACHE:
                self._cache.popitem(last=False)
        return strings

    def string_of(self, cue_id):
        return self._block(cue_id // BLOCK)[cue_id % BLOCK]

    def id_of(self, text, limit=None):
        """Id of ``text`` below ``limit`` (a generation's cue count) or None."""
        if not len(self.hashes):
            return None
        h = np.uint64(_hash64(text))
        lo = int(np.searchsorted(self.hashes, h, side='left'))
        hi = int(np.searchsorted(self.hashes, h, side='right'))
        for cue_id in self.ids[lo:hi]:
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
        hashes = np.concatenate([self.hashes, new_hashes])
        ids = np.concatenate([self.ids, new_ids])
        order = np.argsort(hashes, kind='stable')
        self.hashes, self.ids = hashes[order], ids[order]
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

    def __init__(self, store, count, snapshot_id, outcome_counts, propositions, superseded):
        self._store = store            # shared: vocab, ids, row_of, cues, postings, blobs, cache
        self.count = count
        self.snapshot_id = snapshot_id
        self.outcome_counts = outcome_counts
        self.propositions = propositions
        self.superseded = superseded

    @classmethod
    def empty(cls, identity):
        store = dict(vocab=Vocab(), ids=[], row_of={}, cues=[], postings={}, blobs=[], cache=OrderedDict())
        return cls(store, 0, _digest(['memory', identity]), Map({o: 0 for o in OUTCOMES}), Map(), Map())

    # ── HotMemoryIndex contract ─────────────────────────────────────────
    @property
    def episode_count(self):
        return self.count

    @property
    def records(self):
        return _Records(self)

    def iter_episode_ids(self):
        return iter(self._store['ids'][:self.count])

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
        episode = cache.get(row)
        if episode is not None:
            cache.move_to_end(row)
            return episode
        episode = self._build(row)
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

    # ── ingress (same rules as HotIndex.append) ─────────────────────────
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
        propositions = self.propositions
        if row['proposition']:
            p = row['proposition']
            propositions = propositions.set(p, propositions.get(p, frozenset()) | {identifier})
        successor = CompactIndex(store, new_row + 1, _digest([self.snapshot_id, identifier]),
                                 self.outcome_counts.set(row['outcome'], self.outcome_counts[row['outcome']] + 1),
                                 propositions,
                                 self.superseded if previous is None else self.superseded.set(previous, identifier))
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

    # ── conversion from the ported HotIndex (old checkpoints) ───────────
    @classmethod
    def from_hot(cls, hot):
        """Bulk conversion of a ported HotIndex: one vocabulary build, postings by one sort."""
        index = cls.empty('convert')
        store = index._store
        rows = list(hot.records.items())
        # 1. vocabulary in one pass (insertion order = first appearance), ids per record
        first_id = {}
        order = []
        per_record = []
        for identifier, episode in rows:
            ids = []
            for cue in dict.fromkeys(_cue(c) for c in episode.cues):
                cue_id = first_id.get(cue)
                if cue_id is None:
                    cue_id = len(order)
                    first_id[cue] = cue_id
                    order.append(cue)
                ids.append(cue_id)
            per_record.append(np.asarray(ids, dtype=np.uint32))
        store['vocab'].add(order)
        # 2. records
        for row_number, (identifier, episode) in enumerate(rows):
            obs = episode.steps[0].observation
            store['ids'].append(identifier)
            store['row_of'][identifier] = row_number
            store['cues'].append(per_record[row_number])
            payload = dict(text=obs['text'], metadata=dict(obs.get('metadata') or {}),
                           proposition=obs.get('proposition_id'), polarity=obs.get('evidence_polarity'),
                           supersedes=obs.get('supersedes'), outcome=episode.steps[0].outcome,
                           source=episode.source_addresses[0], revision=episode.revision)
            store['blobs'].append(zlib.compress(json.dumps(payload, ensure_ascii=False, separators=(',', ':')).encode('utf-8'), 6))
        # 3. postings: sort all (cue_id, row) pairs once and slice per cue
        if per_record:
            cue_col = np.concatenate(per_record)
            row_col = np.repeat(np.arange(len(per_record), dtype=np.uint32),
                                [len(a) for a in per_record])
            sort = np.lexsort((row_col, cue_col))
            cue_col, row_col = cue_col[sort], row_col[sort]
            bounds = np.flatnonzero(np.r_[True, cue_col[1:] != cue_col[:-1], True])
            postings = store['postings']
            for lo, hi in zip(bounds[:-1], bounds[1:]):
                postings[int(cue_col[lo])] = row_col[lo:hi].copy()
        return cls(store, len(rows), hot.snapshot_id, hot.outcome_counts, hot.propositions, hot.superseded)

    def __getstate__(self):
        state = dict(self.__dict__)
        shared = dict(state['_store'])
        shared['cache'] = None
        state['_store'] = shared
        return state

    def __setstate__(self, state):
        self.__dict__.update(state)
        if self._store.get('cache') is None:
            self._store['cache'] = OrderedDict()

    def footprint(self):
        store = self._store
        return dict(records=self.count, cues=store['vocab'].count,
                    vocab_bytes=sum(len(b) for b in store['vocab'].blocks),
                    cue_id_bytes=sum(a.nbytes for a in store['cues']),
                    posting_bytes=sum(a.nbytes for a in store['postings'].values()),
                    text_bytes=sum(len(b) for b in store['blobs']))
