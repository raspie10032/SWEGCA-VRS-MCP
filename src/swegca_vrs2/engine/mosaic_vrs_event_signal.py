"""Re-evidence strength proposal followed by event-local signal settlement.

Experimental, explicit version: no compatibility-driven self-reinforcement.
The existing main re-evidence proposal changes strength once for this snapshot;
subsequent numerical iterations propagate that fixed input, not new evidence.
No commit, durable publication, logical inference, or authority grant is here.
"""
from __future__ import annotations

import math
import operator
import struct

from .mosaic_vrs_event_kernel import EventVrsInputs, EventVrsProposal, _f32, _same
from .mosaic_vrs_state_update import (
    VRSStateUpdateReceipt, VRSConnectionStateUpdate,
    VRS_STABLE_REINFORCEMENT_FACTOR, VRS_UNSTABLE_WEAKENING_FACTOR,
)

VERSION = 'vrs-re-evidence-event-signal-f32-v2-experimental'


class EventSignalProposal(EventVrsProposal):
    __slots__ = ('strength_receipt', 'strength_storage_dtype', '_storage_binding')

    def receipt(self):
        result = super().receipt()
        result.update(version=VERSION,
                      status='pending' if self.pending_nodes else 'signal_fixed_point',
                      strength_updates_reapplied_during_iterations=0,
                      numerical_compatibility_controls_strength=False,
                      strength_storage_dtype=self.strength_storage_dtype,
                      storage_binding_verified=self._storage_binding is not None,
                      input_strength_proposal_count=0 if self.strength_receipt is None else len(self.strength_receipt.updates))
        return result


def _stored_strength(value, dtype):
    """Round before arithmetic; rounding alone cannot promote an experience."""
    if dtype not in ('<f4', '<f2'):
        raise ValueError('unsupported event strength storage dtype')
    result = _f32(value)
    if dtype == '<f2':
        try:
            result = _f32(struct.unpack('<e', struct.pack('<e', float(result)))[0])
        except (OverflowError, struct.error) as exc:
            raise ValueError('event strength exceeds durable f16 capacity') from exc
        if (value >= 1.0) != (result >= 1.0):
            raise ValueError('durable f16 rounding changes experience promotion')
    return result


def _bind_strength_updates(inputs, receipt, namespace, dtype='<f4'):
    if receipt is None:
        return {}, set()
    if (type(receipt) is not VRSStateUpdateReceipt or receipt.snapshot_id != inputs.snapshot_id
            or type(receipt.updates) is not tuple):
        raise ValueError('re-evidence proposal generation changed')
    if namespace not in ('vrs-edge:', 'vrs-edge-group:'):
        raise ValueError('explicit connection namespace required')
    strengths, seeds = {}, set()
    seen = set()
    for row in receipt.updates:
        if type(row) is not VRSConnectionStateUpdate or not row.connection_id.startswith(namespace):
            raise ValueError('re-evidence connection namespace changed')
        identifier = row.connection_id[len(namespace):]
        if not identifier.isascii() or not identifier.isdecimal():
            raise ValueError('invalid connection address')
        edge = int(identifier)
        if edge in seen or not 0 <= edge < len(inputs.edges):
            raise ValueError('duplicate or unknown connection address')
        seen.add(edge)
        if row.previous_strength != float(inputs.strength[edge]):
            raise ValueError('re-evidence strength does not match immutable input')
        expected = row.previous_strength
        if row.update_action == 'reinforce' and row.verdict == 'support':
            expected *= VRS_STABLE_REINFORCEMENT_FACTOR
        elif row.update_action == 'weaken' and row.verdict == 'refute':
            expected *= VRS_UNSTABLE_WEAKENING_FACTOR
        elif row.update_action == 'abstain_conflict':
            pass
        elif row.update_action == 'preserve_unresolved' and row.verdict not in ('support', 'refute'):
            pass
        else:
            raise ValueError('re-evidence proposal action changed')
        if row.current_strength != expected:
            raise ValueError('re-evidence strength operation changed')
        from .mosaic_memory_promotion import assess_vrs_experience_promotion
        if row.promotion != assess_vrs_experience_promotion(snapshot_id=receipt.snapshot_id,
                connection_id=row.connection_id, previous_strength=row.previous_strength,
                current_strength=expected):
            raise ValueError('re-evidence promotion binding changed')
        value = _stored_strength(expected, dtype)
        if not _same(value, inputs.strength[edge]):
            strengths[edge] = value
            seeds.add(int(inputs.edges['target'][edge]))
    return strengths, seeds


