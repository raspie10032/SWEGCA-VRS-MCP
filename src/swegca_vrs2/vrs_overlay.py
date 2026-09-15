"""VRS strength refinement over the v2.1 experience graph, region by region (local adapter, 2026-09-15).

Why this exists
---------------
v2.1's store settles only the event *signal* (``settle_event_signal``: scores move, strengths are
fixed) and moves a strength only through explicit same-proposition re-evidence (x1.01 / x0.995).
The original VRS kernel (v0.2 ``refine_vrs``: shuffle cycles over the edges, x1.01 where the two
endpoint states agree, x0.995 otherwise, clamp to base x [.25, 4], promotion at >= 1.0) is not
called anywhere in the native store, so every edge sits at the .5 base and nothing is ever
promoted. This module restores that kernel as an *overlay*: the journal, pair snapshot ids and the
event-signal generation stay byte-identical to upstream; the refined strengths, states and
stabilities live in their own table and are recomputed at idle.

Mapping (v0.2 star graph -> v2.1 experience graph)
--------------------------------------------------
* record node = v0.2 event: ``direct = tanh(1)`` when the record is resolved (outcome success/failure
  and not superseded), else 0 and ``unresolved`` (blocks reinforcement of its edges).
* cue node = generalized hypothesis: ``direct = tanh((support - refute) / max(1, support + refute))``
  over the resolved records touching it (success supports, failure refutes); unresolved when both.
* explicit propositions become virtual hypothesis nodes with the same rule over their members;
  record -> proposition edges carry the evidence polarity as sign.
* only the record -> cue direction is refined (v0.2 edges run event -> hypothesis); the stored
  reverse edge mirrors its pair's strength. Edge sign = +1 (success/pending) / -1 (failure).
* base strength .75 (v0.2), clamp [.1875, 3.0]; promotion at >= 1.0 (``PROMOTION``).

Regions and connectors (the user's design, absent from every upstream version)
-------------------------------------------------------------------------------
The connectivity regions v2.1 already maintains (``core_labels``) are the unit of refinement: each
region's internal edges are refined on their own (degree normalization inside the region), then the
edges that cross regions are refined once more as *connectors* starting from the settled region
states. A connector at >= 1.0 is a trusted bridge between two regions.

Determinism: one ``numpy`` generator seeded per (seed, region) so a rebuild reproduces the overlay.
"""
from __future__ import annotations

import hashlib
import io
import math
import pickle
import time
import zlib

import numpy as np

REINFORCE = np.float32(1.01)
WEAKEN = np.float32(0.995)
BASE = np.float32(0.75)
PROMOTION = 1.0
CYCLES = 16
BATCH = 4096
SEED = 1729
CONNECTOR_LABEL = -1
VERSION = 'vrs-overlay-regions-v1'
TOLERANCE = 1e-3          # v0.2 converge_graph: refine until max |delta| of state and strength is below this
IDLE_CYCLES = 32          # cycles per idle chunk (~3.5 s at 5k records); chunks repeat while not converged


# ── kernel (numpy port of v0.2 refine_vrs; same arithmetic order, float32) ────────────────────

