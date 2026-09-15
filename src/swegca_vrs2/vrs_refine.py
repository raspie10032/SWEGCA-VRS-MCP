"""VRS consolidation: region-wise strength refinement of one generation (vrs-regions branch, 2026-09-15).

This is the numerical core of the rebuilt VRS. Upstream v2.1 settled only the event *signal*
(``settle_event_signal``: scores move, strengths fixed) and never called the refinement kernel, so
no edge could reach the promotion threshold. Here the v0.2 kernel (``refine_vrs``: shuffled passes
over the edges, x1.01 where the two endpoint states agree, x0.995 otherwise, clamped to
base x [.25, 4], promotion at >= 1.0) runs at consolidation time over the experience graph itself.

Graph semantics (the v0.2 star graph generalized to the v2.1 experience graph)
------------------------------------------------------------------------------
* record node = v0.2 event: ``direct = tanh(1)`` while the record is resolved (outcome success/failure,
  or an explicit proposition polarity, and not superseded), else 0 and ``unresolved`` — an unresolved
  endpoint blocks reinforcement of its edges, so pending observations decay to the floor.
* cue node = generalized hypothesis: ``direct = tanh((support - refute) / max(1, support + refute))``
  over the resolved records touching it (success/support counts for, failure/refute against);
  unresolved when both sides are present (falsification first).
* explicit propositions are virtual hypothesis nodes with the same rule over their members; the
  record -> proposition edges carry the evidence polarity as sign. They live only inside the
  consolidation (``VRSVersion.proposition_edges``) so the node directory and regions are untouched.
* only the record -> cue direction is refined (v0.2 edges run event -> hypothesis); the stored
  reverse edge mirrors its pair. Edge sign = the record's polarity (+1 / -1).
* base strength .75 (v0.2), clamp [.1875, 3.0]; promotion at >= 1.0 (``PROMOTION``).

Regions and connectors (the design's G6/G9, absent from every upstream version)
--------------------------------------------------------------------------------
The connectivity regions the graph already maintains are the unit of refinement: each region's
internal edges are refined on their own (degree normalization inside the region); the edges that
cross regions are refined once more as connectors, judged against the settled region states
(their pass moves strengths only — letting it move node states pulls each node between two fixed
points and nothing converges; measured). A connector at >= 1.0 is a trusted bridge.

Determinism: one ``numpy`` generator seeded per (seed, region); a region that converged and gained
no edge is skipped, so the steady-state cost is the regions a new record touched.
"""
from __future__ import annotations

import hashlib
import pickle
import time
import zlib

import numpy as np

REINFORCE = np.float32(1.01)
WEAKEN = np.float32(0.995)
BASE = np.float32(0.75)
PROMOTION = 1.0
CYCLES = 32               # cycles per manual/test consolidation chunk (~2.5 s at 5k records)
IDLE_CYCLES = 512         # cycles per idle consolidation: pending edges reach the floor (.75 -> .1875 takes
                          # ~450 weakening cycles) in one journal row instead of fourteen; ~30 s CPU at 5k records,
                          # outside the daemon lock
BATCH = 4096
SEED = 1729
TOLERANCE = 1e-3          # v0.2 converge_graph: consolidate until the largest move is below this
CONNECTOR_LABEL = -1
VERSION = 'vrs-regions-consolidation-v1'


# ── kernel (numpy port of v0.2 refine_vrs; same arithmetic order, float32) ────────────────────

