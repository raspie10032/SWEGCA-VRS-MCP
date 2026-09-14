"""Whole-experience navigation regions derived from current VRS connectivity.

Cold preparation only. Weighted modularity local moves discover dense cores;
incident mass supplies overlapping membership, not truth or evidence promotion.
No LLM, experience slicing, outcome filtering, or persistent-state writes.
The full source memory stays independently accessible, including ungrouped data.
"""
from __future__ import annotations

from dataclasses import dataclass
from array import array
import hashlib
import math
import operator
from types import MappingProxyType

import numpy as np

from .mosaic_vrs_region_arrays import RegionTermArrays, reverse_memberships


def _frozen(values):
    values = np.asarray(values)
    return np.frombuffer(values.tobytes(), dtype=values.dtype).reshape(values.shape)


def _csr(rows, columns, weights, size):
    # Sort weights too, so parallel-edge summation is independent of input order.
    order = np.lexsort((weights, columns, rows))
    rows, columns, weights = rows[order], columns[order], weights[order]
    start = np.r_[True, (rows[1:] != rows[:-1]) | (columns[1:] != columns[:-1])]
    if len(rows):
        positions = np.flatnonzero(start)
        rows, columns = rows[positions], columns[positions]
        weights = np.add.reduceat(weights, positions)
    offsets = np.r_[0, np.cumsum(np.bincount(rows, minlength=size))]
    return offsets, columns, weights


def _canonical(labels):
    _, first, inverse = np.unique(labels, return_index=True, return_inverse=True)
    remap = np.empty(len(first), dtype=np.int64)
    remap[np.argsort(first)] = np.arange(len(first))
    return remap[inverse]


def _local_moves(offsets, neighbors, weights, maximum_sweeps):
    size = len(offsets) - 1
    degree = np.asarray([weights[offsets[i]:offsets[i + 1]].sum() for i in range(size)])
    mass = float(degree.sum())
    labels, totals = np.arange(size), degree.copy()
    if not mass:
        return labels, True, 0
    for sweep in range(maximum_sweeps):
        moved = False
        for node in range(size):
            if not degree[node]:
                continue
            lo, hi = offsets[node:node + 2]
            other = neighbors[lo:hi] != node
            groups, inverse = np.unique(labels[neighbors[lo:hi][other]], return_inverse=True)
            values = np.bincount(inverse, weights=weights[lo:hi][other], minlength=len(groups))
            into = dict(zip(map(int, groups), map(float, values)))
            old = int(labels[node])
            totals[old] -= degree[node]
            score = lambda group: into.get(group, 0.0) - degree[node] * totals[group] / mass
            best, best_score = old, score(old)
            tolerance = 64 * np.finfo(float).eps * max(mass, 1.0)
            for group in sorted(into):
                candidate = score(group)
                if candidate > best_score + tolerance:
                    best, best_score = group, candidate
            totals[best] += degree[node]
            labels[node] = best
            moved |= best != old
        if not moved:
            return _canonical(labels), True, sweep + 1
    return _canonical(labels), False, maximum_sweeps


def _communities(offsets, neighbors, weights, maximum_sweeps, maximum_levels, local_move_backend=None):
    labels = np.arange(len(offsets) - 1)
    sweeps = []
    for _ in range(maximum_levels):
        local, converged, count = (local_move_backend or _local_moves)(
            offsets, neighbors, weights, maximum_sweeps)
        sweeps.append(count)
        labels = _canonical(local[labels])
        if not converged:
            return labels, False, tuple(sweeps)
        size = int(local.max()) + 1 if len(local) else 0
        if size == len(local):
            return labels, True, tuple(sweeps)
        rows = np.repeat(np.arange(len(local)), np.diff(offsets))
        offsets, neighbors, weights = _csr(local[rows], local[neighbors], weights, size)
    return labels, False, tuple(sweeps)


