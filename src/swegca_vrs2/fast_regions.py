# -*- coding: utf-8 -*-
"""Connectivity regions for large components — same rule, vectorized, warm-started.

Local Windows-scale adapter (2026-09-14). ``engine.mosaic_vrs_connectivity_regions``
derives navigation regions by weighted-modularity local moves (Louvain style) over the
changed connected component, on every ingress, one node at a time in Python. With real
records the component is the whole graph, so each ingress re-clusters everything.

This module builds the identical ``ConnectivityRegions`` object (same fields, same
topology digest formula, same membership definition) with three changes in *how*:

* local moves are evaluated for many nodes at once (even/odd halves per sweep so two
  neighbours cannot swap forever), each move using the same modularity gain
  ``into[g] - degree*totals[g]/mass`` and the same tolerance;
* when the active frontier is small the sweep finishes with the engine's own sequential
  rule, which is monotone and therefore terminates like the original;
* level-0 labels start from the previous generation's cores for nodes that already
  existed (warm start). New nodes start alone, exactly as the cold build does.

Small components (``SMALL`` nodes or fewer) use the engine's ``ConnectivityRegions.build``
unchanged. Membership coefficients are normalized with NumPy sums instead of
``math.fsum``; the difference is at most one ulp per coefficient.
"""
from __future__ import annotations

import hashlib
from types import SimpleNamespace

import numpy as np

from .engine.mosaic_vrs_connectivity_regions import ConnectivityRegions, _csr, _canonical, _frozen
from .engine.mosaic_vrs_region_arrays import reverse_memberships

SMALL = 2000          # components up to this many nodes: engine build, untouched
SEQUENTIAL_TAIL = 512 # active nodes at or below this: go straight to the sequential rule
SYNCHRONOUS_SWEEPS = 12  # bulk synchronous sweeps before the sequential finish


def _degree(offsets, weights):
    size = len(offsets) - 1
    rows = np.repeat(np.arange(size), np.diff(offsets))
    return np.bincount(rows, weights=weights, minlength=size), rows