def refine(direct, unresolved, src, dst, sign, strength, base, *, cycles=CYCLES, passes=1,
           batch=BATCH, seed=SEED, state=None):
    """One refinement: ``cycles`` shuffled passes over the edges, then per-cycle strength update.

    Returns (mean_state, stability, strength, evaluations, reinforced_edges). Arrays are float32 like
    the torch original; ``state`` warm-starts from a previous refinement when given.
    """
    n, e = len(direct), len(src)
    direct = np.asarray(direct, np.float32)
    state = direct.copy() if state is None else np.asarray(state, np.float32).copy()
    strength = np.asarray(strength, np.float32).copy()
    base = np.asarray(base, np.float32)
    sign = np.asarray(sign, np.float32)
    src = np.asarray(src, np.int64); dst = np.asarray(dst, np.int64)
    unresolved = np.asarray(unresolved, bool)
    rng = np.random.default_rng(seed)
    consensus = np.zeros(n, np.float32); consensus_sq = np.zeros(n, np.float32)
    reinforced = np.zeros(e, bool)
    lo, hi = base * np.float32(0.25), base * np.float32(4.0)
    batch = max(1, min(e, batch))
    for _cycle in range(cycles):
        order = rng.permutation(e) if e else np.empty(0, np.int64)
        for _pass in range(passes):
            degree = np.bincount(dst, weights=np.abs(strength), minlength=n).astype(np.float32)
            np.maximum(degree, 1.0, out=degree)
            for start in range(0, e, batch):
                sel = order[start:start + batch]
                t = dst[sel]
                agg = np.bincount(t, weights=state[src[sel]] * sign[sel] * strength[sel], minlength=n).astype(np.float32)
                touched = np.unique(t)
                candidate = np.tanh(direct[touched] + np.float32(0.2) * agg[touched] / degree[touched])
                state[touched] = np.float32(0.8) * state[touched] + np.float32(0.2) * candidate
        if e:
            compatibility = np.float32(1.0) - np.float32(0.5) * np.abs(state[src] * sign - state[dst])
            informed = (np.abs(state[src]) + np.abs(state[dst])) >= np.float32(0.1)
            blocked = unresolved[src] | unresolved[dst]
            stable = (compatibility >= np.float32(0.75)) & informed & ~blocked
            reinforced |= stable
            strength = strength * np.where(stable, REINFORCE, WEAKEN)
            strength = np.maximum(np.minimum(strength, hi), lo)
        consensus += state; consensus_sq += state * state
    mean = consensus / np.float32(cycles)
    variance = np.maximum(consensus_sq / np.float32(cycles) - mean * mean, 0.0)
    stability = np.clip(1.0 - np.sqrt(variance), 0.0, 1.0).astype(np.float32)
    return mean.astype(np.float32), stability, strength.astype(np.float32), e * cycles * passes, int(reinforced.sum())


# ── inputs from a live Main ──────────────────────────────────────────────────────────────────

class Inputs:
    """Overlay graph: real nodes (records + cues) followed by virtual proposition nodes; edges are the
    record -> cue half of the flat graph followed by record -> proposition edges."""
    __slots__ = ('node_count', 'real_nodes', 'direct', 'unresolved', 'labels', 'src', 'dst', 'sign', 'base',
                 'flat_edge', 'record_mask', 'propositions', 'resolved_count', 'edge_count_flat')

    def __init__(self, **k):
        for name, value in k.items():
            setattr(self, name, value)


def _resolved(episode, superseded):
    step = episode.steps[0]
    return step.outcome in ('success', 'failure') and episode.episode_id not in superseded


