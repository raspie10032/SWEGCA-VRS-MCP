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

from . import vrs_evidence

REINFORCE = np.float32(1.01)
WEAKEN = np.float32(0.995)
BASE = np.float32(0.75)   # legacy placeholder for an edge appended before its first consolidation
BASE_FLOOR = np.float32(0.05)   # a record with no evidence weight still gets a tiny base so the clamp is defined
PROMOTION = 1.0
CYCLES = 16               # shuffle cycles per consolidation — one refinement per generation, as the original
                          # organizer runs it (16-20); strengths carry over, so a connection's strength is how many
                          # generations it stayed stable, not a fixed point (running to convergence saturated every
                          # edge at the clamp — measured 2026-09-15)
IDLE_CYCLES = 16
BATCH = 4096
SEED = 1729
TOLERANCE = 1e-3          # v0.2 converge_graph: consolidate until the largest move is below this
CONNECTOR_LABEL = -1
FINE_REGIONS = True       # split each connectivity region again with the same modularity rule (finer units)
FINE_MIN_NODES = 400      # regions below this many nodes are not split further
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

record_polarity = vrs_evidence.record_polarity


# ── the stable version ───────────────────────────────────────────────────────────────────────

class VRSVersion:
    """Result of one consolidation: refined strengths for the forward edges, node states, and the
    virtual proposition edges. Immutable; the graph's flat arrays carry the mirrored strengths."""
    __slots__ = ('version_id', 'parent_id', 'created', 'seed', 'cycles', 'node_count', 'edge_count',
                 'state', 'stability', 'labels', 'labels_digest', 'proposition_ids', 'prop_src', 'prop_strength',
                 'prop_state', 'prop_pid', 'region_delta', 'delta', 'converged', 'promoted', 'reinforced', 'evaluations',
                 'resolved_count', 'seconds', 'regions', 'connector_edges', 'direct', 'unresolved', 'prop_direct', 'prop_unresolved',
                 'portals', 'portal_events', 'coarse_labels', 'fine_seconds', 'decisions', 'record_weight', 'evidence_counts')

    def __init__(self, **k):
        for name in self.__slots__:
            setattr(self, name, k.get(name))

    def summary(self):
        return dict(version=VERSION, version_id=self.version_id, parent_id=self.parent_id, created=self.created,
                    seed=self.seed, cycles=self.cycles, nodes=self.node_count, edges=self.edge_count,
                    regions=self.regions, connector_edges=self.connector_edges, promoted=self.promoted,
                    reinforced=self.reinforced, resolved_records=self.resolved_count, delta=self.delta,
                    converged=self.converged, seconds=self.seconds, evaluations=self.evaluations,
                    portals=self.portal_counts(), fine_seconds=getattr(self, 'fine_seconds', None),
                    evidence=getattr(self, 'evidence_counts', None))

    def proposition_strength(self, node):
        mask = self.prop_src == node
        return float(self.prop_strength[mask].max()) if mask.any() else None

    def portal(self, a, b):
        """The portal between two regions (order-free), or None when they share no connector edge."""
        return (getattr(self, 'portals', None) or {}).get((min(a, b), max(a, b)))

    def portal_counts(self):
        portals = getattr(self, 'portals', None) or {}          # versions pickled before portals existed
        events = getattr(self, 'portal_events', None) or ()
        return dict(pairs=len(portals), candidates=sum(1 for v in portals.values() if v['status'] == 'candidate'),
                    opened=sum(1 for e in events if e['event'] == 'opened'),
                    withdrawn=sum(1 for e in events if e['event'] == 'withdrawn'))


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


