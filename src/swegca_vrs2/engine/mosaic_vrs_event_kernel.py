"""Experimental event-local VRS arithmetic; NOT the legacy shuffle algorithm.

Inputs are cold-bound immutable main assets. A call returns a detached sparse
proposal (including pending work), never a new authoritative memory snapshot.
Endpoint influence here is numerical dependency, not a logical implication.
No production adapter opts into this version automatically.
"""
from __future__ import annotations

import math
import operator
import re
from dataclasses import dataclass
from types import MappingProxyType

import numpy as np

from .mosaic_vrs_address_index import _require_immutable_edges
from .mosaic_vrs_dependency_index import EndpointDependencyIndex
from .mosaic_vrs_state_update import (
    VRS_STABLE_REINFORCEMENT_FACTOR,
    VRS_UNSTABLE_WEAKENING_FACTOR,
)

VERSION = 'vrs-event-synchronous-f32-v1-experimental'


def _immutable_vector(value, size, dtype):
    if type(value) is not np.ndarray or value.shape != (size,) or value.dtype != dtype:
        raise ValueError('event input shape or dtype changed')
    owner = value
    while isinstance(owner, np.ndarray):
        if owner.flags.writeable:
            raise ValueError('event inputs must be immutable')
        owner = owner.base
    if not isinstance(owner, bytes):
        raise ValueError('event inputs need immutable backing storage')
    if not np.isfinite(value).all():
        raise ValueError('event input contains nonfinite values')


@dataclass(frozen=True, eq=False)
class EventVrsInputs:
    """Cold validation only; does not copy or reread the full graph per step."""
    snapshot_id: str
    direct: np.ndarray
    score: np.ndarray
    edges: np.ndarray
    strength: np.ndarray
    unresolved: np.ndarray
    dependencies: EndpointDependencyIndex

    def __post_init__(self):
        if re.fullmatch('[0-9a-f]{64}', self.snapshot_id) is None:
            raise ValueError('event inputs need a generation digest')
        _require_immutable_edges(self.edges)
        count = len(self.direct)
        for array in (self.direct, self.score):
            _immutable_vector(array, count, np.dtype('float32'))
        _immutable_vector(self.strength, len(self.edges), np.dtype('float32'))
        _immutable_vector(self.unresolved, count, np.dtype('bool'))
        if ('vrs_strength' not in self.edges.dtype.names
                or not np.isfinite(self.edges['vrs_strength']).all()
                or np.any(self.edges['vrs_strength'] < 0)
                or np.any(self.strength < 0)
                or np.any(~np.isin(self.edges['sign'], (-1, 0, 1)))):
            raise ValueError('event edges or strength changed')
        if len(self.edges) and max(self.edges['source'].max(), self.edges['target'].max()) >= count:
            raise ValueError('event endpoint is outside the node directory')
        if type(self.dependencies) is not EndpointDependencyIndex:
            raise ValueError('event dependency index type changed')
        self.dependencies.require_source(self.edges)
        self._record_bindings()

    def _record_bindings(self):
        from .mosaic_vrs_event_delta import array_binding
        object.__setattr__(self, '_array_bindings', tuple(array_binding(getattr(self, name))
            for name in ('direct', 'score', 'edges', 'strength', 'unresolved')))

    def require_validated_immutable(self):
        from .mosaic_vrs_event_delta import array_binding
        for name, binding in zip(('direct', 'score', 'edges', 'strength', 'unresolved'), self._array_bindings):
            if array_binding(getattr(self, name)) != binding:
                raise ValueError('event validated array metadata changed')
        self.dependencies.require_source(self.edges)
        return self


class EventVrsProposal:
    """Returned work belongs to main; it is not a persistent worker context."""
    __slots__ = ('_inputs', 'scores', 'strengths', 'pending_nodes', 'rounds',
                 'node_evaluations', 'edge_evaluations', 'seed_nodes')

    def __init__(self):
        raise TypeError('use advance_event_vrs')

    def __setattr__(self, name, value):
        raise AttributeError('event proposal is immutable')

    @classmethod
    def _create(cls, inputs, scores, strengths, pending, rounds, nodes, edges, seeds):
        result = object.__new__(cls)
        for name, value in (
            ('_inputs', inputs), ('scores', MappingProxyType(scores)),
            ('strengths', MappingProxyType(strengths)), ('pending_nodes', tuple(sorted(pending))),
            ('rounds', rounds), ('node_evaluations', nodes), ('edge_evaluations', edges),
            ('seed_nodes', seeds),
        ):
            object.__setattr__(result, name, value)
        return result

    def receipt(self):
        return dict(version=VERSION, parent_snapshot_id=self._inputs.snapshot_id,
                    status='pending' if self.pending_nodes else 'event_fixed_point',
                    pending_node_count=len(self.pending_nodes), rounds=self.rounds,
                    node_evaluations=self.node_evaluations, edge_evaluations=self.edge_evaluations,
                    changed_scores=len(self.scores), changed_strengths=len(self.strengths),
                    legacy_numerical_equivalence=False, whole_graph_convergence_claimed=False,
                    logical_implication_claimed=False, cognitive_completion=False,
                    persistent_state_mutated=False, authority_granted=False)