def _memberships(offsets, neighbors, weights, core):
    """Preserve reference reductions; accumulate unboxed numeric values only."""
    member_offsets = np.empty(len(core)+1, dtype=np.int64)
    member_offsets[0] = 0
    memberships, coefficients = array('q'), array('d')
    for node in range(len(core)):
        lo, hi = offsets[node:node + 2]
        groups, inverse = np.unique(core[neighbors[lo:hi]], return_inverse=True)
        masses = np.bincount(inverse, weights=weights[lo:hi], minlength=len(groups))
        association = {int(g): float(w) for g, w in zip(groups, masses) if w > 0}
        if not association:
            association = {int(core[node]): 1.0}
        total = math.fsum(association.values())
        for group in sorted(association):
            memberships.append(group)
            coefficients.append(association[group] / total)
        member_offsets[node+1] = len(memberships)
    return (_frozen(member_offsets),
            _frozen(np.asarray(memberships, dtype=np.int64)),
            _frozen(np.asarray(coefficients, dtype=np.float64)))


@dataclass(frozen=True)
class SharedExperienceBridge:
    """One original address shared by regions, not a new synthetic experience."""

    pair_snapshot_id: str
    topology_id: str
    episode_id: str
    revision: str
    source_addresses: tuple[str, ...]
    outcomes: tuple[str, ...]
    memberships: tuple[tuple[int, float], ...]
    grants_authority: bool = False

    @property
    def key(self):
        return (self.pair_snapshot_id, self.episode_id, self.revision)


