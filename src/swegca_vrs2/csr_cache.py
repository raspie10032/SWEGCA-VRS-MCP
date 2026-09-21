# -*- coding: utf-8 -*-
"""Incremental level-0 adjacency for connectivity regions (local Windows-scale adapter, 2026-09-14).

``fast_regions.build_regions`` starts from the engine's ``_csr`` of the whole changed component:
``rows = [u, v]``, ``cols = [v, u]``, weights ``[s, s]`` for every flat edge, sorted by
(row, col, weight) with parallel entries summed. At 2,700 records the changed component is the
whole graph (~0.8M flat edges), and that lexsort was the largest single cost of an ingest.

Edges are append-only and a new record's node id is larger than every existing id, so the
same array can be extended instead of rebuilt: the new record's row is appended, and each
existing endpoint row gains one entry whose column (the new record) is larger than any column
already in that row — sorted order is preserved without a sort. Strength edits (explicit
same-proposition re-evidence) change a pair's summed weight; the pair is recomputed from the
two flat edges' strengths, which is the same two-operand sum ``_csr`` produces.

The cache is a plain tuple ``(component_id, node_count, edge_count, offsets, neighbors,
weights)``; ``extended()`` returns the arrays a fresh ``_csr`` would return, bit for bit
(``VRS2_VERIFY_CSR=1`` asserts that on every ingest). It is never persisted: the first
ingest after a restart rebuilds it once through ``_csr``. Weights are unit weights per flat edge, as the fresh
build's (2026-09-21; before that the cache used the flat strengths and disagreed with the fresh build).
"""
import os

import numpy as np

VERIFY = os.environ.get('VRS2_VERIFY_CSR') == '1'


def _grouped(rows, cols, weights):
    """Batch entries in ``_csr`` order with parallel entries summed (same arithmetic)."""
    order = np.lexsort((weights, cols, rows))
    rows, cols, weights = rows[order], cols[order], weights[order]
    start = np.r_[True, (rows[1:] != rows[:-1]) | (cols[1:] != cols[:-1])]
    positions = np.flatnonzero(start)
    return rows[positions], cols[positions], np.add.reduceat(weights, positions)


def _pair_weight(flat, u, v):
    """Summed weight of CSR entry (u, v): strengths of flat edges u->v and v->u in _csr's order."""
    values = []
    for a, b in ((u, v), (v, u)):
        lo, hi = int(flat.out_ptr[a]), int(flat.out_ptr[a + 1])
        edges = flat.out_edge[lo:hi]
        hit = edges[flat.dst[edges] == b]
        values.extend(float(flat.strength[e]) for e in hit)
    values.sort()
    total = 0.0
    for x in values:              # reduceat over weight-sorted contributions
        total = total + x
    return total


def extended(cache, flat, new_edge_start, edits):
    """CSR of the whole graph ``flat`` given the cache of its prefix (``new_edge_start`` edges).

    ``edits`` are ``(edge_index, new_value)`` strength edits already applied in ``flat``.
    Returns ``(offsets, neighbors, weights)`` as int64/int64/float64 arrays.
    """
    _, node_count, edge_count, offsets, neighbors, weights = cache
    assert edge_count == new_edge_start
    total_nodes = flat.count
    src = flat.src[new_edge_start:].astype(np.int64)
    dst = flat.dst[new_edge_start:].astype(np.int64)
    # unit weights: regions are navigation topology, built in append_many from ``np.ones`` per flat edge — the
    # cache must extend with the same (until 2026-09-21 it used the flat strengths, BASE .5 per edge, so a pair
    # weighed 1.0 when appended and 2.0 after the next restart's fresh build)
    strength = np.ones(len(src), dtype=np.float64)
    if not len(src):                       # a record without cues: only empty rows are appended
        rows = cols = np.empty(0, dtype=np.int64); values = np.empty(0, dtype=np.float64)
    else:
        rows, cols, values = _grouped(np.r_[src, dst], np.r_[dst, src], np.r_[strength, strength])
    old_counts = np.zeros(total_nodes, dtype=np.int64)
    old_counts[:node_count] = np.diff(offsets)
    new_counts = np.bincount(rows, minlength=total_nodes)
    new_offsets = np.r_[0, np.cumsum(old_counts + new_counts)].astype(np.int64)
    out_neighbors = np.empty(len(neighbors) + len(rows), dtype=np.int64)
    out_weights = np.empty(len(out_neighbors), dtype=np.float64)
    # old entries keep their order; each shifts by the number of new entries in earlier rows
    shift = np.r_[0, np.cumsum(new_counts)][:-1]
    old_rows = np.repeat(np.arange(node_count), old_counts[:node_count])
    old_positions = np.arange(len(neighbors)) + shift[old_rows]
    out_neighbors[old_positions] = neighbors
    out_weights[old_positions] = weights
    # new entries follow the old entries of their row, in batch order (rows, then cols)
    first = np.r_[0, np.cumsum(new_counts)]
    within = np.arange(len(rows)) - first[rows]
    new_positions = new_offsets[rows] + old_counts[rows] + within
    out_neighbors[new_positions] = cols
    out_weights[new_positions] = values
    # sorted-column invariant: every new column in an existing row is the new record's id or a
    # fresh cue id, both >= node_count > any column already present in that row
    if len(rows) and node_count and (cols[rows < node_count] < node_count).any():
        raise ValueError('csr_cache_order_invariant_violated')
    if edits:
        # a strength edit does not touch unit weights; the pair must still exist (the invariant the edit assumes)
        for edge, _ in edits:
            u, v = int(flat.src[edge]), int(flat.dst[edge])
            for a, b in ((u, v), (v, u)):
                lo, hi = int(new_offsets[a]), int(new_offsets[a + 1])
                k = lo + int(np.searchsorted(out_neighbors[lo:hi], b))
                if k >= hi or out_neighbors[k] != b:
                    raise ValueError('csr_cache_edit_target_missing')
    return new_offsets, out_neighbors, out_weights


def verify(offsets, neighbors, weights, flat, csr):
    """Compare with a fresh engine _csr of the whole graph (debug aid, VRS2_VERIFY_CSR=1)."""
    u = flat.src.astype(np.int64); v = flat.dst.astype(np.int64); s = np.ones(len(u), dtype=np.float64)
    o, n, w = csr(np.r_[u, v], np.r_[v, u], np.r_[s, s], flat.count)
    if not (np.array_equal(o, offsets) and np.array_equal(n, neighbors)
            and np.array_equal(w.view(np.uint64), weights.view(np.uint64))):
        raise AssertionError('incremental CSR differs from engine _csr')