def _f32(value):
    if not math.isfinite(value) or abs(value) > np.finfo(np.float32).max:
        raise ValueError('event arithmetic produced an unrepresentable value')
    return np.float32(value)


def _same(left, right):
    # Preserve storage equivalence including signed zero, not only float ==.
    return np.float32(left).view(np.uint32) == np.float32(right).view(np.uint32)


def advance_event_vrs(inputs, *, changed_nodes=(), previous=None, maximum_rounds=1):
    """Advance complete synchronous rounds; exhaustion returns all pending work.

For each dirty node, recompute its incoming normalized signal from ALL incoming
edges, using pre-round values and math.fsum followed by f32 rounding. Evaluate
all incident edge compatibilities using the simultaneously updated node scores.
Changed scores revisit themselves and outgoing targets; changed strengths revisit
their targets. No global clock strengthens an unrelated component. A fixed point
means only this event has no outstanding dependency changes, not truth or growth.
"""
    if type(inputs) is not EventVrsInputs:
        raise ValueError('cold-bound event inputs required')
    inputs.require_validated_immutable()
    if type(maximum_rounds) is not int or maximum_rounds < 0:
        raise ValueError('event round budget must be a nonnegative integer')
    if previous is not None:
        if type(previous) is not EventVrsProposal or previous._inputs is not inputs or changed_nodes:
            raise ValueError('pending event belongs to a different generation or event')
        scores, strengths = dict(previous.scores), dict(previous.strengths)
        pending, seeds = set(previous.pending_nodes), previous.seed_nodes
        rounds, node_count, edge_count = previous.rounds, previous.node_evaluations, previous.edge_evaluations
    else:
        seeds = tuple(sorted(set(operator.index(n) for n in changed_nodes)))
        if any(n < 0 or n >= len(inputs.score) for n in seeds):
            raise ValueError('event seed is outside the node directory')
        scores, strengths, pending = {}, {}, set(seeds)
        rounds = node_count = edge_count = 0
    edges, index = inputs.edges, inputs.dependencies
    for _ in range(maximum_rounds):
        if not pending:
            break
        score = lambda n: float(scores.get(n, inputs.score[n]))
        strength = lambda e: float(strengths.get(e, inputs.strength[e]))
        next_scores, incident, outgoing = {}, set(), {}
        for node in sorted(pending):
            incoming = index.edges_for(node, direction='incoming')
            outgoing[node] = index.edges_for(node, direction='outgoing')
            incident.update(map(int, incoming))
            incident.update(map(int, outgoing[node]))
            degree = max(1.0, math.fsum(abs(strength(int(e))) for e in incoming))
            signal = math.fsum(score(int(edges['source'][e])) * int(edges['sign'][e]) * strength(int(e))
                               for e in incoming)
            candidate = math.tanh(float(inputs.direct[node]) + .2 * signal / degree)
            next_scores[node] = _f32(.8 * score(node) + .2 * candidate)
        next_strengths = {}
        for edge in sorted(incident):
            source, target = int(edges['source'][edge]), int(edges['target'][edge])
            left = float(next_scores.get(source, scores.get(source, inputs.score[source])))
            right = float(next_scores.get(target, scores.get(target, inputs.score[target])))
            stable = (1.0 - .5 * abs(left * int(edges['sign'][edge]) - right) >= .75
                      and abs(left) + abs(right) >= .1
                      and not (inputs.unresolved[source] or inputs.unresolved[target]))
            factor = VRS_STABLE_REINFORCEMENT_FACTOR if stable else VRS_UNSTABLE_WEAKENING_FACTOR
            base = float(edges['vrs_strength'][edge])
            next_strengths[edge] = _f32(max(base * .25, min(strength(edge) * factor, base * 4.0)))
        next_pending = set()
        for node, value in next_scores.items():
            if not _same(value, score(node)):
                next_pending.add(node)
                next_pending.update(int(edges['target'][e]) for e in outgoing[node])
            if _same(value, inputs.score[node]):
                scores.pop(node, None)
            else:
                scores[node] = value
        for edge, value in next_strengths.items():
            if not _same(value, strength(edge)):
                next_pending.add(int(edges['target'][edge]))
            if _same(value, inputs.strength[edge]):
                strengths.pop(edge, None)
            else:
                strengths[edge] = value
        node_count += len(pending)
        edge_count += len(incident)
        pending = next_pending
        rounds += 1
    return EventVrsProposal._create(inputs, scores, strengths, pending, rounds, node_count, edge_count, seeds)