def refine(direct, unresolved, src, dst, sign, strength, base, *, cycles=CYCLES, passes=1,
           batch=BATCH, seed=SEED, state=None):
    """``cycles`` shuffled passes over the edges, each followed by the strength update.

    Returns (mean_state, stability, strength, evaluations, reinforced_edges); float32 like the torch
    original. ``state`` warm-starts from the previous consolidation.
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


# ── record semantics ─────────────────────────────────────────────────────────────────────────

def record_polarity(episode, superseded):
    """+1 / -1 for a resolved record (outcome, else explicit evidence polarity), 0 when unresolved."""
    step = episode.steps[0]
    if episode.episode_id in superseded:
        return 0
    if step.outcome == 'success':
        return 1
    if step.outcome == 'failure':
        return -1
    polarity = step.observation.get('evidence_polarity')
    return 1 if polarity == 'support' else -1 if polarity == 'refute' else 0


# ── the stable version ───────────────────────────────────────────────────────────────────────

class VRSVersion:
    """Result of one consolidation: refined strengths for the forward edges, node states, and the
    virtual proposition edges. Immutable; the graph's flat arrays carry the mirrored strengths."""
    __slots__ = ('version_id', 'parent_id', 'created', 'seed', 'cycles', 'node_count', 'edge_count',
                 'state', 'stability', 'labels', 'labels_digest', 'proposition_ids', 'prop_src', 'prop_strength',
                 'prop_state', 'prop_pid', 'region_delta', 'delta', 'converged', 'promoted', 'reinforced', 'evaluations',
                 'resolved_count', 'seconds', 'regions', 'connector_edges', 'direct', 'unresolved', 'prop_direct', 'prop_unresolved')

    def __init__(self, **k):
        for name in self.__slots__:
            setattr(self, name, k.get(name))

    def summary(self):
        return dict(version=VERSION, version_id=self.version_id, parent_id=self.parent_id, created=self.created,
                    seed=self.seed, cycles=self.cycles, nodes=self.node_count, edges=self.edge_count,
                    regions=self.regions, connector_edges=self.connector_edges, promoted=self.promoted,
                    reinforced=self.reinforced, resolved_records=self.resolved_count, delta=self.delta,
                    converged=self.converged, seconds=self.seconds, evaluations=self.evaluations)

    def proposition_strength(self, node):
        mask = self.prop_src == node
        return float(self.prop_strength[mask].max()) if mask.any() else None


def region_labels(graph):
    """Core region label per node from the graph's connectivity regions (components offset apart)."""
    n = graph.flat.count
    labels = np.zeros(n, np.int64)
    base = 0
    for component, entry in sorted(graph.regions.items()):
        regions, local = entry
        core = np.asarray(regions.core_labels, np.int64)
        local = np.asarray(local)                       # node -> local position (-1 outside); may be shorter than n
        members = np.flatnonzero(local >= 0)
        labels[members] = base + core[local[members]]
        base += int(core.max()) + 1 if len(core) else 1
    return labels


def _seed(seed, region):
    return int.from_bytes(hashlib.sha256(f'{seed}:{region}'.encode()).digest()[:8], 'little')


def build_inputs(graph, memory, labels):
    """Overlay arrays for the current generation: real nodes (records + cues) then virtual propositions."""
    flat, nodes = graph.flat, graph.nodes
    n = flat.count
    src = np.asarray(flat.src, np.int64); dst = np.asarray(flat.dst, np.int64)
    record_mask = np.asarray(nodes.node_cue) < 0
    forward = np.arange(0, len(src), 2)
    if len(src) and not (record_mask[src[forward]].all() and (src[forward] == dst[forward + 1]).all()):
        raise ValueError('flat edge layout is not (record->cue, cue->record) pairs')
    direct = np.zeros(n, np.float32); unresolved = np.ones(n, bool)
    polarity = np.zeros(n, np.int8)
    superseded = memory.superseded
    resolved_count = 0
    for node in np.flatnonzero(record_mask):
        episode = memory.episode(nodes.node_episode[int(node)])
        p = record_polarity(episode, superseded)
        if p:
            direct[node] = np.tanh(1.0); unresolved[node] = False; polarity[node] = p; resolved_count += 1
    f_src, f_dst = src[forward], dst[forward]
    pol = polarity[f_src]
    support = np.bincount(f_dst[pol > 0], minlength=n); refute = np.bincount(f_dst[pol < 0], minlength=n)
    cue_nodes = ~record_mask
    total = np.maximum(1, support + refute)
    direct[cue_nodes] = np.tanh((support[cue_nodes] - refute[cue_nodes]) / total[cue_nodes]).astype(np.float32)
    unresolved[cue_nodes] = (support[cue_nodes] > 0) & (refute[cue_nodes] > 0)
    sign = np.where(pol < 0, -1, 1).astype(np.int8)
    prop_ids = sorted(memory.propositions)
    p_direct, p_unresolved, p_src, p_dst, p_sign, p_label = [], [], [], [], [], []
    for k, pid in enumerate(prop_ids):
        pnode = n + k; s = r = 0; first = None
        for eid in sorted(memory.propositions[pid]):
            rnode = nodes.episode_node.get(eid)
            if rnode is None:
                continue
            first = rnode if first is None else first
            episode = memory.episode(eid)
            if episode.episode_id in superseded:
                p_src.append(rnode); p_dst.append(pnode); p_sign.append(1)
                continue
            supports = episode.steps[0].observation.get('evidence_polarity') != 'refute'
            s += supports; r += (not supports)
            p_src.append(rnode); p_dst.append(pnode); p_sign.append(1 if supports else -1)
        p_direct.append(np.tanh((s - r) / max(1, s + r))); p_unresolved.append(bool(s and r))
        p_label.append(int(labels[first]) if first is not None else 0)
    extra = len(prop_ids)
    return dict(node_count=n + extra, real_nodes=n, forward=forward,
                direct=np.concatenate([direct, np.asarray(p_direct, np.float32)]),
                unresolved=np.concatenate([unresolved, np.asarray(p_unresolved, bool)]),
                labels=np.concatenate([labels[:n], np.asarray(p_label, np.int64)]),
                src=np.concatenate([f_src, np.asarray(p_src, np.int64)]),
                dst=np.concatenate([f_dst, np.asarray(p_dst, np.int64)]),
                sign=np.concatenate([sign, np.asarray(p_sign, np.int8)]),
                proposition_ids=prop_ids, prop_edges=len(p_src), resolved_count=resolved_count)