def build_portals(inp, strength, labels_of_nodes, previous):
    """G6 portals (design Phase 3): one navigation object per region pair that shares connector edges.

    ``score`` = share of the pair's connector edges at or above the promotion threshold (trusted bridges);
    ``status`` 'candidate' when at least one bridge is promoted, else 'weak'. Provenance = the strongest
    bridges (record node, cue node, strength). Decay and withdrawal are inherited from the strengths:
    a bridge whose endpoints stop being resolved decays at consolidation, and the portal is withdrawn
    when its last promoted bridge drops below 1.0. Events against the previous version are recorded.
    Region scores are navigation, never truth certification, and nothing here restricts memory access.
    """
    src, dst = inp['src'], inp['dst']
    la, lb = labels_of_nodes[src], labels_of_nodes[dst]
    cross = np.flatnonzero(la != lb)
    portals = {}
    if len(cross):
        a = np.minimum(la[cross], lb[cross]); b = np.maximum(la[cross], lb[cross])
        key = a.astype(np.int64) * (int(labels_of_nodes.max()) + 2) + b
        order = np.argsort(key, kind='stable')
        key_sorted = key[order]
        bounds = np.flatnonzero(np.r_[True, key_sorted[1:] != key_sorted[:-1], True])
        for i in range(len(bounds) - 1):
            block = cross[order[bounds[i]:bounds[i + 1]]]
            st = strength[block]
            promoted = int((st >= PROMOTION).sum())
            top = block[np.argsort(-st)[:3]]
            pair = (int(a[order[bounds[i]]]), int(b[order[bounds[i]]]))
            portals[pair] = dict(bridges=int(len(block)), promoted=promoted, score=round(promoted / len(block), 4),
                                 mean_strength=round(float(st.mean()), 4), max_strength=round(float(st.max()), 4),
                                 status='candidate' if promoted else 'weak',
                                 provenance=[(int(src[e]), int(dst[e]), round(float(strength[e]), 4)) for e in top])
    events = []
    old = (getattr(previous, 'portals', None) or {}) if previous is not None else {}
    for pair, portal in portals.items():
        was = old.get(pair, {}).get('status')
        if portal['status'] == 'candidate' and was != 'candidate':
            events.append(dict(pair=pair, event='opened', score=portal['score'], promoted=portal['promoted']))
        elif portal['status'] != 'candidate' and was == 'candidate':
            events.append(dict(pair=pair, event='withdrawn', score=portal['score']))
    for pair in old:
        if pair not in portals and old[pair].get('status') == 'candidate':
            events.append(dict(pair=pair, event='withdrawn', score=0.0))
    return portals, events


def fine_regions(graph, coarse, *, min_nodes=FINE_MIN_NODES, previous=None):
    """Split every coarse region again by the same modularity rule on its induced subgraph.

    The connectivity regions the graph maintains are few and large (16 for 100k nodes, a hub of 20k):
    queries activate half of them and a scope built on them excludes almost nothing. Applying the
    rule recursively inside each region gives units small enough for region scope and portals to
    mean something, with no new algorithm. Small regions stay whole.

    Warm start (``previous`` = the last consolidation's fine labels, same node ids): the local moves
    start from the previous membership, and each resulting sub-region takes over the previous fine
    id it overlaps most (unclaimed ids only), so ids stay put for regions that did not change — the
    per-region convergence skip and the per-region shuffle seeds then carry across consolidations.
    Returns (fine_labels, region_count).
    """
    from .fast_regions import build_regions
    from types import SimpleNamespace
    flat = graph.flat
    src = np.asarray(flat.src, np.int64); dst = np.asarray(flat.dst, np.int64)
    sign = np.asarray(flat.sign, np.int8); strength = np.asarray(flat.strength, np.float64)
    coarse = np.asarray(coarse, np.int64)
    n = len(coarse)
    prev = None
    if previous is not None and len(previous):
        prev = np.full(n, -1, np.int64); m = min(n, len(previous)); prev[:m] = np.asarray(previous[:m], np.int64)
    fine = np.full(n, -1, np.int64)
    claimed = set()
    next_id = (int(prev.max()) + 1) if prev is not None and prev.max() >= 0 else 0

    def take(candidates_prev):
        # the previous fine id most of these nodes had, if unclaimed; else a fresh id
        nonlocal next_id
        if len(candidates_prev):
            ids, counts = np.unique(candidates_prev[candidates_prev >= 0], return_counts=True)
            for k in np.argsort(-counts):
                fid = int(ids[k])
                if fid not in claimed:
                    claimed.add(fid); return fid
        fid = next_id; next_id += 1; claimed.add(fid); return fid

    for r in np.unique(coarse):
        members = np.flatnonzero(coarse == r)
        if len(members) < min_nodes:
            fine[members] = take(prev[members] if prev is not None else np.empty(0, np.int64))
            continue
        local = np.full(n, -1, np.int64); local[members] = np.arange(len(members))
        e = np.flatnonzero((coarse[src] == r) & (coarse[dst] == r))
        if len(e) == 0:
            fine[members] = take(prev[members] if prev is not None else np.empty(0, np.int64))
            continue
        source = SimpleNamespace(terms=graph.lazy_terms(members), edge_source=local[src[e]], edge_target=local[dst[e]],
                                 edge_sign=sign[e], vrs_strength=strength[e])
        warm = None
        if prev is not None:
            had = members[prev[members] >= 0]
            if len(had):
                # previous fine ids of these members as local sub-labels 0..k-1 (build_regions warm start)
                _, sub_old = np.unique(prev[had], return_inverse=True)
                warm = (SimpleNamespace(terms=SimpleNamespace(members=had), core_labels=sub_old), None)
        regions, _ = build_regions(source, vrs_snapshot_id=graph.snapshot_id, previous=warm)
        sub = np.asarray(regions.core_labels, np.int64)
        for k in np.unique(sub):
            group = members[sub == k]
            fine[group] = take(prev[group] if prev is not None else np.empty(0, np.int64))
    return fine, len(claimed)


