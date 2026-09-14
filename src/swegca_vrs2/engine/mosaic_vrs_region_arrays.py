"""Packed immutable region-to-node index shared by cold build and cold load."""
from collections.abc import Sequence
from dataclasses import dataclass
import operator

import numpy as np


@dataclass(frozen=True)
class RegionTermArrays(Sequence):
    offsets: np.ndarray
    nodes: np.ndarray

    def __len__(self):
        return len(self.offsets)-1

    def __getitem__(self, key):
        if isinstance(key, slice):
            return tuple(self[i] for i in range(*key.indices(len(self))))
        key = operator.index(key)
        if key < 0:
            key += len(self)
        if not 0 <= key < len(self):
            raise IndexError(key)
        lo, hi = self.offsets[key:key+2]
        return self.nodes[lo:hi]


def reverse_memberships(offsets, groups, count):
    """Same stable node order as the former per-region append lists."""
    nodes = np.repeat(np.arange(len(offsets)-1, dtype=np.int64), np.diff(offsets))
    order = np.argsort(groups, kind='stable')
    reverse_offsets = np.r_[0, np.cumsum(np.bincount(groups, minlength=count))]
    # Immutable bytes ownership: no mutable construction buffers survive.
    return RegionTermArrays(np.frombuffer(reverse_offsets.tobytes(), dtype=np.int64),
                            np.frombuffer(nodes[order].tobytes(), dtype=np.int64))