def _refine_subgraph(inp, strength, state, edge_mask, *, seed, cycles):
    edges = np.flatnonzero(edge_mask)
    if len(edges) == 0:
        return None
    src, dst = inp['src'][edges], inp['dst'][edges]
    local_nodes, inverse = np.unique(np.concatenate([src, dst]), return_inverse=True)
    l_src, l_dst = inverse[:len(src)], inverse[len(src):]
    base = np.full(len(edges), BASE, np.float32)
    mean, stability, new_strength, evals, reinforced = refine(
        inp['direct'][local_nodes], inp['unresolved'][local_nodes], l_src, l_dst, inp['sign'][edges],
        strength[edges], base, cycles=cycles, seed=seed, state=state[local_nodes])
    return edges, local_nodes, mean, stability, new_strength, evals, reinforced


def consolidate(graph, memory, previous, *, seed=SEED, cycles=CYCLES, labels=None):
    """One consolidation chunk. Returns (VRSVersion, flat_strength, flat_score).

    ``previous`` is the graph's current stable version (or None). Strengths warm-start from the
    graph's flat arrays (stable edges keep their refined strength, pending edges sit at base) and
    states from the previous version; the virtual proposition edges warm-start by proposition id.
    """
    started = time.perf_counter()
    labels = region_labels(graph) if labels is None else np.asarray(labels, np.int64)
    inp = build_inputs(graph, memory, labels)
    flat = graph.flat
    e = len(inp['src']); n_real = inp['real_nodes']; n_flat_fwd = len(inp['forward'])
    strength = np.full(e, BASE, np.float32)
    strength[:n_flat_fwd] = np.asarray(flat.strength)[inp['forward']]
    state = inp['direct'].copy()
    fresh = np.ones(e, bool)
    old_delta = {}
    if previous is not None:
        m = min(previous.node_count, n_real)
        state[:m] = previous.state[:m]
        old_prop = {pid: i for i, pid in enumerate(previous.proposition_ids)}
        for i, pid in enumerate(inp['proposition_ids']):
            j = old_prop.get(pid)
            if j is not None:
                state[n_real + i] = previous.prop_state[j]
        # virtual edges warm-start by (record node, proposition id); forward edges that existed at the
        # previous consolidation already carry their refined strength in the flat arrays
        old_map = {(int(previous.prop_src[k]), previous.prop_pid[k]): k for k in range(len(previous.prop_src))}
        for i in range(n_flat_fwd, e):
            k = old_map.get((int(inp['src'][i]), inp['proposition_ids'][int(inp['dst'][i]) - n_real]))
            if k is not None:
                strength[i] = previous.prop_strength[k]; fresh[i] = False
        fresh[:min(n_flat_fwd, previous.edge_count // 2)] = False
        # a node whose inputs changed (resolved, superseded, a cue's support/refute balance, a proposition's
        # members) makes every edge touching it fresh, even in a region that had converged
        changed = np.ones(inp['node_count'], bool)
        if previous.direct is not None:
            changed[:m] = (inp['direct'][:m] != previous.direct[:m]) | (inp['unresolved'][:m] != previous.unresolved[:m])
            for i, pid in enumerate(inp['proposition_ids']):
                j = old_prop.get(pid)
                if j is not None:
                    changed[n_real + i] = bool(inp['direct'][n_real + i] != previous.prop_direct[j]
                                               or inp['unresolved'][n_real + i] != previous.prop_unresolved[j])
        fresh |= changed[inp['src']] | changed[inp['dst']]
        old_delta = dict(previous.region_delta or {})
    stability = np.zeros(inp['node_count'], np.float32)
    if previous is not None:
        m = min(previous.node_count, n_real)
        stability[:m] = previous.stability[:m]
    evaluations = reinforced = 0
    region_ids = sorted(set(int(x) for x in np.unique(inp['labels'])))
    same = inp['labels'][inp['src']] == inp['labels'][inp['dst']]
    region_delta = {}
    passes = [(r, same & (inp['labels'][inp['src']] == r), _seed(seed, r)) for r in region_ids]
    passes.append((CONNECTOR_LABEL, ~same, _seed(seed, CONNECTOR_LABEL)))
    for r, mask_r, region_seed in passes:
        if not fresh[mask_r].any() and old_delta.get(r, 1.0) <= TOLERANCE:
            region_delta[r] = old_delta[r]
            continue
        before_s = strength[mask_r].copy()
        result = _refine_subgraph(inp, strength, state, mask_r, seed=region_seed, cycles=cycles)
        if result is None:
            continue
        edges, local_nodes, mean, stab, new_strength, evals, rf = result
        strength[edges] = new_strength
        evaluations += evals; reinforced += rf
        if r == CONNECTOR_LABEL:
            region_delta[r] = float(np.abs(new_strength - before_s).max())
        else:
            before_n = state[local_nodes].copy()
            state[local_nodes] = mean; stability[local_nodes] = stab
            region_delta[r] = float(max(np.abs(new_strength - before_s).max(), np.abs(mean - before_n).max()))
    delta = max(region_delta.values()) if region_delta else 0.0
    # flat arrays: forward edges refined, reverse edges mirrored; score = node state
    flat_strength = np.asarray(flat.strength, np.float32).copy()
    flat_strength[inp['forward']] = strength[:n_flat_fwd]
    flat_strength[inp['forward'] + 1] = strength[:n_flat_fwd]
    flat_score = state[:n_real].astype(np.float32)
    prop_src = inp['src'][n_flat_fwd:].astype(np.int64)
    prop_pid = [inp['proposition_ids'][int(d) - n_real] for d in inp['dst'][n_flat_fwd:]]
    labels_digest = hashlib.sha256(np.ascontiguousarray(labels, np.int64).tobytes()).hexdigest()
    promoted = int((strength >= PROMOTION).sum())
    version_id = hashlib.sha256(b'|'.join([
        (previous.version_id if previous is not None else 'none').encode(), str(seed).encode(), str(cycles).encode(),
        labels_digest.encode(), np.ascontiguousarray(flat_strength).tobytes(), np.ascontiguousarray(flat_score).tobytes(),
        np.ascontiguousarray(strength[n_flat_fwd:]).tobytes()])).hexdigest()
    version = VRSVersion(version_id=version_id, parent_id=previous.version_id if previous is not None else None,
                         created=time.strftime('%Y-%m-%d %H:%M:%S'), seed=seed, cycles=cycles, node_count=n_real,
                         edge_count=len(flat.src), state=state[:n_real].astype(np.float32),
                         stability=stability[:n_real], labels=labels.astype(np.int32), labels_digest=labels_digest,
                         proposition_ids=inp['proposition_ids'], prop_src=prop_src,
                         prop_strength=strength[n_flat_fwd:].astype(np.float32), prop_state=state[n_real:].astype(np.float32),
                         region_delta=region_delta, delta=round(delta, 6), converged=bool(delta <= TOLERANCE),
                         promoted=promoted, reinforced=reinforced, evaluations=evaluations,
                         resolved_count=inp['resolved_count'], seconds=round(time.perf_counter() - started, 3),
                         regions=len(region_ids), connector_edges=int((~same).sum()),
                         direct=inp['direct'][:n_real].astype(np.float32), unresolved=inp['unresolved'][:n_real].copy(),
                         prop_direct=inp['direct'][n_real:].astype(np.float32), prop_unresolved=inp['unresolved'][n_real:].copy())
    version.prop_pid = prop_pid
    return version, flat_strength, flat_score