@dataclass(frozen=True)
class ConnectivityRegions:
    """Immutable derived topology. Region scores grant no semantic authority."""

    vrs_snapshot_id: str
    topology_id: str
    terms: tuple[str, ...]
    core_labels: np.ndarray
    edge_source: np.ndarray
    edge_target: np.ndarray
    edge_sign: np.ndarray
    strengths: np.ndarray
    member_offsets: np.ndarray
    member_regions: np.ndarray
    member_weights: np.ndarray
    region_terms: RegionTermArrays
    converged: bool
    sweeps: tuple[int, ...]
    source: object

    @classmethod
    def build(cls, source, *, vrs_snapshot_id, maximum_sweeps=100, maximum_levels=32,
              local_move_backend=None):
        if (not isinstance(vrs_snapshot_id, str) or len(vrs_snapshot_id) != 64
                or any(c not in '0123456789abcdef' for c in vrs_snapshot_id)):
            raise ValueError('current VRS content digest required')
        for value in (maximum_sweeps, maximum_levels):
            if type(value) is not int or value < 1:
                raise ValueError('positive explicit work limits required')
        terms = source.terms if type(source.terms) in (tuple, range) else tuple(source.terms)
        arrays = [np.asarray(x) for x in (source.edge_source, source.edge_target,
                                         source.edge_sign, source.vrs_strength)]
        if any(a.ndim != 1 for a in arrays) or len({len(a) for a in arrays}) != 1:
            raise ValueError('edge array shapes differ')
        u, v, sign, strength = arrays
        if (u.dtype.kind not in 'iu' or v.dtype.kind not in 'iu' or sign.dtype.kind not in 'iu'
                or np.any(u < 0) or np.any(v < 0) or np.any(u >= len(terms))
                or np.any(v >= len(terms)) or not np.isfinite(strength).all()
                or np.any(strength < 0)):
            raise ValueError('invalid VRS endpoint, sign or strength')
        u, v = u.astype(np.int64, copy=False), v.astype(np.int64, copy=False)
        strength = strength.astype(np.float64, copy=False)
        # Negative/refuting relation signs have positive *association* magnitude.
        # Signs and all zero-strength edges remain in the separate exact arrays.
        offsets, neighbors, weights = _csr(np.r_[u, v], np.r_[v, u],
                                           np.r_[strength, strength], len(terms))
        if not np.isfinite(weights.sum()):
            raise ValueError('association mass overflow')
        core, converged, sweeps = _communities(offsets, neighbors, weights,
                                               maximum_sweeps, maximum_levels, local_move_backend)
        member_offsets, memberships, coefficients = _memberships(offsets, neighbors, weights, core)
        region_terms = reverse_memberships(member_offsets, memberships,
            int(core.max()) + 1 if len(core) else 0)
        digest = hashlib.sha256(b'vrs-connectivity-modularity-overlap-v1\0')
        digest.update(vrs_snapshot_id.encode('ascii'))
        digest.update(core.astype('<i8').tobytes())
        digest.update(str((maximum_sweeps, maximum_levels, converged)).encode('ascii'))
        return cls(vrs_snapshot_id, digest.hexdigest(), terms, _frozen(core),
                   _frozen(u), _frozen(v), _frozen(sign), _frozen(strength),
                   member_offsets, memberships, coefficients,
                   region_terms, converged, sweeps, source)

    def require_source(self, source, vrs_snapshot_id):
        if source is not self.source or vrs_snapshot_id != self.vrs_snapshot_id:
            raise ValueError('region topology belongs to a different VRS generation')

    def memberships_for_term(self, node):
        node = operator.index(node)
        if not 0 <= node < len(self.terms):
            raise KeyError(node)
        lo, hi = self.member_offsets[node:node + 2]
        return tuple((int(g), float(w)) for g, w in
                     zip(self.member_regions[lo:hi], self.member_weights[lo:hi]))

    def memberships_for_episode(self, episode):
        """Use existing cue addresses; never split or rewrite the parent episode.

        No match returns an explicit ungrouped result, NOT inaccessible memory.
        Retrieval usefulness/time/agenda must be supplied by later observed
        telemetry; this version does not manufacture those component scores.
        """
        nodes = {n for cue in episode.cues for n in self.source.address_index.term_ids(cue)}
        scores = {}
        for node in sorted(nodes):
            for group, weight in self.memberships_for_term(node):
                scores[group] = scores.get(group, 0.0) + weight
        total = math.fsum(scores.values())
        return tuple((g, scores[g] / total) for g in sorted(scores)) if total else ()

    def require_pair(self, pair):
        if pair.vrs_snapshot_id != self.vrs_snapshot_id:
            raise ValueError('region topology belongs to a different VRS generation')
        pending, visited = [pair.memory], set()
        while pending:
            source = pending.pop()
            if source is self.source:
                return
            if id(source) not in visited:
                visited.add(id(source))
                pending.extend(getattr(source, 'sources', ()))
        raise ValueError('current VRS source is absent from full memory')

    def bridge_for_episode(self, pair, episode_id):
        """A shared whole experience is itself the group-to-group navigation key."""
        self.require_pair(pair)
        if pair.memory.lookup_requires_io:
            raise ValueError('prepare hot memory before region navigation')
        episode = pair.memory.episode(episode_id)
        memberships = self.memberships_for_episode(episode)
        if len(memberships) < 2:
            return None
        return SharedExperienceBridge(pair.snapshot_id, self.topology_id,
            episode.episode_id, episode.revision, episode.source_addresses,
            tuple(step.outcome for step in episode.steps), memberships)

    def candidates(self, pair, region_ids):
        """Hot cue-index union/intersection; proposals only, no recall allowlist.

        Each requested region is an OR of its terms; multiple regions intersect.
        Caller still performs the normal four-stage memory activation and must
        keep independent full-memory access. This function is not that pipeline.
        """
        self.require_pair(pair)
        memory = pair.memory
        groups = tuple(dict.fromkeys(operator.index(g) for g in region_ids))
        if not groups or any(not 0 <= g < len(self.region_terms) for g in groups):
            raise ValueError('existing region IDs required')
        if memory.lookup_requires_io:
            raise ValueError('prepare hot memory before region navigation')
        result = None
        for group in groups:
            ids = {identifier for node in self.region_terms[group]
                   for identifier in memory.episode_ids_for_cue(self.terms[node])}
            result = ids if result is None else result.intersection(ids)
        return tuple(sorted(result))

    def receipt(self):
        return MappingProxyType(dict(topology_id=self.topology_id,
            vrs_snapshot_id=self.vrs_snapshot_id, region_count=len(self.region_terms),
            term_count=len(self.terms), edge_count=len(self.edge_source),
            converged=self.converged, sweeps=self.sweeps,
            membership_is_truth=False, grants_authority=False,
            full_memory_access_restricted=False, original_experiences_split=0))
