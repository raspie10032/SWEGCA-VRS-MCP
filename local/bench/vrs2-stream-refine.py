# -*- coding: utf-8 -*-
"""A refinement cycle over a VRS too large for RAM — the 10^9-parameter measurement of 2.2 (2026-09-21).

The kernel is ``vrs_refine.refine``'s one cycle (aggregate the signed, weighted source states into each target,
update the state, then reinforce x1.01 / weaken x0.995 and clamp), run **streaming** over the FlatGraph's in-edge
layout: edges are CSR by target (``indptr`` in RAM, ``src`` and a *signed* ``strength`` on the SSD — no dst column,
no sign column), each chunk owns a contiguous range of targets, so 16 threads scatter into disjoint node slices,
and every thread reads its chunk into its own fixed buffers with plain file reads (not memmaps: mapped pages land
in the process working set and made the first run look like 8.9 GB — 2026-09-21 17:19) and writes the new
strengths back the same way. The node arrays (state, direct, degree, aggregate, unresolved) stay in RAM; the state
update runs in slices so no node-sized temporary is ever allocated. Bytes per cycle: 8 B/edge (pass 1) + 12 B/edge
(pass 2) = 20 B/edge — 18.6 GB at 10^9, against the 5 Gbps SSD ceiling that is a 30 s floor.

    python vrs2-stream-refine.py --edges 1e9 --nodes 1e8 --dir D:/vrs-stream --threads 16 --chunk 1e6
    python vrs2-stream-refine.py --edges 1e8 --nodes 1e7 ...        (a dry size: 0.8 GB on disk)

Prints: generation time, the cycle's wall time per pass (aggregate / state / update), edges per second, bytes read
and written, peak working set and peak private commit — and says plainly what bound it: the SSD (bytes / time) or
the gather (edges / time). Nothing here touches a real store; the arrays are synthetic (uniform random edges, BASE
strengths with random signs).
"""
import argparse
import os
import sys
import threading
import time
from concurrent.futures import ThreadPoolExecutor

import numpy as np

BASE, REINFORCE, WEAKEN = np.float32(0.75), np.float32(1.01), np.float32(0.995)
SLICE = 1 << 23                                   # node-array slice for the state update (32 MB per float32 temp)


def memory_mb():
    """(peak working set, peak private commit, working set now) in MB — Windows; NaN elsewhere."""
    try:
        import ctypes, ctypes.wintypes as wt
        class PMC(ctypes.Structure):
            _fields_ = [("cb", wt.DWORD), ("PageFaultCount", wt.DWORD), ("PeakWorkingSetSize", ctypes.c_size_t),
                        ("WorkingSetSize", ctypes.c_size_t), ("QuotaPeakPagedPoolUsage", ctypes.c_size_t),
                        ("QuotaPagedPoolUsage", ctypes.c_size_t), ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t),
                        ("QuotaNonPagedPoolUsage", ctypes.c_size_t), ("PagefileUsage", ctypes.c_size_t), ("PeakPagefileUsage", ctypes.c_size_t)]
        pmc = PMC(); pmc.cb = ctypes.sizeof(PMC)
        k32, psapi = ctypes.windll.kernel32, ctypes.windll.psapi
        k32.GetCurrentProcess.restype = ctypes.c_void_p
        psapi.GetProcessMemoryInfo.argtypes = [ctypes.c_void_p, ctypes.POINTER(PMC), wt.DWORD]
        if not psapi.GetProcessMemoryInfo(k32.GetCurrentProcess(), ctypes.byref(pmc), pmc.cb):
            return float("nan"), float("nan"), float("nan")
        return pmc.PeakWorkingSetSize / 2**20, pmc.PeakPagefileUsage / 2**20, pmc.WorkingSetSize / 2**20
    except Exception:
        return float("nan"), float("nan"), float("nan")


