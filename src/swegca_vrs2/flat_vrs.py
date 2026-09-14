# -*- coding: utf-8 -*-
"""Flat, vectorized VRS event settlement — same rule as ``engine.mosaic_vrs_event_signal``.

Local Windows-scale adapter (2026-09-14). The ported engine settles one node at a time
in Python over sparse trie-backed successors; with real records (hundreds of cue nodes
each, shared with most other records) every ingress re-settles the whole component and
the per-element cost dominates (measured: 70 ms per stored record per ingress, O(N²)).

This module keeps the engine's semantics and changes only the representation and the
arithmetic vehicle:

* nodes/edges live in flat immutable ``bytes``-backed NumPy arrays (same immutability
  contract as ``EventVrsInputs``: writeable=False, owner is ``bytes``);
* one settlement round evaluates every pending node at once with the identical formula
  ``next = f32(.8*old + .2*tanh(direct + .2*signal/max(1, sum|strength|)))`` computed in
  float64 and rounded to float32, on pre-round scores (synchronous, as the engine);
* the propagation rule is identical: a node whose float32 bit pattern changed revisits
  itself and every outgoing target next round; a round budget stays a budget.

The only arithmetic difference is the summation order of ``signal`` (NumPy float64
segment sums instead of ``math.fsum``). ``tools/compare_flat_vrs.py`` checks the final
scores bit-for-bit against the ported engine on real records; any difference is reported
as a count, never hidden.
"""
from __future__ import annotations

import numpy as np

try:
    from scipy.sparse import csr_matrix
except ImportError:  # pragma: no cover
    csr_matrix = None

VERSION = 'vrs-re-evidence-event-signal-f32-v2-experimental'   # same rule, flat vehicle


def frozen(values, dtype=None):
    """Immutable bytes-backed array (the engine's immutability contract).

    An array that already satisfies the contract (read-only, contiguous, its own
    ``bytes`` base, right dtype) is returned as is — unpickled checkpoint arrays are
    exactly that, and copying them doubled resident memory.
    """
    if (isinstance(values, np.ndarray) and not values.flags.writeable and values.flags.c_contiguous
            and isinstance(values.base, bytes) and len(values.base) == values.nbytes
            and (dtype is None or values.dtype == np.dtype(dtype))):
        return values
    array = np.ascontiguousarray(values, dtype=dtype)
    return np.frombuffer(array.tobytes(), dtype=array.dtype).reshape(array.shape)


class FlatGraph:
    """One immutable generation: node vectors, edge arrays and CSR views.

    Edges are directed; every stored edge appears once. ``in_*`` arrays group edges by
    target (incoming), ``out_*`` by source (outgoing). Both are rebuilt from the edge
    arrays with a stable sort, so equal inputs give equal layouts.
    """
    __slots__ = ('count', 'direct', 'score', 'unresolved', 'src', 'dst', 'sign', 'strength',
                 'in_ptr', 'in_edge', 'out_ptr', 'out_edge', 'den')

    def __init__(self, direct, score, unresolved, src, dst, sign, strength):
        self.count = len(direct)
        self.direct = frozen(direct, np.float32)
        self.score = frozen(score, np.float32)
        self.unresolved = frozen(unresolved, bool)
        self.src = frozen(src, np.uint32)
        self.dst = frozen(dst, np.uint32)
        self.sign = frozen(sign, np.int8)
        self.strength = frozen(strength, np.float32)
        if not (len(self.score) == len(self.unresolved) == self.count
                and len(self.src) == len(self.dst) == len(self.sign) == len(self.strength)):
            raise ValueError('flat graph node/edge counts disagree')
        if len(self.src) and max(int(self.src.max()), int(self.dst.max())) >= self.count:
            raise ValueError('flat graph endpoint outside node directory')
        order = np.argsort(self.dst, kind='stable')
        self.in_edge = frozen(order.astype(np.int64))
        self.in_ptr = frozen(np.r_[0, np.cumsum(np.bincount(self.dst, minlength=self.count))].astype(np.int64))
        order = np.argsort(self.src, kind='stable')
        self.out_edge = frozen(order.astype(np.int64))
        self.out_ptr = frozen(np.r_[0, np.cumsum(np.bincount(self.src, minlength=self.count))].astype(np.int64))
        # max(1, sum |strength| of incoming) per node — fixed for the event, as in the engine.
        mass = np.bincount(self.dst, weights=np.abs(self.strength.astype(np.float64)), minlength=self.count)
        self.den = frozen(np.maximum(1.0, mass), np.float64)

    def __reduce__(self):
        # Rebuild through __init__ so unpickled arrays are frozen again and CSR views match.
        return (FlatGraph, (self.direct, self.score, self.unresolved, self.src, self.dst,
                            self.sign, self.strength))

    # ── construction helpers ────────────────────────────────────────────
    @classmethod
    def empty(cls):
        z = np.empty(0, dtype=np.float32)
        return cls(z, z, np.empty(0, dtype=bool), np.empty(0, dtype=np.uint32),
                   np.empty(0, dtype=np.uint32), np.empty(0, dtype=np.int8), z)

    def extend(self, *, new_direct, new_src, new_dst, new_sign, new_strength,
               strength_updates=(), score=None):
        """Successor with appended nodes/edges, optional strength edits and a new score vector."""
        strength = self.strength.copy()
        for edge, value in strength_updates:
            strength[edge] = value
        return FlatGraph(
            np.concatenate([self.direct, np.asarray(new_direct, np.float32)]),
            np.concatenate([self.score if score is None else score, np.zeros(len(new_direct), np.float32)]),
            np.concatenate([self.unresolved, np.ones(len(new_direct), bool)]),
            np.concatenate([self.src, np.asarray(new_src, np.uint32)]),
            np.concatenate([self.dst, np.asarray(new_dst, np.uint32)]),
            np.concatenate([self.sign, np.asarray(new_sign, np.int8)]),
            np.concatenate([strength, np.asarray(new_strength, np.float32)]))

    def with_score(self, score):
        g = object.__new__(FlatGraph)
        for name in self.__slots__:
            object.__setattr__(g, name, getattr(self, name))
        object.__setattr__(g, 'score', frozen(score, np.float32))
        return g

    def outgoing_targets(self, node):
        lo, hi = self.out_ptr[node], self.out_ptr[node + 1]
        return self.dst[self.out_edge[lo:hi]]

    def incoming_edges(self, node):
        lo, hi = self.in_ptr[node], self.in_ptr[node + 1]
        return self.in_edge[lo:hi]


