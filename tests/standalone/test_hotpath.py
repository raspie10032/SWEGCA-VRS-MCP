# -*- coding: utf-8 -*-
"""The per-ingest hot path (2026-09-21): no sort where the order is known — and the same bits as the sorts."""
import numpy as np

from swegca_vrs2 import fast_regions
from swegca_vrs2.engine.mosaic_vrs_connectivity_regions import _csr
from swegca_vrs2.flat_vrs import FlatGraph, merged_order


def test_merged_order_equals_a_stable_argsort_of_the_grown_keys():
    rng = np.random.default_rng(1)
    for n, m, span in ((0, 5, 3), (1, 1, 1), (50, 7, 4), (5000, 60, 300), (20000, 1, 20000), (3000, 500, 2)):
        keys = rng.integers(0, span, n).astype(np.uint32)
        new = rng.integers(0, span, m).astype(np.uint32)
        order = np.argsort(keys, kind='stable')
        got = merged_order(keys, order, new, n)
        want = np.argsort(np.concatenate([keys, new]), kind='stable')
        assert np.array_equal(got, want), (n, m, span)
    assert np.array_equal(merged_order(np.arange(4, dtype=np.uint32), np.arange(4), np.empty(0, np.uint32), 4), np.arange(4))


def test_an_extended_flat_graph_has_the_csr_views_a_rebuilt_one_has():
    rng = np.random.default_rng(2)
    n, e = 400, 3000
    g = FlatGraph(rng.random(n).astype(np.float32), np.zeros(n, np.float32), np.ones(n, bool),
                  rng.integers(0, n, e).astype(np.uint32), rng.integers(0, n, e).astype(np.uint32),
                  rng.choice([-1, 1], e).astype(np.int8), rng.random(e).astype(np.float32))
    for _ in range(4):
        k, m = 3, 40
        new_src = rng.integers(0, n + k, m); new_dst = rng.integers(0, n + k, m)
        grown = g.extend(new_direct=np.zeros(k, np.float32), new_src=new_src, new_dst=new_dst,
                         new_sign=np.ones(m, np.int8), new_strength=[0.5] * m,
                         strength_updates=[(0, 0.25)], node_updates=[(1, 0.0, True)])
        rebuilt = FlatGraph(grown.direct, grown.score, grown.unresolved, grown.src, grown.dst, grown.sign, grown.strength)
        for name in ('in_edge', 'in_ptr', 'out_edge', 'out_ptr', 'den'):
            assert np.array_equal(getattr(grown, name), getattr(rebuilt, name)), name
        n += k; g = grown
    again = g.with_arrays(strength=(g.strength * 2).astype(np.float32))
    rebuilt = FlatGraph(again.direct, again.score, again.unresolved, again.src, again.dst, again.sign, again.strength)
    assert np.array_equal(again.in_edge, rebuilt.in_edge) and np.array_equal(again.den, rebuilt.den)


def test_contracted_csr_matches_the_engine_csr_and_falls_back_when_it_must():
    rng = np.random.default_rng(3)
    for size, count in ((3, 10), (130, 200_000), (60, 5000)):
        rows = rng.integers(0, size, count); cols = rng.integers(0, size, count)
        w = rng.integers(1, 4, count).astype(np.float64)
        a = _csr(rows, cols, w, size); b = fast_regions._contracted_csr(rows, cols, w, size)
        assert all(np.array_equal(x, y) for x, y in zip(a, b)) and all(x.dtype == y.dtype for x, y in zip(a, b)), size
    # fractional weights: the summation order matters, so the engine path is used (and agrees with itself)
    rows = rng.integers(0, 9, 500); cols = rng.integers(0, 9, 500); w = rng.random(500)
    a = _csr(rows, cols, w, 9); b = fast_regions._contracted_csr(rows, cols, w, 9)
    assert all(np.array_equal(x, y) for x, y in zip(a, b))
    # an empty level
    e = np.empty(0, np.int64)
    assert [len(x) for x in fast_regions._contracted_csr(e, e, np.empty(0), 4)] == [5, 0, 0]