def build_inputs(main, labels=None):
    """Read the current generation (memory + graph + regions) into overlay arrays."""
    graph, memory = main.graph, main.memory
    flat, nodes = graph.flat, graph.nodes
    n = flat.count
    src = np.asarray(flat.src, np.int64); dst = np.asarray(flat.dst, np.int64)
    node_cue = np.asarray(nodes.node_cue)
    record_mask = node_cue < 0
    # v2.1 appends every record's edges as (record -> cue, cue -> record) pairs: even index = forward.
    forward = np.arange(0, len(src), 2)
    if len(src) and not (record_mask[src[forward]].all() and (src[forward] == dst[forward + 1]).all()):
        raise ValueError('flat edge layout is not (record->cue, cue->record) pairs')
    direct = np.zeros(n, np.float32); unresolved = np.ones(n, bool)
    polarity = np.zeros(n, np.int8)                  # per record node: +1 success, -1 failure, 0 pending
    superseded = memory.superseded
    resolved_count = 0
    for node in np.flatnonzero(record_mask):
        episode = memory.episode(nodes.node_episode[int(node)])
        if _resolved(episode, superseded):
            direct[node] = np.tanh(1.0); unresolved[node] = False; resolved_count += 1
            polarity[node] = 1 if episode.steps[0].outcome == 'success' else -1
    # cue nodes as generalized hypotheses: counts of resolved records touching them
    f_src, f_dst = src[forward], dst[forward]
    pol = polarity[f_src]
    support = np.bincount(f_dst[pol > 0], minlength=n); refute = np.bincount(f_dst[pol < 0], minlength=n)
    cue_nodes = ~record_mask
    total = np.maximum(1, support + refute)
    direct[cue_nodes] = np.tanh((support[cue_nodes] - refute[cue_nodes]) / total[cue_nodes]).astype(np.float32)
    unresolved[cue_nodes] = (support[cue_nodes] > 0) & (refute[cue_nodes] > 0)
    sign = np.where(pol < 0, -1, 1).astype(np.int8)
    # virtual proposition nodes
    prop_ids = sorted(memory.propositions)
    p_direct, p_unresolved, p_src, p_dst, p_sign, p_label = [], [], [], [], [], []
    labels = np.asarray(labels) if labels is not None else np.zeros(n, np.int64)
    for k, pid in enumerate(prop_ids):
        pnode = n + k; s = r = 0; first = None
        for eid in sorted(memory.propositions[pid]):
            rnode = nodes.episode_node.get(eid)
            if rnode is None:
                continue
            first = rnode if first is None else first
            episode = memory.episode(eid)
            if not _resolved(episode, superseded):
                p_src.append(rnode); p_dst.append(pnode); p_sign.append(1)
                continue
            supports = episode.steps[0].observation.get('evidence_polarity') != 'refute'
            s += supports; r += (not supports)
            p_src.append(rnode); p_dst.append(pnode); p_sign.append(1 if supports else -1)
        p_direct.append(np.tanh((s - r) / max(1, s + r))); p_unresolved.append(bool(s and r))
        p_label.append(int(labels[first]) if first is not None else 0)   # the proposition sits with its first member's region
    extra = len(prop_ids)
    all_src = np.concatenate([f_src, np.asarray(p_src, np.int64)])
    all_dst = np.concatenate([f_dst, np.asarray(p_dst, np.int64)])
    all_sign = np.concatenate([sign, np.asarray(p_sign, np.int8)])
    flat_edge = np.concatenate([forward, np.full(len(p_src), -1, np.int64)])
    return Inputs(node_count=n + extra, real_nodes=n,
                  direct=np.concatenate([direct, np.asarray(p_direct, np.float32)]),
                  unresolved=np.concatenate([unresolved, np.asarray(p_unresolved, bool)]),
                  labels=np.concatenate([labels[:n], np.asarray(p_label, np.int64)]),
                  src=all_src, dst=all_dst, sign=all_sign, base=np.full(len(all_src), BASE, np.float32),
                  flat_edge=flat_edge, record_mask=np.concatenate([record_mask, np.zeros(extra, bool)]),
                  propositions=prop_ids, resolved_count=resolved_count, edge_count_flat=len(src))


def region_labels(main):
    """Core region label per real node from the current connectivity regions (0 when absent)."""
    graph = main.graph
    n = graph.flat.count
    labels = np.zeros(n, np.int64)
    for entry in graph.regions.values():
        regions = entry[0] if isinstance(entry, tuple) else entry
        core = getattr(regions, 'core_labels', None)
        if core is not None and len(core) == n:
            return np.asarray(core, np.int64)
    return labels


# ── region-wise driver ───────────────────────────────────────────────────────────────────────