def _vector_moves(offsets, neighbors, weights, maximum_sweeps, initial, fresh=None):
    """Modularity local moves: a few synchronous sweeps, then the engine's sequential rule.

    Synchronous sweeps move many nodes at once and can oscillate; the sequential phase
    is the engine's own rule on the active set, which is monotone in modularity and
    therefore terminates. Returns (labels, converged, sweeps).
    """
    size = len(offsets) - 1
    degree, rows = _degree(offsets, weights)
    mass = float(degree.sum())
    labels = np.arange(size) if initial is None else initial.copy()
    if not mass:
        return labels, True, 0
    tolerance = 64 * np.finfo(float).eps * max(mass, 1.0)
    keep = neighbors != rows                       # no self loops in the gain
    active = degree > 0
    if initial is not None and fresh is not None and fresh.any():
        # warm start: fresh nodes and their neighbours move first; settled nodes wake up
        # only when a neighbour's label changes
        active &= fresh | np.isin(np.arange(size), neighbors[np.isin(rows, np.flatnonzero(fresh))])
    sweeps = 0
    # phase 1 — synchronous bulk moves while they still move a lot
    for _ in range(SYNCHRONOUS_SWEEPS):
        nodes = np.flatnonzero(active)
        if len(nodes) <= SEQUENTIAL_TAIL:
            break
        moved_total = 0
        for parity in (0, 1):
            part = nodes[nodes % 2 == parity]
            moved = _synchronous(part, offsets, neighbors, weights, labels, degree, mass, tolerance, rows, keep)
            if len(moved):
                active[moved] = True
                active[neighbors[np.isin(rows, moved)]] = True
                moved_total += len(moved)
        sweeps += 1
        if moved_total < max(SEQUENTIAL_TAIL, len(nodes) // 100):
            break
    # phase 2 — the engine's sequential rule on the active set until nothing moves
    for _ in range(maximum_sweeps):
        nodes = np.flatnonzero(active)
        if len(nodes) == 0:
            return _canonical(labels), True, sweeps
        active[:] = False
        moved = _sequential(nodes, offsets, neighbors, weights, labels, degree, mass, tolerance)
        sweeps += 1
        if len(moved) == 0:
            return _canonical(labels), True, sweeps
        active[moved] = True
        active[neighbors[np.isin(rows, moved)]] = True
    return _canonical(labels), False, sweeps


def _synchronous(nodes, offsets, neighbors, weights, labels, degree, mass, tolerance, rows, keep):
    totals = np.bincount(labels, weights=degree, minlength=len(labels))
    sel = np.isin(rows, nodes) & keep
    r, g, w = rows[sel], labels[neighbors[sel]], weights[sel]
    if len(r) == 0:
        return np.empty(0, np.int64)
    order = np.lexsort((g, r))
    r, g, w = r[order], g[order], w[order]
    start = np.r_[True, (r[1:] != r[:-1]) | (g[1:] != g[:-1])]
    pos = np.flatnonzero(start)
    r, g = r[pos], g[pos]
    into = np.add.reduceat(w, pos)
    # gain for candidate group g; the node's own degree is removed from its current group first
    own = labels[r]
    adjusted = totals[g] - np.where(g == own, degree[r], 0.0)
    gain = into - degree[r] * adjusted / mass
    # current group's gain (0 if no neighbour there)
    cur_gain = np.zeros(len(labels))
    is_own = g == own
    cur_gain[r[is_own]] = gain[is_own]
    best_gain = np.full(len(labels), -np.inf)
    np.maximum.at(best_gain, r, gain)
    improve = gain >= best_gain[r]              # rows attaining the max for their node
    # smallest group id among maxima (the engine visits groups ascending, first strict winner)
    best_group = np.full(len(labels), -1, np.int64)
    cand_r, cand_g = r[improve], g[improve]
    order2 = np.lexsort((cand_g, cand_r))
    cand_r, cand_g = cand_r[order2], cand_g[order2]
    first = np.r_[True, cand_r[1:] != cand_r[:-1]]
    best_group[cand_r[first]] = cand_g[first]
    changed = np.flatnonzero((best_group >= 0) & (best_gain > cur_gain + tolerance) & (best_group != labels))
    labels[changed] = best_group[changed]
    return changed


def _sequential(nodes, offsets, neighbors, weights, labels, degree, mass, tolerance):
    """The engine's own sequential rule restricted to ``nodes`` (monotone, terminates)."""
    totals = np.bincount(labels, weights=degree, minlength=len(labels))
    moved = []
    for node in nodes:
        lo, hi = offsets[node:node + 2]
        # Keep the CSR edge order while accumulating each neighbour group.
        # Small per-node groups otherwise trigger two NumPy allocations and a
        # sort on every node visit in the sequential tail.
        into = {}
        for edge in range(lo, hi):
            neighbor = int(neighbors[edge])
            if neighbor == node:
                continue
            group = int(labels[neighbor])
            into[group] = into.get(group, 0.0) + float(weights[edge])
        if not into:
            continue
        old = int(labels[node])
        totals[old] -= degree[node]
        score = lambda group: into.get(group, 0.0) - degree[node] * totals[group] / mass
        best, best_score = old, score(old)
        for group in sorted(into):
            candidate = score(group)
            if candidate > best_score + tolerance:
                best, best_score = group, candidate
        totals[best] += degree[node]
        if best != old:
            labels[node] = best
            moved.append(node)
    return np.asarray(moved, np.int64)


def _memberships(offsets, neighbors, weights, core):
    """Vectorized form of the engine's per-node association masses (same definition)."""
    size = len(offsets) - 1
    rows = np.repeat(np.arange(size), np.diff(offsets))
    g = core[neighbors]
    sel = weights > 0
    r, g, w = rows[sel], g[sel], weights[sel]
    order = np.lexsort((g, r))
    r, g, w = r[order], g[order], w[order]
    if len(r):
        start = np.r_[True, (r[1:] != r[:-1]) | (g[1:] != g[:-1])]
        pos = np.flatnonzero(start)
        r, g, masses = r[pos], g[pos], np.add.reduceat(w, pos)
    else:
        masses = np.empty(0)
    counts = np.bincount(r, minlength=size)
    lonely = np.flatnonzero(counts == 0)         # no positive association: own core, weight 1
    if len(lonely):
        r = np.concatenate([r, lonely]); g = np.concatenate([g, core[lonely]])
        masses = np.concatenate([masses, np.ones(len(lonely))])
        order = np.lexsort((g, r))
        r, g, masses = r[order], g[order], masses[order]
        counts = np.bincount(r, minlength=size)
    totals = np.bincount(r, weights=masses, minlength=size)
    coefficients = masses / totals[r]
    member_offsets = np.r_[0, np.cumsum(counts)].astype(np.int64)
    return (_frozen(member_offsets), _frozen(g.astype(np.int64)), _frozen(coefficients.astype(np.float64)))


def build_regions(source, *, vrs_snapshot_id, previous=None, maximum_sweeps=100, maximum_levels=32,
                  csr=None, csr_out=None):
    """``ConnectivityRegions`` for one component; engine build below SMALL nodes.

    ``previous`` is an optional ``(regions, positions)`` pair for the same component in
    the earlier generation; its core labels seed level 0 for nodes that already existed.
    ``csr`` is an optional precomputed level-0 ``(offsets, neighbors, weights)`` in local
    node order (what the engine's ``_csr`` would return); ``csr_out`` (a dict) receives the
    level-0 CSR actually used under key ``'csr'`` so a caller can cache it.
    """
    terms = source.terms
    if len(terms) <= SMALL:
        small = SimpleNamespace(terms=tuple(terms), edge_source=source.edge_source, edge_target=source.edge_target,
                                edge_sign=source.edge_sign, vrs_strength=source.vrs_strength)
        built = ConnectivityRegions.build(small, vrs_snapshot_id=vrs_snapshot_id,
                                          maximum_sweeps=maximum_sweeps, maximum_levels=maximum_levels)
        return built, 'engine_sequential'
    u = np.asarray(source.edge_source, np.int64)
    v = np.asarray(source.edge_target, np.int64)
    sign = np.asarray(source.edge_sign)
    strength = np.asarray(source.vrs_strength, np.float64)
    if csr is not None:
        offsets, neighbors, weights = csr
    else:
        offsets, neighbors, weights = _csr(np.r_[u, v], np.r_[v, u], np.r_[strength, strength], len(terms))
    if csr_out is not None:
        csr_out['csr'] = (offsets, neighbors, weights)
    initial = fresh = None
    if previous is not None:
        old_regions, old_positions = previous
        initial = np.full(len(terms), len(terms) + 1, np.int64)   # sentinel: fresh
        # nodes keep their global ids across generations; previous local positions come
        # from the stored member arrays when available, else from names
        old_members = getattr(old_regions.terms, 'members', None)
        new_members = getattr(terms, 'members', None)
        if old_members is not None and new_members is not None:
            position = {int(n): i for i, n in enumerate(old_members)}
            for i, n in enumerate(new_members):
                j = position.get(int(n))
                if j is not None:
                    initial[i] = int(old_regions.core_labels[j])
        else:
            by_name = {t: i for i, t in enumerate(old_regions.terms)}
            for i, name in enumerate(terms):
                j = by_name.get(name)
                if j is not None:
                    initial[i] = int(old_regions.core_labels[j])
        fresh = initial > len(terms)
        initial[fresh] = np.arange(int(fresh.sum())) + (int(initial[~fresh].max()) + 1 if (~fresh).any() else 0)
    labels = np.arange(len(terms))
    sweeps, converged = [], False
    o, n, w = offsets, neighbors, weights
    for level in range(maximum_levels):
        local, converged, count = _vector_moves(o, n, w, maximum_sweeps,
                                                initial if level == 0 else None, fresh if level == 0 else None)
        sweeps.append(count)
        labels = _canonical(local[labels])
        if not converged:
            break
        size = int(local.max()) + 1 if len(local) else 0
        if size == len(local):
            break
        rows = np.repeat(np.arange(len(local)), np.diff(o))
        o, n, w = _csr(local[rows], local[n], w, size)
    core = labels
    member_offsets, memberships, coefficients = _memberships(offsets, neighbors, weights, core)
    region_terms = reverse_memberships(member_offsets, memberships, int(core.max()) + 1 if len(core) else 0)
    digest = hashlib.sha256(b'vrs-connectivity-modularity-overlap-v1\0')
    digest.update(vrs_snapshot_id.encode('ascii'))
    digest.update(core.astype('<i8').tobytes())
    digest.update(str((maximum_sweeps, maximum_levels, converged)).encode('ascii'))
    # the regions object and its source share one set of frozen edge arrays (no duplicate)
    fu, fv, fsign, fstrength = _frozen(u), _frozen(v), _frozen(sign), _frozen(strength)
    shared_source = SimpleNamespace(terms=terms, edge_source=fu, edge_target=fv, edge_sign=fsign, vrs_strength=fstrength)
    built = ConnectivityRegions(vrs_snapshot_id, digest.hexdigest(), terms, _frozen(core),
                                fu, fv, fsign, fstrength,
                                member_offsets, memberships, coefficients, region_terms,
                                converged, tuple(sweeps), shared_source)
    return built, 'vectorized_warm_start' if previous is not None else 'vectorized_cold'
