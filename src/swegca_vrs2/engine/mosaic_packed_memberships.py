"""Immutable numeric storage, not validation or cognitive authority.

Buffers are process-local native-endian arrays, never a disk/network format.
Cold callers retain their schema, ordering, normalization and provenance gates.
"""
from array import array
from collections.abc import Sequence
from dataclasses import FrozenInstanceError
from operator import index


class PackedMemberships(Sequence):
    __slots__ = ('_groups', '_weights')

    def __init__(self, groups, weights):
        if (type(groups) is not bytes or type(weights) is not bytes
                or len(groups) != len(weights) or len(groups) % 8):
            raise ValueError('equal immutable 64-bit columns required')
        object.__setattr__(self, '_groups', groups)
        object.__setattr__(self, '_weights', weights)

    def __setattr__(self, name, value):
        raise FrozenInstanceError('immutable membership buffers')

    def __delattr__(self, name):
        raise FrozenInstanceError('immutable membership buffers')

    def __len__(self):
        return len(self._groups) // 8

    def __iter__(self):
        return zip(memoryview(self._groups).cast('Q'), memoryview(self._weights).cast('d'))

    def __getitem__(self, item):
        if isinstance(item, slice):
            return tuple(self[i] for i in range(*item.indices(len(self))))
        item = index(item)
        return (memoryview(self._groups).cast('Q')[item],
                memoryview(self._weights).cast('d')[item])

    def __eq__(self, other):
        if type(other) not in (tuple, PackedMemberships):
            return NotImplemented
        if len(self) != len(other):
            return False
        if type(other) is PackedMemberships and self._groups == other._groups and self._weights == other._weights:
            return True
        return all(a == b for a, b in zip(self, other))

    def __hash__(self):
        # Must match equal tuples; no mutable cross-request cache.
        return hash(tuple(self))

    def __deepcopy__(self, memo):
        # dataclasses.asdict on a detached receipt must retain the public tuple
        # shape, never expose internal byte columns to JSON consumers.
        return tuple(self)


def pack_validated_memberships(groups, weights):
    """Storage conversion AFTER cold validation; preserve arbitrary Python IDs.

    Wide IDs use the original representation, never truncate or cap addressability.
    """
    if not groups or groups[-1] > 2**64 - 1:
        return tuple(zip(groups, weights))
    group_array, weight_array = array('Q', groups), array('d', weights)
    if group_array.itemsize != 8 or weight_array.itemsize != 8:
        return tuple(zip(groups, weights))
    return PackedMemberships(group_array.tobytes(), weight_array.tobytes())
