from dataclasses import dataclass
from types import MappingProxyType
import operator
import re
import numpy as np
from .mosaic_immutable_numeric import immutable_numeric_array
from .mosaic_vrs_dependency_index import EndpointDependencyIndex, _segment, _merge


def array_binding(value):
    if type(value) is SparseEventVector:
        value.require_binding()
        return (value,)
    if type(value) is SparseEventEdges:
        value.require_binding()
        return (value,)
    if type(value) is not np.ndarray:
        raise ValueError('unknown event array representation')
    return (id(value), value.shape, value.dtype, value.strides,
            value.__array_interface__['data'][0], value.flags.writeable)


def _put(tree, index, value, shift=24):
    key = (index >> shift) & 255
    result = dict(tree)
    result[key] = value if shift == 0 else _put(tree.get(key, {}), index, value, shift-8)
    return MappingProxyType(result)


@dataclass(frozen=True, slots=True, eq=False, init=False)
class SparseEventVector:
    _base: object
    _binding: tuple
    _tree: object
    size: int
    dtype: object

    @classmethod
    def extend(cls, parent, *, indices=(), values, append):
        """Parent must already have crossed EventVrsInputs cold validation."""
        if type(parent) is cls:
            parent.require_binding()
            base, binding, tree = parent._base, parent._binding, parent._tree
        elif type(parent) is np.ndarray:
            owner = parent
            while isinstance(owner, np.ndarray):
                if owner.flags.writeable:
                    raise ValueError('event parent must have immutable backing')
                owner = owner.base
            if type(owner) is not bytes or parent.ndim != 1 or parent.dtype.hasobject:
                raise ValueError('event parent must have immutable numeric backing')
            # Structured edge fields are strided. A detached view shares the
            # verified bytes; immutable_numeric_array would copy that prefix.
            base = parent.view()
            binding, tree = array_binding(base), MappingProxyType({})
        else:
            raise ValueError('verified event parent representation required')
        count, dtype = len(parent), parent.dtype
        if (type(indices) is not tuple or len(set(indices)) != len(indices)
                or any(type(i) is not int or not 0 <= i < count for i in indices)
                or type(values) is not np.ndarray or values.dtype != dtype or values.shape != (len(indices),)
                or type(append) is not np.ndarray or append.dtype != dtype or append.ndim != 1
                or count + len(append) > 0x100000000):
            raise ValueError('event delta shape, dtype or address changed')
        if not np.isfinite(values).all() or not np.isfinite(append).all():
            raise ValueError('event delta contains nonfinite values')
        for index, value in zip(indices, values):
            tree = _put(tree, index, value)
        for index, value in enumerate(append, count):
            tree = _put(tree, index, value)
        obj = object.__new__(cls)
        for name, value in (('_base', base), ('_binding', binding), ('_tree', tree),
                            ('size', count + len(append)), ('dtype', dtype)):
            object.__setattr__(obj, name, value)
        return obj

    def require_binding(self):
        if array_binding(self._base) != self._binding:
            raise ValueError('event shared base metadata changed')

    def __len__(self):
        return self.size

    def __getitem__(self, index):
        index = operator.index(index)
        if not 0 <= index < self.size:
            raise IndexError('event numeric address outside directory')
        tree = self._tree
        for shift in (24, 16, 8, 0):
            key = (index >> shift) & 255
            if key not in tree:
                return self._base[index]
            tree = tree[key]
        return tree

    def __array__(self, *args, **kwargs):
        raise TypeError('event delta cannot implicitly materialize the full array')


@dataclass(frozen=True, slots=True, eq=False)
class SparseEventEdges:
    fields: object
    dtype: object
    size: int

    def __getitem__(self, field):
        return self.fields[field]

    def __len__(self):
        return self.size

    def require_binding(self):
        for values in self.fields.values():
            values.require_binding()