def _seed(seed, region):
    return int.from_bytes(hashlib.sha256(f'{seed}:{region}'.encode()).digest()[:8], 'little')


def build_inputs(graph, memory, labels):
    """Kernel inputs for the current generation from the SWEGCA evidence layer (``vrs_evidence``).

    Real nodes are records and cues, then one virtual node per declared proposition (hypothesis).
    A cue is association, not a declared hypothesis: direct 0, never unresolved, edge sign +1 — its
    state is numerical dependency on the records touching it (the engine's own reading of an edge).
    A record's direct is tanh(w) with w the arbiter's proposal weight from its proposition's evidence
    (0 for a pending record or a bare outcome without a declared hypothesis: undefined stays
    insufficient), and w is the base strength of its edges. A proposition's direct and unresolved come
    from the accumulator (see vrs_evidence); a record -> proposition edge carries the polarity as sign.
    """
    flat, nodes = graph.flat, graph.nodes
    n = flat.count
    src = np.asarray(flat.src, np.int64); dst = np.asarray(flat.dst, np.int64)
    record_mask = np.asarray(nodes.node_cue) < 0
    forward = np.arange(0, len(src), 2)
    if len(src) and not (record_mask[src[forward]].all() and (src[forward] == dst[forward + 1]).all()):
        raise ValueError('flat edge layout is not (record->cue, cue->record) pairs')
    evidence = vrs_evidence.build(graph, memory)
    direct = np.zeros(n, np.float32); unresolved = np.zeros(n, bool)
    weight = np.zeros(n, np.float32)
    resolved_count = 0
    for node in np.flatnonzero(record_mask):
        eid = nodes.node_episode[int(node)]
        pol = evidence.record_polarity.get(eid, 0)
        w = float(evidence.record_weight.get(eid, 0.0)) if pol else 0.0
        primary = evidence.record_primary.get(eid)
        h = evidence.hypotheses.get(primary) if primary else None
        if pol:
            resolved_count += 1
        weight[node] = w
        direct[node] = np.tanh(w)
        unresolved[node] = (not pol) or (h is not None and h.unresolved())
    f_src, f_dst = src[forward], dst[forward]
    base = np.maximum(weight[f_src], BASE_FLOOR).astype(np.float32)
    sign = np.ones(len(forward), np.int8)                      # association edges carry no polarity
    prop_ids = sorted(memory.propositions)
    p_direct, p_unresolved, p_src, p_dst, p_sign, p_base, p_label = [], [], [], [], [], [], []
    decisions = {}
    for k, pid in enumerate(prop_ids):
        pnode = n + k; first = None
        h = evidence.hypotheses.get('proposition:' + pid)
        for eid in sorted(memory.propositions[pid]):
            rnode = nodes.episode_node.get(eid)
            if rnode is None:
                continue
            first = rnode if first is None else first
            pol = evidence.record_polarity.get(eid, 0)
            p_src.append(rnode); p_dst.append(pnode); p_sign.append(pol if pol else 1)
            p_base.append(max(float(evidence.record_weight.get(eid, 0.0)) if pol else 0.0, BASE_FLOOR))
        p_direct.append(h.direct() if h is not None else 0.0)
        p_unresolved.append(h.unresolved() if h is not None else False)
        p_label.append(int(labels[first]) if first is not None else 0)
        if h is not None:
            decisions[pid] = h.summary()
    extra = len(prop_ids)
    return dict(node_count=n + extra, real_nodes=n, forward=forward,
                direct=np.concatenate([direct, np.asarray(p_direct, np.float32)]),
                unresolved=np.concatenate([unresolved, np.asarray(p_unresolved, bool)]),
                labels=np.concatenate([labels[:n], np.asarray(p_label, np.int64)]),
                src=np.concatenate([f_src, np.asarray(p_src, np.int64)]),
                dst=np.concatenate([f_dst, np.asarray(p_dst, np.int64)]),
                sign=np.concatenate([sign, np.asarray(p_sign, np.int8)]),
                base=np.concatenate([base, np.asarray(p_base, np.float32)]),
                proposition_ids=prop_ids, prop_edges=len(p_src), resolved_count=resolved_count,
                decisions=decisions, record_weight=weight,
                evidence_counts=dict(hypotheses=len(evidence.hypotheses), observations=evidence.observation_count,
                                     accepted=sum(1 for h in evidence.hypotheses.values() if h.decision.status == 'accept'),
                                     rejected=sum(1 for h in evidence.hypotheses.values() if h.decision.status == 'reject'),
                                     abstain=sum(1 for h in evidence.hypotheses.values() if h.decision.status == 'abstain')))


