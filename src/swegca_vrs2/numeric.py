"""Affected-component vector execution of the unchanged f32 event rule.

Native modules remain the reference. Ambiguous f32 rounding is recomputed with
math.fsum/math.tanh in the original edge order. No warm-start topology change.
"""
import math
import numpy as np
from .engine.mosaic_vrs_event_signal import (
    EventSignalProposal, _bind_strength_updates, settle_event_signal as reference,
)
from .engine.mosaic_vrs_connectivity_regions import _canonical


def settle(inputs, *, changed_nodes=(), strength_updates=None, maximum_rounds=512):
    inputs.require_validated_immutable()
    if type(maximum_rounds) is not int or maximum_rounds < 0:
        raise ValueError('signal round budget must be a nonnegative integer')
    strengths, targets = _bind_strength_updates(inputs, strength_updates, 'vrs-edge:')
    seeds = set(changed_nodes) | targets
    if any(type(n) is not int or not 0 <= n < len(inputs.score) for n in seeds):
        raise ValueError('signal seed outside node directory')
    # Only outgoing-reachable nodes can be changed by this event. Incoming-only
    # sources are immutable boundary values, not additional activated nodes.
    reachable, queue = set(), list(seeds)
    while queue:
        n = queue.pop()
        if n in reachable:
            continue
        reachable.add(n)
        queue.extend(int(inputs.edges['target'][e]) for e in inputs.dependencies.edges_for(n, direction='outgoing'))
    ordered = sorted(reachable)
    if len(ordered) < 32:
        return reference(inputs, changed_nodes=changed_nodes, strength_updates=strength_updates, maximum_rounds=maximum_rounds)
    positions = {n: i for i, n in enumerate(ordered)}
    all_nodes = dict(positions)
    source, target, weight, offsets = [], [], [], [0]
    for n in ordered:
        for e in inputs.dependencies.edges_for(n, direction='incoming'):
            s = int(inputs.edges['source'][e])
            if s not in all_nodes:
                all_nodes[s] = len(all_nodes)
            source.append(all_nodes[s]); target.append(positions[n])
            weight.append(float(strengths.get(int(e), inputs.strength[e])) * int(inputs.edges['sign'][e]))
        offsets.append(len(source))
    source, target = np.asarray(source, dtype=np.int64), np.asarray(target, dtype=np.int64)
    weight = np.asarray(weight, dtype=np.float64)
    original = np.asarray([float(inputs.score[n]) for n in all_nodes], dtype=np.float64)
    current = original.copy()
    direct = np.asarray([float(inputs.direct[n]) for n in ordered], dtype=np.float64)
    # Denominator is sum of unsigned strengths, even for a zero-sign edge.
    denominators = np.asarray([max(1., math.fsum(float(strengths.get(int(e), inputs.strength[e]))
        for e in inputs.dependencies.edges_for(n, direction='incoming'))) for n in ordered])
    pending = np.zeros(len(ordered), dtype=bool)
    pending[[positions[n] for n in seeds]] = True
    rounds = evaluations = edge_evaluations = 0
    degrees = np.diff(offsets)
    for _ in range(maximum_rounds):
        active = np.flatnonzero(pending)
        if not len(active):
            break
        products = current[source] * weight
        signals = np.bincount(target, weights=products, minlength=len(ordered))
        value = .8 * current[active] + .2 * np.tanh(direct[active] + .2 * signals[active] / denominators[active])
        rounded = value.astype(np.float32)
        # Conservative bound for sum and elementary-function rounding. Rare
        # midpoint/cancellation cases use the reference scalar evaluation.
        below = np.nextafter(rounded, np.float32(-np.inf)).astype(np.float64)
        above = np.nextafter(rounded, np.float32(np.inf)).astype(np.float64)
        low = (below + rounded.astype(np.float64)) * .5
        high = (above + rounded.astype(np.float64)) * .5
        magnitude = np.bincount(target, weights=np.abs(products), minlength=len(ordered))
        error = 128 * np.finfo(float).eps * (1 + degrees[active]) * (1 + magnitude[active] + np.abs(current[active]))
        ambiguous = np.minimum(np.abs(value-low), np.abs(value-high)) <= error
        for a in np.flatnonzero(ambiguous):
            n = active[a]
            signal = math.fsum(float(current[source[e]]) * float(weight[e]) for e in range(offsets[n], offsets[n+1]))
            rounded[a] = np.float32(.8*float(current[n]) + .2*math.tanh(float(direct[n])+.2*signal/float(denominators[n])))
        changed = active[rounded.view(np.uint32) != current[active].astype(np.float32).view(np.uint32)]
        current[active] = rounded
        pending.fill(False)
        pending[changed] = True
        changed_flags = np.zeros(len(all_nodes), dtype=bool)
        changed_flags[changed] = True
        pending[target[changed_flags[source]]] = True
        rounds += 1; evaluations += len(active); edge_evaluations += int(degrees[active].sum())
    differing = current[:len(ordered)].astype(np.float32).view(np.uint32) != original[:len(ordered)].astype(np.float32).view(np.uint32)
    scores = {ordered[i]: np.float32(current[i]) for i in np.flatnonzero(differing)}
    result = EventSignalProposal._create(inputs, scores, strengths, {ordered[i] for i in np.flatnonzero(pending)},
        rounds, evaluations, edge_evaluations, tuple(sorted(seeds)))
    object.__setattr__(result, 'strength_receipt', strength_updates)
    object.__setattr__(result, 'strength_storage_dtype', '<f4')
    object.__setattr__(result, '_storage_binding', None)
    return result


def local_moves(offsets, neighbors, weights, maximum_sweeps):
    """Same sequential moves/ties/reductions; unbox invariant adjacency once.

    Unlike synchronous or warm-start sweeps this preserves the reference labels,
    membership and topology digest, including aggregation at subsequent levels.
    """
    size = len(offsets)-1
    degree = np.asarray([weights[offsets[i]:offsets[i+1]].sum() for i in range(size)])
    mass = float(degree.sum())
    labels, totals = list(range(size)), degree.tolist()
    if not mass:
        return np.asarray(labels), True, 0
    adjacency = [tuple((int(n), float(w)) for n,w in zip(neighbors[offsets[i]:offsets[i+1]],weights[offsets[i]:offsets[i+1]]) if n != i) for i in range(size)]
    degrees = degree.tolist()
    tolerance = 64*np.finfo(float).eps*max(mass,1.)
    for sweep in range(maximum_sweeps):
        moved = False
        for node, adjacent in enumerate(adjacency):
            d = degrees[node]
            if not d:
                continue
            into = {}
            for n, w in adjacent:
                group = labels[n]
                into[group] = into.get(group,0.) + w
            old = labels[node]
            totals[old] -= d
            best, best_score = old, into.get(old,0.) - d*totals[old]/mass
            for group in sorted(into):
                candidate = into[group] - d*totals[group]/mass
                if candidate > best_score + tolerance:
                    best, best_score = group, candidate
            totals[best] += d
            labels[node] = best
            moved |= best != old
        if not moved:
            return _canonical(np.asarray(labels)), True, sweep+1
    return _canonical(np.asarray(labels)), False, maximum_sweeps