def prepare_event_delta(parent, *, snapshot_id, appended_direct, appended_score,
                        appended_unresolved, appended_edges, appended_strength,
                        base_indices=(), base_values=None, strength_indices=(), strength_values=None,
                        direct_indices=(), direct_values=None, score_indices=(), score_values=None,
                        unresolved_indices=(), unresolved_values=None):
    """Prepare a candidate numerical input, sharing the verified cold parent.

    Old endpoints/signs cannot change through this constructor. Changing a base
    or current strength is a numeric proposal, not a main re-evidence verdict.
    Callers retain provenance, seed selection, storage rounding and commit gates.
    """
    from .mosaic_vrs_event_kernel import EventVrsInputs
    if type(parent) is not EventVrsInputs:
        raise ValueError('cold-bound event parent required')
    parent.require_validated_immutable()
    if type(snapshot_id) is not str or re.fullmatch('[0-9a-f]{64}', snapshot_id) is None:
        raise ValueError('event inputs need a generation digest')
    if (type(appended_edges) is not np.ndarray or appended_edges.ndim != 1
            or appended_edges.dtype != parent.edges.dtype):
        raise ValueError('appended event edge layout changed')
    added = immutable_numeric_array(appended_edges)
    def vector(old, indices, values, append):
        return SparseEventVector.extend(old, indices=indices,
            values=np.empty(0, dtype=old.dtype) if values is None else values, append=append)
    direct = vector(parent.direct, direct_indices, direct_values, appended_direct)
    score = vector(parent.score, score_indices, score_values, appended_score)
    unresolved = vector(parent.unresolved, unresolved_indices, unresolved_values, appended_unresolved)
    strength = vector(parent.strength, strength_indices, strength_values, appended_strength)
    if (len(direct) != len(score) or len(unresolved) != len(score)
            or len(strength) != len(parent.edges)+len(added) or len(strength) > 0xFFFFFFFF):
        raise ValueError('event delta node/edge counts disagree')
    base_dtype = parent.edges.dtype['vrs_strength']
    base_values = np.empty(0, dtype=base_dtype) if base_values is None else base_values
    if (not np.isfinite(added['vrs_strength']).all() or np.any(added['vrs_strength'] < 0)
            or np.any(base_values < 0) or np.any(appended_strength < 0)
            or strength_values is not None and np.any(strength_values < 0)
            or np.any(~np.isin(added['sign'], (-1, 0, 1)))
            or len(added) and max(added['source'].max(), added['target'].max()) >= len(score)):
        raise ValueError('event delta edge values or endpoints changed')
    fields = {}
    for field in parent.edges.dtype.names:
        old = parent.edges[field]
        fields[field] = vector(old, base_indices if field == 'vrs_strength' else (),
                               base_values if field == 'vrs_strength' else None, added[field])
    edges = SparseEventEdges(MappingProxyType(fields), parent.edges.dtype, len(strength))
    # Only the constructor above can preserve the old endpoint/sign prefix by
    # construction. The public ndarray advance still checks arbitrary prefixes.
    segments = parent.dependencies._segments
    if len(added):
        segments = (*segments, _segment(added, len(parent.edges)))
        while len(segments) > 2 and 2*len(segments[-1][0][0]) >= len(segments[-2][0][0]):
            segments = (*segments[:-2], _merge(segments[-2], segments[-1]))
    dependencies = EndpointDependencyIndex._create(edges, segments)
    obj = object.__new__(EventVrsInputs)
    for key, value in (('snapshot_id', snapshot_id), ('direct', direct), ('score', score),
                       ('edges', edges), ('strength', strength), ('unresolved', unresolved),
                       ('dependencies', dependencies)):
        object.__setattr__(obj, key, value)
    obj._record_bindings()
    object.__setattr__(obj, '_delta_parent', parent)
    object.__setattr__(obj, '_score_indices', score_indices)
    object.__setattr__(obj, '_strength_indices', strength_indices)
    object.__setattr__(obj, '_direct_indices', direct_indices)
    return obj