class Overlay:
    __slots__ = ('version', 'created', 'seed', 'cycles', 'node_count', 'real_nodes', 'edge_count', 'edge_count_flat',
                 'strength', 'state', 'stability', 'flat_edge', 'src', 'dst', 'sign', 'labels', 'propositions',
                 'regions', 'connector_edges', 'promoted', 'resolved_count', 'seconds', 'evaluations', 'reinforced',
                 'record_mean', 'record_max', 'record_promoted', 'record_edges', 'pair', 'delta', 'converged', 'region_delta')

    def __init__(self, **k):
        for name, value in k.items():
            setattr(self, name, value)

    # strengths on the flat (stored) edge index: forward edges refined, reverse edges mirrored
    def flat_strength(self):
        out = np.full(self.edge_count_flat, BASE, np.float32)
        mask = self.flat_edge >= 0
        out[self.flat_edge[mask]] = self.strength[mask]
        out[self.flat_edge[mask] + 1] = self.strength[mask]
        return out

    def record_summary(self, node):
        """Per-record view (O(1)): mean/max strength of its forward edges, promoted edge count, state, stability."""
        if node is None or node < 0 or node >= self.real_nodes or self.record_edges[node] == 0:
            return None
        return dict(mean=round(float(self.record_mean[node]), 4), max=round(float(self.record_max[node]), 4),
                    promoted=int(self.record_promoted[node]), edges=int(self.record_edges[node]),
                    state=round(float(self.state[node]), 4), stability=round(float(self.stability[node]), 4))

    def summary(self):
        s = self.strength
        return dict(version=self.version, created=self.created, nodes=self.node_count, edges=int(len(s)),
                    promoted=int(self.promoted), min=float(s.min()) if len(s) else None, max=float(s.max()) if len(s) else None,
                    median=float(np.median(s)) if len(s) else None, regions=self.regions, connector_edges=self.connector_edges,
                    resolved_records=self.resolved_count, seconds=self.seconds, evaluations=self.evaluations,
                    reinforced=self.reinforced, seed=self.seed, cycles=self.cycles, delta=self.delta, converged=self.converged)

    def to_blob(self):
        payload = {name: getattr(self, name) for name in self.__slots__}
        return zlib.compress(pickle.dumps(payload, protocol=4), 6)

    @classmethod
    def from_blob(cls, blob):
        payload = pickle.loads(zlib.decompress(blob))
        payload.setdefault('delta', 1.0); payload.setdefault('converged', False); payload.setdefault('region_delta', {})
        return cls(**payload)


def _refine_subgraph(inputs, strength, state, edge_mask, *, seed, cycles):
    """Refine the edges in ``edge_mask`` with local node numbering; returns (nodes, mean, stability, new strengths, evals, reinforced)."""
    edges = np.flatnonzero(edge_mask)
    if len(edges) == 0:
        return None
    src, dst = inputs.src[edges], inputs.dst[edges]
    local_nodes, inverse = np.unique(np.concatenate([src, dst]), return_inverse=True)
    l_src, l_dst = inverse[:len(src)], inverse[len(src):]
    mean, stability, new_strength, evals, reinforced = refine(
        inputs.direct[local_nodes], inputs.unresolved[local_nodes], l_src, l_dst, inputs.sign[edges],
        strength[edges], inputs.base[edges], cycles=cycles, seed=seed, state=state[local_nodes])
    return edges, local_nodes, mean, stability, new_strength, evals, reinforced