def test_row_grouped_lexsort_equals_numpy_lexsort():
    rng = np.random.default_rng(4)
    for n, size, count in ((10, 3, 40), (5000, 130, 300_000), (1, 1, 5)):
        r = np.sort(rng.integers(0, n, count)); g = rng.integers(0, size, count)
        assert np.array_equal(fast_regions._lexsort_rows(r, g, size), np.lexsort((g, r)))
    assert len(fast_regions._lexsort_rows(np.empty(0, np.int64), np.empty(0, np.int64), 3)) == 0


def test_memberships_unchanged_against_the_lexsort_form():
    rng = np.random.default_rng(5)
    size, count = 3000, 60_000
    rows = np.sort(rng.integers(0, size, count)); nbr = rng.integers(0, size, count)
    offsets = np.r_[0, np.cumsum(np.bincount(rows, minlength=size))].astype(np.int64)
    weights = rng.choice([0.0, 1.0, 2.0], count)
    core = rng.integers(0, 40, size)
    got = fast_regions._memberships(offsets, nbr, weights, core)
    # the previous form, verbatim
    r = np.repeat(np.arange(size), np.diff(offsets)); g = core[nbr]; sel = weights > 0
    r, g, w = r[sel], g[sel], weights[sel]
    order = np.lexsort((g, r)); r, g, w = r[order], g[order], w[order]
    start = np.r_[True, (r[1:] != r[:-1]) | (g[1:] != g[:-1])]; pos = np.flatnonzero(start)
    r, g, masses = r[pos], g[pos], np.add.reduceat(w, pos)
    counts = np.bincount(r, minlength=size); lonely = np.flatnonzero(counts == 0)
    if len(lonely):
        r = np.concatenate([r, lonely]); g = np.concatenate([g, core[lonely]]); masses = np.concatenate([masses, np.ones(len(lonely))])
        order = np.lexsort((g, r)); r, g, masses = r[order], g[order], masses[order]; counts = np.bincount(r, minlength=size)
    totals = np.bincount(r, weights=masses, minlength=size)
    want = (np.r_[0, np.cumsum(counts)].astype(np.int64), g.astype(np.int64), (masses / totals[r]).astype(np.float64))
    assert all(np.array_equal(x, y) for x, y in zip(got, want))


def test_the_csr_cache_extends_with_the_fresh_builds_unit_weights():
    """2026-09-21: the cache weighed a new pair 1.0 (flat strengths) where the fresh build weighs it 2.0 (unit per
    edge) — regions after a restart differed from regions before it. Now the extension equals the fresh unit-weight
    CSR bit for bit (the live check is ``VRS2_VERIFY_CSR=1`` on an ingest)."""
    from swegca_vrs2 import csr_cache
    from swegca_vrs2.engine.mosaic_vrs_connectivity_regions import _csr as engine_csr
    rng = np.random.default_rng(6)
    n, e = 300, 2000
    g = FlatGraph(np.zeros(n, np.float32), np.zeros(n, np.float32), np.ones(n, bool),
                  rng.integers(0, n, e).astype(np.uint32), rng.integers(0, n, e).astype(np.uint32),
                  np.ones(e, np.int8), np.full(e, 0.5, np.float32))
    u = g.src.astype(np.int64); v = g.dst.astype(np.int64); ones = np.ones(e)
    o, nb, w = engine_csr(np.r_[u, v], np.r_[v, u], np.r_[ones, ones], n)
    cache = (0, n, e, o, nb, w)
    m = 30
    grown = g.extend(new_direct=np.zeros(2, np.float32), new_src=np.r_[np.full(m, n), rng.integers(0, n, m)][:m],
                     new_dst=np.r_[rng.integers(0, n, m // 2), np.full(m - m // 2, n + 1)], new_sign=np.ones(m, np.int8),
                     new_strength=[0.5] * m)
    got = csr_cache.extended(cache, grown, e, [])
    csr_cache.verify(*got, grown, engine_csr)                      # raises when the bits differ
    w = got[2]
    assert np.all(w >= 1.0) and np.all(w == np.floor(w))            # unit per edge: integral counts, never .5 steps
    assert len(w) > len(cache[5]) and np.all(w[len(cache[5]):] >= 1.0)
