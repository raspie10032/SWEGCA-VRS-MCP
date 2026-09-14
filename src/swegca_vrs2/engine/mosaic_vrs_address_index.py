"""Immutable, generation-bound endpoint/sign addresses, not cached judgments.

A cold sorted base is shared across append-only generations. Each extension
sorts only its new canonical keys. Strength changes never become index values.
No file I/O, hashing, full-memory allowlist or semantic authority is involved.
"""
from __future__ import annotations

import operator

import numpy as np

KEY_DTYPE = np.dtype([('source', '<u4'), ('target', '<u4'), ('sign', 'i1')])


def _frozen(array):
    return np.frombuffer(array.tobytes(), dtype=array.dtype).reshape(array.shape)


def _require_immutable_edges(edges):
    if not isinstance(edges, np.ndarray) or edges.ndim != 1 or edges.dtype.hasobject:
        raise ValueError('address index requires a plain one-dimensional edge array')
    if len(edges) > np.iinfo(np.uint32).max:
        raise ValueError('edge IDs exceed uint32 address format')
    for name in KEY_DTYPE.names:
        if name not in (edges.dtype.fields or {}) or edges.dtype[name] != KEY_DTYPE[name]:
            raise ValueError('address key dtype changed')
    root = edges
    while isinstance(root, np.ndarray):
        if root.flags.writeable:
            raise ValueError('address index requires immutable source storage')
        root = root.base
    # setflags(write=False) on an owning ndarray can be reversed by a caller.
    # The resident's bytes-backed arrays cannot be made writable again.
    if not isinstance(root, bytes):
        raise ValueError('address index source must be bytes-backed immutable')


def _keys(edges):
    result = np.empty(len(edges), dtype=KEY_DTYPE)
    for name in KEY_DTYPE.names:
        result[name] = edges[name]
    return result


def _key_order(keys):
    # Same stable (u32 source, u32 target, i8 sign) ordering without the
    # structured-record comparator. lexsort's last key is the primary key.
    return np.lexsort((keys['sign'], keys['target'], keys['source']))


def _segment(edges, first_group):
    keys = _keys(edges)
    order = _key_order(keys)
    sorted_keys = keys[order]
    if len(keys) > 1 and np.any(sorted_keys[1:] == sorted_keys[:-1]):
        raise ValueError('canonical parent contains duplicate endpoint-sign groups')
    ids = (order + first_group).astype(np.uint32)
    return _frozen(sorted_keys), _frozen(ids)


def _merge_segments(left, right):
    keys = np.concatenate((left[0], right[0]))
    ids = np.concatenate((left[1], right[1]))
    order = _key_order(keys)
    return _frozen(keys[order]), _frozen(ids[order])


class CanonicalEdgeAddressIndex:
    """Only build/advance construct validated instances; sources stay immutable."""

    __slots__ = ('_source', '_segments')

    def __init__(self):
        raise TypeError('use CanonicalEdgeAddressIndex.build')

    def __setattr__(self, name, value):
        raise AttributeError('address index is immutable')

    @classmethod
    def _create(cls, source, segments):
        result = object.__new__(cls)
        object.__setattr__(result, '_source', source)
        object.__setattr__(result, '_segments', segments)
        return result

    @classmethod
    def build(cls, source):
        _require_immutable_edges(source)
        segments = (_segment(source, 0),) if len(source) else ()
        return cls._create(source, segments)

    def require_source(self, source):
        if source is not self._source:
            raise ValueError('address index belongs to a different immutable generation')

    @property
    def segment_count(self):
        return len(self._segments)

    @property
    def index_bytes(self):
        return sum(keys.nbytes + ids.nbytes for keys, ids in self._segments)

    def lookup(self, source, target, sign):
        source, target, sign = map(operator.index, (source, target, sign))
        if not (0 <= source <= 0xFFFFFFFF and 0 <= target <= 0xFFFFFFFF and -128 <= sign <= 127):
            raise ValueError('address key out of range')
        probe = np.asarray((source, target, sign), dtype=KEY_DTYPE)[()]
        for keys, ids in self._segments:
            position = int(np.searchsorted(keys, probe))
            if position < len(keys) and keys[position] == probe:
                return int(ids[position])
        return None

    def advance(self, successor):
        """Share old index segments after checking an unchanged key prefix.

        This is O(parent edges) validation, not an O(E log E) rebuild. It is a
        publication-time operation, never an ordinary cognition lookup. Same
        keys with different strengths can rebind without adding a segment.
        """
        _require_immutable_edges(successor)
        count = len(self._source)
        if len(successor) < count or any(not np.array_equal(successor[name][:count], self._source[name])
                                         for name in KEY_DTYPE.names):
            raise ValueError('successor changed canonical address prefix')
        added = successor[count:]
        segments = self._segments
        if len(added):
            segment = _segment(added, count)
            for key in segment[0]:
                if self.lookup(int(key['source']), int(key['target']), int(key['sign'])) is not None:
                    raise ValueError('successor appended an existing canonical address')
            segments = (*segments, segment)
            # Keep cold base shared; compact only the delta tail. Tail sizes
            # decrease by more than 2x, bounding lookup layers logarithmically.
            while len(segments) > 2 and 2*len(segments[-1][0]) >= len(segments[-2][0]):
                segments = (*segments[:-2], _merge_segments(segments[-2], segments[-1]))
        return type(self)._create(successor, segments)

    def advance_event_delta(self, parent, successor):
        """Use prepare_event_delta's preserved key prefix, not a dense scan."""
        from .mosaic_vrs_event_kernel import EventVrsInputs
        if (type(parent) is not EventVrsInputs or type(successor) is not EventVrsInputs
                or getattr(successor, '_delta_parent', None) is not parent):
            raise ValueError('producer-bound topology delta required')
        parent.require_validated_immutable()
        successor.require_validated_immutable()
        self.require_source(parent.edges)
        count = len(parent.edges)
        added = np.empty(len(successor.edges)-count, dtype=KEY_DTYPE)
        for name in KEY_DTYPE.names:
            added[name] = [successor.edges[name][i] for i in range(count, len(successor.edges))]
        segments = self._segments
        if len(added):
            segment = _segment(added, count)
            for key in segment[0]:
                if self.lookup(int(key['source']),int(key['target']),int(key['sign'])) is not None:
                    raise ValueError('successor appended an existing canonical address')
            segments = (*segments, segment)
            while len(segments)>2 and 2*len(segments[-1][0])>=len(segments[-2][0]):
                segments = (*segments[:-2], _merge_segments(segments[-2],segments[-1]))
        return type(self)._create(successor.edges,segments)