def refine_overlay(main, previous=None, *, seed=SEED, cycles=CYCLES, labels=None):
    """Region-wise refinement of one generation (``main`` needs ``.graph`` and ``.memory`` only — pass a
    frozen view when running outside the daemon lock), warm-started from ``previous``."""
    started = time.perf_counter()
    labels = region_labels(main) if labels is None else np.asarray(labels, np.int64)
    inputs = build_inputs(main, labels)
    e = len(inputs.src)
    strength = inputs.base.copy()
    state = inputs.direct.copy()
    if previous is not None:
        # warm start: same forward edges keep their refined strength (flat edges by index, virtual
        # record -> proposition edges by (record node, proposition id)), same nodes keep their state
        keep = np.full(e, -1, np.int64)
        p_flat = int((previous.flat_edge >= 0).sum()); n_flat = int((inputs.flat_edge >= 0).sum())
        common = min(p_flat, n_flat)                 # flat edges only append, so the forward edges are a prefix
        if common and np.array_equal(previous.flat_edge[:common], inputs.flat_edge[:common]):
            keep[:common] = np.arange(common)
        old_virtual = {(int(previous.src[i]), previous.propositions[int(previous.dst[i]) - previous.real_nodes]): i
                       for i in range(p_flat, len(previous.flat_edge))}
        for i in range(n_flat, e):
            keep[i] = old_virtual.get((int(inputs.src[i]), inputs.propositions[int(inputs.dst[i]) - inputs.real_nodes]), -1)
        mask = keep >= 0
        strength[mask] = previous.strength[keep[mask]]
        m = min(previous.real_nodes, inputs.real_nodes)
        state[:m] = previous.state[:m]
        old_prop = {pid: previous.real_nodes + i for i, pid in enumerate(previous.propositions)}
        for i, pid in enumerate(inputs.propositions):          # virtual nodes warm-start by proposition id
            if pid in old_prop:
                state[inputs.real_nodes + i] = previous.state[old_prop[pid]]
    evaluations = reinforced = 0
    region_ids = sorted(set(int(x) for x in np.unique(inputs.labels)))
    same = inputs.labels[inputs.src] == inputs.labels[inputs.dst]
    stability = np.zeros(inputs.node_count, np.float32) if previous is None else np.concatenate(
        [previous.stability[:min(previous.node_count, inputs.node_count)],
         np.zeros(max(0, inputs.node_count - previous.node_count), np.float32)])[:inputs.node_count]
    fresh = np.ones(e, bool) if previous is None else ~mask          # edges without a warm start
    old_delta = {} if previous is None else dict(getattr(previous, 'region_delta', {}) or {})
    region_delta = {}
    passes = [(r, same & (inputs.labels[inputs.src] == r), _seed(seed, r)) for r in region_ids]
    passes.append((CONNECTOR_LABEL, ~same, _seed(seed, CONNECTOR_LABEL)))   # connectors last, from the settled region states
    for r, mask_r, region_seed in passes:
        # a region that converged last time and gained no edge is left alone (steady-state cost = touched regions)
        if not fresh[mask_r].any() and old_delta.get(r, 1.0) <= TOLERANCE:
            region_delta[r] = old_delta[r]
            continue
        before_s = strength[mask_r].copy(); before_n = None
        result = _refine_subgraph(inputs, strength, state, mask_r, seed=region_seed, cycles=cycles)
        if result is None:
            continue
        edges, local_nodes, mean, stab, new_strength, evals, rf = result
        strength[edges] = new_strength
        evaluations += evals; reinforced += rf
        if r == CONNECTOR_LABEL:
            # node states are owned by their regions: a bridge is judged against the settled region
            # states, its pass moves strengths only (otherwise the two passes pull each node between
            # two fixed points and nothing converges — measured 2026-09-15)
            region_delta[r] = float(np.abs(new_strength - before_s).max())
        else:
            before_n = state[local_nodes].copy()
            state[local_nodes] = mean; stability[local_nodes] = stab
            region_delta[r] = float(max(np.abs(new_strength - before_s).max(), np.abs(mean - before_n).max()))
    n = inputs.node_count
    # convergence (v0.2 converge_graph): largest move of any warm-started strength or node state
    delta = max(region_delta.values()) if region_delta else 0.0
    record_edges = np.bincount(inputs.src, minlength=n)
    record_mean = np.bincount(inputs.src, weights=strength, minlength=n) / np.maximum(1, record_edges)
    record_max = np.zeros(n, np.float32); np.maximum.at(record_max, inputs.src, strength)
    record_promoted = np.bincount(inputs.src[strength >= PROMOTION], minlength=n)
    return Overlay(version=VERSION, created=time.strftime('%Y-%m-%d %H:%M:%S'), seed=seed, cycles=cycles,
                   node_count=n, real_nodes=inputs.real_nodes, edge_count=e, pair=None,
                   edge_count_flat=inputs.edge_count_flat, strength=strength, state=state, stability=stability,
                   flat_edge=inputs.flat_edge.astype(np.int64), src=inputs.src.astype(np.uint32), dst=inputs.dst.astype(np.uint32),
                   sign=inputs.sign, labels=inputs.labels.astype(np.int32), propositions=inputs.propositions,
                   regions=len(region_ids), connector_edges=int((~same).sum()), promoted=int((strength >= PROMOTION).sum()),
                   resolved_count=inputs.resolved_count, seconds=round(time.perf_counter() - started, 3),
                   evaluations=evaluations, reinforced=reinforced, record_mean=record_mean.astype(np.float32),
                   record_max=record_max, record_promoted=record_promoted.astype(np.int32), record_edges=record_edges.astype(np.int32),
                   delta=round(delta, 6), converged=bool(delta <= TOLERANCE), region_delta=region_delta)


def _seed(seed, region):
    digest = hashlib.sha256(f'{seed}:{region}'.encode()).digest()
    return int.from_bytes(digest[:8], 'little')
