"""Generation-bound endpoint adjacency, not a proof of numerical locality.

All signs/strengths/outcomes remain represented. Callers must separately prove
the VRS update's influence rule; these addresses alone do not justify omitting
global shuffle/reinforcement dependencies or restricting cognition.
"""
import operator
import numpy as np

from .mosaic_vrs_address_index import _frozen, _require_immutable_edges, KEY_DTYPE


def _segment(edges, offset):
    result = []
    for field in ('source', 'target'):
        order = np.argsort(edges[field], kind='stable')
        result.append((_frozen(edges[field][order]), _frozen((order+offset).astype(np.uint32))))
    return tuple(result)


def _merge(left, right):
    merged = []
    for (ln, li), (rn, ri) in zip(left, right):
        nodes, ids = np.concatenate((ln, rn)), np.concatenate((li, ri))
        order = np.argsort(nodes, kind='stable')
        merged.append((_frozen(nodes[order]), _frozen(ids[order])))
    return tuple(merged)


class EndpointDependencyIndex:
    __slots__ = ('_source', '_segments')

    def __init__(self):
        raise TypeError('use build')

    def __setattr__(self, name, value):
        raise AttributeError('dependency index is immutable')

    @classmethod
    def _create(cls, source, segments):
        obj = object.__new__(cls)
        object.__setattr__(obj, '_source', source)
        object.__setattr__(obj, '_segments', segments)
        return obj

    @classmethod
    def build(cls, source):
        _require_immutable_edges(source)
        return cls._create(source, (_segment(source, 0),))

    def require_source(self, source):
        if source is not self._source:
            raise ValueError('dependency index belongs to a different immutable generation')

    def edges_for(self, node, *, direction):
        node = operator.index(node)
        if not 0 <= node <= 0xFFFFFFFF or direction not in ('outgoing', 'incoming'):
            raise ValueError('invalid endpoint query')
        # Python int promotes uint32 search arrays to int64 in NumPy, causing
        # a full-size temporary for what should be a logarithmic hot lookup.
        # The bounds above make this exact dtype binding lossless.
        probe = np.uint32(node)
        result = []
        for segment in self._segments:
            nodes, ids = segment[0 if direction == 'outgoing' else 1]
            lo, hi = np.searchsorted(nodes, probe, side='left'), np.searchsorted(nodes, probe, side='right')
            result.append(ids[lo:hi])
        return _frozen(np.concatenate(result))

    @property
    def index_bytes(self):
        return sum(nodes.nbytes+ids.nbytes for s in self._segments for nodes, ids in s)

    def advance(self, successor):
        _require_immutable_edges(successor)
        count = len(self._source)
        if len(successor) < count or any(not np.array_equal(successor[n][:count], self._source[n]) for n in KEY_DTYPE.names):
            raise ValueError('successor changed endpoint prefix')
        if len(successor) == count:
            return type(self)._create(successor, self._segments)
        segments = [*self._segments, _segment(successor[count:], count)]
        # Keep the cold base shared. Compact only delta segments with geometric
        # size growth; at most O(log accumulated delta edges) tail segments.
        while len(segments) > 2 and 2*len(segments[-1][0][0]) >= len(segments[-2][0][0]):
            combined = _merge(segments[-2], segments[-1])
            segments[-2:] = [combined]
        return type(self)._create(successor, tuple(segments))