def generate(directory, e, n, chunk, seed=0):
    """CSR by target: ``indptr.bin`` (int64, n+1), ``src.bin`` (uint32, e), ``strength.bin`` (float32, e; sign folded)."""
    os.makedirs(directory, exist_ok=True)
    paths = {name: os.path.join(directory, name + ".bin") for name in ("indptr", "src", "strength")}
    if all(os.path.isfile(p) for p in paths.values()) and os.path.getsize(paths["src"]) == 4 * e:
        return paths, 0.0
    t0 = time.time()
    rng = np.random.default_rng(seed)
    degree = np.zeros(n, np.int64)
    chunks = (e + chunk - 1) // chunk
    for k in range(chunks):                                   # targets uniform within the chunk's node range
        a, b = k * chunk, min(e, (k + 1) * chunk)
        lo, hi = int(n * a / e), max(int(n * a / e) + 1, int(n * b / e))
        degree[lo:hi] += np.bincount(rng.integers(lo, hi, b - a) - lo, minlength=hi - lo)
    indptr = np.zeros(n + 1, np.int64)
    np.cumsum(degree, out=indptr[1:])
    del degree
    indptr.tofile(paths["indptr"])
    with open(paths["src"], "wb") as fs, open(paths["strength"], "wb") as fw:
        for k in range(chunks):
            a, b = k * chunk, min(e, (k + 1) * chunk)
            fs.write(rng.integers(0, n, b - a, dtype=np.uint32).tobytes())
            fw.write((rng.choice(np.array([-1, 1], np.float32), b - a) * BASE).tobytes())
    return paths, time.time() - t0


class Reader:
    """Per-thread file handles and fixed buffers: the working set is bounded by threads x chunk x (4 + 4 + temps)."""
    def __init__(self, paths, chunk):
        self.paths, self.chunk = paths, chunk
        self.local = threading.local()

    def handles(self):
        h = getattr(self.local, "h", None)
        if h is None:
            h = self.local.h = dict(
                src=open(self.paths["src"], "rb", buffering=0), w=open(self.paths["strength"], "r+b", buffering=0),
                bsrc=np.empty(self.chunk, np.uint32), bw=np.empty(self.chunk, np.float32))
        return h

    def read(self, a, b):
        h = self.handles()
        m = b - a
        for name, buf, size in (("src", h["bsrc"], 4), ("w", h["bw"], 4)):
            f = h[name]; f.seek(a * size)
            view = memoryview(buf)[:m].cast("B")
            got = 0
            while got < len(view):
                r = f.readinto(view[got:])
                if not r:
                    raise EOFError(name)
                got += r
        return h["bsrc"][:m], h["bw"][:m]

    def write(self, a, values):
        f = self.handles()["w"]; f.seek(a * 4); f.write(values.tobytes())

    def close(self):
        pass                                                  # handles die with their threads