def _refine_subgraph(inp, strength, state, edge_mask, *, seed, cycles):
    edges = np.flatnonzero(edge_mask)
    if len(edges) == 0:
        return None
    src, dst = inp['src'][edges], inp['dst'][edges]
    local_nodes, inverse = np.unique(np.concatenate([src, dst]), return_inverse=True)
    l_src, l_dst = inverse[:len(src)], inverse[len(src):]
    base = inp['base'][edges]
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
    coarse = region_labels(graph) if labels is None else np.asarray(labels, np.int64)
    fine_seconds = 0.0
    if labels is None and FINE_REGIONS and len(coarse):
        t_fine = time.perf_counter()
        labels, _ = fine_regions(graph, coarse, previous=getattr(previous, 'labels', None) if previous is not None else None)
        fine_seconds = round(time.perf_counter() - t_fine, 3)
    else:
        labels = coarse
    inp = build_inputs(graph, memory, labels)
    flat = graph.flat
    e = len(inp['src']); n_real = inp['real_nodes']; n_flat_fwd = len(inp['forward'])
    strength = inp['base'].copy()
    # edges refined before keep their refined strength (the flat arrays carry it); an edge appended since
    # the last consolidation starts at its base w — the .75 placeholder written at append time is not a
    # strength (measured 2026-09-15: starting from it let a w=.25 record hit its cap in two generations)
    refined = int(previous.edge_count // 2) if previous is not None else 0
    refined = min(refined, n_flat_fwd)
    strength[:refined] = np.asarray(flat.strength)[inp['forward'][:refined]]
    # the evidence may have moved a record's weight since its edges were refined: keep the refined
    # value inside the new clamp (base x [.25, 4])
    strength = np.minimum(np.maximum(strength, inp['base'] * np.float32(0.25)), inp['base'] * np.float32(4.0))
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
                strength[i] = min(max(previous.prop_strength[k], inp['base'][i] * 0.25), inp['base'][i] * 4.0); fresh[i] = False
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
    src_label = inp['labels'][inp['src']]
    passes = [(r, same & (src_label == r), _seed(seed, r)) for r in region_ids]
    # connectors in bundles by the source record's region (with fine regions most edges cross, so one
    # global connector pass would run every time; a bundle is skipped like a region when nothing in it
    # is fresh and it converged last time)
    passes.extend((('c', r), ~same & (src_label == r), _seed(seed, f'c{r}')) for r in region_ids)
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
        if isinstance(r, tuple):
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
    portals, portal_events = build_portals(inp, strength, inp['labels'], previous)
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
                         prop_direct=inp['direct'][n_real:].astype(np.float32), prop_unresolved=inp['unresolved'][n_real:].copy(),
                         portals=portals, portal_events=portal_events, coarse_labels=coarse.astype(np.int32),
                         fine_seconds=fine_seconds, decisions=inp['decisions'], record_weight=inp['record_weight'],
                         evidence_counts=inp['evidence_counts'])
    version.prop_pid = prop_pid
    return version, flat_strength, flat_score