def _gather_ranges(ptr, nodes):
    """Concatenated index ranges ptr[n]:ptr[n+1] for each n in nodes, plus segment ids."""
    starts, stops = ptr[nodes], ptr[nodes + 1]
    counts = stops - starts
    total = int(counts.sum())
    if total == 0:
        return np.empty(0, np.int64), np.empty(0, np.int64), counts
    segment = np.repeat(np.arange(len(nodes)), counts)
    offsets = np.arange(total) - np.repeat(np.cumsum(counts) - counts, counts)
    return np.repeat(starts, counts) + offsets, segment, counts


def settle(graph, seeds, *, maximum_rounds=512, dense_threshold=0.0):
    """Settle the event signal from ``seeds``; returns (score, receipt_fields).

    ``score`` is the settled float32 vector (a full vector, not a delta). The receipt
    fields mirror the engine's proposal receipt: rounds, node/edge evaluations, pending.

    Two equivalent vehicles per round: without SciPy (or below ``dense_threshold``),
    gather only the pending nodes' incoming edges; otherwise (the default) compute
    the incoming signal for every node with one sparse matrix-vector product and apply
    the result to pending nodes only. Non-pending nodes are never updated either way,
    exactly as in the engine.
    """
    count = graph.count
    score = graph.score.astype(np.float64)          # pre-round values, float64 for arithmetic
    initial = graph.score                            # to count changed nodes for the receipt
    direct = graph.direct.astype(np.float64)
    weight = graph.sign.astype(np.float64) * graph.strength.astype(np.float64)
    src = graph.src.astype(np.int64)
    dst = graph.dst.astype(np.int64)
    matrix = adjacency = None
    if csr_matrix is not None and len(src):
        matrix = csr_matrix((weight, (dst, src)), shape=(count, count))   # row = target
        adjacency = csr_matrix((np.ones(len(src)), (dst, src)), shape=(count, count))  # who feeds whom
    pending = np.zeros(count, dtype=bool)
    pending[np.asarray(seeds, np.int64)] = True
    in_degree = np.diff(graph.in_ptr)
    rounds = node_evals = edge_evals = 0
    for _ in range(maximum_rounds):
        nodes = np.flatnonzero(pending)
        if len(nodes) == 0:
            break
        if matrix is not None and len(nodes) > dense_threshold * count:
            signal = matrix @ score                   # every node; applied to pending only
            nxt_all = (0.8 * score + 0.2 * np.tanh(direct + 0.2 * signal / graph.den)).astype(np.float32)
            nxt = nxt_all[nodes]
        else:
            edges, segment, _ = _gather_ranges(graph.in_ptr, nodes)
            edge_ids = graph.in_edge[edges]
            terms = score[src[edge_ids]] * weight[edge_ids]
            signal = np.bincount(segment, weights=terms, minlength=len(nodes))
            nxt = (0.8 * score[nodes] + 0.2 * np.tanh(direct[nodes] + 0.2 * signal / graph.den[nodes])).astype(np.float32)
        old = score[nodes].astype(np.float32)
        changed = nxt.view(np.uint32) != old.view(np.uint32)
        rounds += 1
        node_evals += len(nodes)
        edge_evals += int(in_degree[nodes].sum())
        score[nodes] = nxt.astype(np.float64)
        moved = nodes[changed]
        pending = np.zeros(count, dtype=bool)
        if len(moved) == 0:
            break
        pending[moved] = True
        if adjacency is not None:
            moved_mask = np.zeros(count); moved_mask[moved] = 1.0
            pending |= (adjacency @ moved_mask) > 0        # targets of every moved node
        else:
            targets, _, _ = _gather_ranges(graph.out_ptr, moved)
            pending[graph.dst[graph.out_edge[targets]]] = True
    remaining = np.flatnonzero(pending)
    settled = score.astype(np.float32)
    changed_total = int(np.count_nonzero(settled.view(np.uint32) != initial.view(np.uint32)))
    return settled, dict(version=VERSION, status='pending' if len(remaining) else 'signal_fixed_point',
                         pending_node_count=int(len(remaining)), rounds=rounds, node_evaluations=node_evals,
                         edge_evaluations=edge_evals, changed_scores=changed_total,
                         pending_nodes=tuple(int(n) for n in remaining))