def cycle(paths, e, n, chunk, threads, seed=1):
    """One refinement cycle, streaming. Returns timings and counters."""
    indptr = np.fromfile(paths["indptr"], np.int64)
    assert len(indptr) == n + 1 and indptr[-1] == e
    rng = np.random.default_rng(seed)
    direct = np.empty(n, np.float32)
    for s in range(0, n, SLICE):
        direct[s:s + SLICE] = np.tanh(rng.standard_normal(min(SLICE, n - s)).astype(np.float32))
    unresolved = np.empty(n, bool)
    for s in range(0, n, SLICE):
        unresolved[s:s + SLICE] = rng.random(min(SLICE, n - s)) < 0.1
    state = direct.copy()
    degree = np.zeros(n, np.float32)
    agg = np.zeros(n, np.float32)
    # chunk boundaries on node boundaries, ~chunk edges each: the edge range of targets [lo, hi) is [indptr[lo], indptr[hi])
    cuts = np.searchsorted(indptr, np.arange(0, e + chunk, chunk), side="left")
    cuts = np.unique(np.clip(cuts, 0, n))
    if cuts[-1] != n:
        cuts = np.append(cuts, n)
    ranges = [(int(cuts[i]), int(cuts[i + 1])) for i in range(len(cuts) - 1) if cuts[i + 1] > cuts[i]]
    widest = max(int(indptr[hi] - indptr[lo]) for lo, hi in ranges)
    reader = Reader(paths, widest)

    # pass 1: degree (|strength| per target) and the aggregate of signed weighted source states, per target range
    def aggregate(lohi):
        lo, hi = lohi
        a, b = int(indptr[lo]), int(indptr[hi])
        src, w = reader.read(a, b)
        t = np.repeat(np.arange(hi - lo, dtype=np.int64), np.diff(indptr[lo:hi + 1]))
        s = state[src]                                        # the random gather
        degree[lo:hi] = np.bincount(t, weights=np.abs(w), minlength=hi - lo).astype(np.float32)
        agg[lo:hi] = np.bincount(t, weights=s * w, minlength=hi - lo).astype(np.float32)
        return b - a

    t0 = time.perf_counter()
    with ThreadPoolExecutor(threads) as pool:
        done = sum(pool.map(aggregate, ranges))
    t_agg = time.perf_counter() - t0
    # state update (node-sized, in RAM, sliced so the temporaries stay at SLICE)
    t0 = time.perf_counter()
    for s in range(0, n, SLICE):
        sl = slice(s, s + SLICE)
        d = np.maximum(degree[sl], np.float32(1.0))
        candidate = np.tanh(direct[sl] + np.float32(0.2) * agg[sl] / d)
        state[sl] = np.float32(0.8) * state[sl] + np.float32(0.2) * candidate
    t_state = time.perf_counter() - t0

    # pass 2: compatibility per edge -> reinforce / weaken / clamp, written back to the strength file
    lo_c, hi_c = BASE * np.float32(0.25), BASE * np.float32(4.0)
    reinforced = np.zeros(len(ranges), np.int64)

    def update(k_lohi):
        k, (lo, hi) = k_lohi
        a, b = int(indptr[lo]), int(indptr[hi])
        src, w = reader.read(a, b)
        t = np.repeat(np.arange(lo, hi, dtype=np.int64), np.diff(indptr[lo:hi + 1]))
        s_src = state[src]                                    # the random gather, again
        s_dst = state[t]                                      # contiguous targets: a slice-like gather
        sg = np.sign(w)
        compatibility = np.float32(1.0) - np.float32(0.5) * np.abs(s_src * sg - s_dst)
        informed = (np.abs(s_src) + np.abs(s_dst)) >= np.float32(0.1)
        blocked = unresolved[src] | unresolved[t]
        stable = (compatibility >= np.float32(0.75)) & informed & ~blocked
        mag = np.abs(w) * np.where(stable, REINFORCE, WEAKEN)
        reader.write(a, np.maximum(np.minimum(mag, hi_c), lo_c) * sg)
        reinforced[k] = int(stable.sum())
        return b - a

    t0 = time.perf_counter()
    with ThreadPoolExecutor(threads) as pool:
        done2 = sum(pool.map(update, enumerate(ranges)))
    t_upd = time.perf_counter() - t0
    assert done == done2 == e
    return dict(aggregate_s=t_agg, state_s=t_state, update_s=t_upd, reinforced=int(reinforced.sum()),
                bytes_read=e * 8 * 2, bytes_written=e * 4, chunks=len(ranges), widest=widest)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--edges", type=float, default=1e8)
    ap.add_argument("--nodes", type=float, default=1e7)
    ap.add_argument("--dir", default=os.path.join(os.environ.get("TEMP", "."), "vrs-stream"))
    ap.add_argument("--threads", type=int, default=16)
    ap.add_argument("--chunk", type=float, default=1e6)
    ap.add_argument("--cycles", type=int, default=1)
    ap.add_argument("--keep", action="store_true", help="keep the generated arrays on disk")
    a = ap.parse_args()
    e, n, chunk = int(a.edges), int(a.nodes), int(a.chunk)
    print(f"edges {e:,} nodes {n:,} chunk {chunk:,} threads {a.threads} | on disk {(e * 8 + (n + 1) * 8) / 2**30:.1f} GB, "
          f"node arrays in RAM {((n + 1) * 8 + n * 4 * 4 + n) / 2**30:.2f} GB (indptr + state/direct/degree/agg + unresolved)")
    paths, gen = generate(a.dir, e, n, chunk)
    print(f"generated in {gen:.1f} s" if gen else "arrays present")
    for c in range(a.cycles):
        r = cycle(paths, e, n, chunk, a.threads, seed=1 + c)
        total = r["aggregate_s"] + r["state_s"] + r["update_s"]
        peak_ws, peak_private, now = memory_mb()
        print(f"cycle {c + 1}: {total:.1f} s (aggregate {r['aggregate_s']:.1f} + state {r['state_s']:.2f} + update {r['update_s']:.1f}) | "
              f"{e / total / 1e6:.0f} M edges/s | read {r['bytes_read'] / 2**30:.1f} GB written {r['bytes_written'] / 2**30:.1f} GB "
              f"-> {(r['bytes_read'] + r['bytes_written']) / 2**30 / total:.2f} GB/s | reinforced {r['reinforced']:,} | "
              f"chunks {r['chunks']} (widest {r['widest']:,} edges) | peak working set {peak_ws:.0f} MB, peak private {peak_private:.0f} MB")
    if not a.keep:
        for p in paths.values():
            try:
                os.remove(p)
            except OSError:
                pass


if __name__ == "__main__":
    main()