def settle_event_signal(inputs, *, changed_nodes=(), strength_updates=None,
                        connection_namespace='vrs-edge:', previous=None, maximum_rounds=1,
                        strength_storage_dtype=None):
    """Settle signal changes without re-counting the same strength evidence.

strength_storage_dtype controls changed strengths, not a full-array cast.
BoundEventSignalStorage additionally verifies durable input equality at cold
bootstrap and checks only ingress edits later. Direct calls are not storage-bound.

Every affected node uses all incoming signs and current proposed strengths.
The fixed-input map has a real-valued max-norm Lipschitz upper bound .84
(.8 self carry + .2 update * .2 normalized incoming); finite f32 behavior is
still tested and a round budget remains pending, never a convergence proof.
"""
    if type(inputs) is not EventVrsInputs:
        raise ValueError('cold-bound event inputs required')
    inputs.require_validated_immutable()
    if type(maximum_rounds) is not int or maximum_rounds < 0:
        raise ValueError('signal round budget must be a nonnegative integer')
    dtype = (previous.strength_storage_dtype if type(previous) is EventSignalProposal
             and strength_storage_dtype is None else '<f4' if strength_storage_dtype is None else strength_storage_dtype)
    if dtype not in ('<f4', '<f2'):
        raise ValueError('unsupported event strength storage dtype')
    if previous is not None:
        if (type(previous) is not EventSignalProposal or previous._inputs is not inputs
                or changed_nodes or strength_updates is not None or dtype != previous.strength_storage_dtype):
            raise ValueError('signal resume generation or event changed')
        scores, strengths = dict(previous.scores), dict(previous.strengths)
        pending, seeds = set(previous.pending_nodes), previous.seed_nodes
        rounds, nodes, edges = previous.rounds, previous.node_evaluations, previous.edge_evaluations
        receipt = previous.strength_receipt
    else:
        seeds = set(operator.index(n) for n in changed_nodes)
        if any(n < 0 or n >= len(inputs.score) for n in seeds):
            raise ValueError('signal seed outside node directory')
        strengths, targets = _bind_strength_updates(inputs, strength_updates, connection_namespace, dtype)
        seeds.update(targets)
        seeds = tuple(sorted(seeds))
        scores, pending = {}, set(seeds)
        rounds = nodes = edges = 0
        receipt = strength_updates
    graph, index = inputs.edges, inputs.dependencies
    # Strength evidence is fixed for this event. Reuse numerical dependency
    # reads within this invocation, not across generations or main judgments.
    # The cache grows only as actual affected nodes/incoming sources are read.
    topology, initial_scores = {}, {}
    def initial(node):
        if node not in initial_scores:
            initial_scores[node] = float(inputs.score[node])
        return initial_scores[node]
    for _ in range(maximum_rounds):
        if not pending:
            break
        next_scores, outgoing = {}, {}
        edge_reads = 0
        for node in sorted(pending):
            if node not in topology:
                incoming = index.edges_for(node, direction='incoming')
                terms = []
                for edge in incoming:
                    source = int(graph['source'][edge])
                    weight = float(strengths[int(edge)] if int(edge) in strengths else inputs.strength[edge])
                    terms.append((source, int(graph['sign'][edge]), weight, initial(source)))
                targets = tuple(int(graph['target'][edge]) for edge in index.edges_for(node, direction='outgoing'))
                topology[node] = (tuple(terms), max(1.0, math.fsum(abs(t[2]) for t in terms)),
                                  targets, float(inputs.direct[node]), initial(node))
            incoming, denominator, outgoing[node], direct, original = topology[node]
            signal = math.fsum(float(scores.get(source, baseline)) * sign * weight
                               for source, sign, weight, baseline in incoming)
            old = float(scores.get(node, original))
            next_scores[node] = _f32(.8*old + .2*math.tanh(direct+.2*signal/denominator))
            edge_reads += len(incoming)
        following = set()
        for node,value in next_scores.items():
            if not _same(value,scores.get(node,initial_scores[node])):
                following.add(node)
                following.update(outgoing[node])
            if _same(value,initial_scores[node]): scores.pop(node,None)
            else: scores[node]=value
        rounds += 1; nodes += len(pending); edges += edge_reads
        pending = following
    result = EventSignalProposal._create(inputs,scores,strengths,pending,rounds,nodes,edges,seeds)
    object.__setattr__(result,'strength_receipt',receipt)
    object.__setattr__(result,'strength_storage_dtype',dtype)
    object.__setattr__(result,'_storage_binding',None)
    return result
