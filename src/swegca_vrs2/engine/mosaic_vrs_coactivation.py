"""Main-owned observations of actual memory activation, not belief updates.

One event is a hyperedge of all activated experiences. We do not manufacture
O(k**2) pair relations, success/usefulness scores, or new observed experiences.
"""
from dataclasses import dataclass, fields, replace
from time import perf_counter_ns

from .mosaic_memory_activation import MemoryActivationReceipt, MemoryStep, activate_memory
from .mosaic_resident_observation_tree import PackedResidentObservation
from .mosaic_vrs_region_publication import read_region_generation
from .mosaic_vrs_membership_cache import read_membership_cache
from .mosaic_packed_memberships import PackedMemberships


_STEP_METADATA = tuple(f.name for f in fields(MemoryStep) if f.name != 'observation')


class _CoactivationCost:
    """Optional request-local cost partition; never evidence or authority.

    One clock read at each boundary, no payload copies or global counters.
    Bookkeeping/GC/scheduling time belongs to the active stage, not CPU time.
    Nested reports overlap their parent and must not be added to it.
    """
    def __init__(self, report, stage):
        if type(report) is not dict or report:
            raise ValueError('empty coactivation timing dictionary required')
        self.report = report
        self.stages = {}
        self.stage = stage
        self.started = self.marked = perf_counter_ns()
        report.update(schema='rozephine-coactivation-cost-v1', stages_ns=self.stages,
                      complete=False, grants_authority=False,
                      includes_instrumentation_overhead=True)

    def step(self, stage):
        now = perf_counter_ns()
        self.stages[self.stage] = self.stages.get(self.stage, 0) + now-self.marked
        self.marked, self.stage = now, stage

    def finish(self, complete):
        active = self.stage
        self.step(None)
        self.report.update(elapsed_ns=self.marked-self.started, complete=complete,
                           failed_stage=None if complete else active)


def _same_original_steps(left, right):
    """Reuse exact admitted storage identity, not declared IDs or factual truth.

    Directory reads create detached views of the SAME immutable observation.
    Comparing those views as generic mappings needlessly decodes every field.
    Unknown/different storage retains the ordinary content comparison. This
    is a main-only structural lineage check; it grants no semantic authority.
    """
    if left is right:
        return True
    if type(left) is not tuple or type(right) is not tuple:
        return left == right
    if len(left) != len(right):
        return False
    for a, b in zip(left, right):
        if a is b:
            continue
        if type(a) is not MemoryStep or type(b) is not MemoryStep:
            if a != b:
                return False
            continue
        if any(getattr(a, name) != getattr(b, name) for name in _STEP_METADATA):
            return False
        x, y = a.observation, b.observation
        shared = (type(x) is PackedResidentObservation and type(y) is PackedResidentObservation
            and x._blob is y._blob and x._externals is y._externals
            and x._start == y._start and x._stop == y._stop)
        if not shared and x != y:
            return False
    return True


@dataclass(frozen=True)
class CoactivatedExperience:
    episode_id: str
    revision: str
    source_addresses: tuple[str, ...]
    outcomes: tuple[str, ...]
    memberships: tuple[tuple[int, float], ...] | PackedMemberships
    proposition: str
    current_verdict: str


@dataclass(frozen=True)
class CoactivationEvent:
    request_id: str
    observed_at_ns: int
    pair_snapshot_id: str
    memory_snapshot_id: str
    vrs_snapshot_id: str
    topology_id: str | None
    query: str
    current_cues: tuple[str, ...]
    experiences: tuple[CoactivatedExperience, ...]
    should_abstain: bool
    grants_authority: bool = False


def _event(pinned, activation, request_id, observed_at_ns, *, _membership_cache=None,
           _original_cues=None, _cost=None):
    if _cost is not None: _cost.step('event_validation')
    if not isinstance(request_id, str) or not request_id.strip():
        raise ValueError('main-issued request identity required')
    if type(observed_at_ns) is not int or observed_at_ns < 0:
        raise ValueError('nonnegative integer nanosecond observation time required')
    if not isinstance(activation, MemoryActivationReceipt):
        raise TypeError('actual four-stage activation receipt required')
    pair, topology = pinned.pair, pinned.topology
    if activation.snapshot_id != pair.memory.snapshot_id:
        raise ValueError('activation does not belong to pinned memory')
    if topology is not None:
        topology.require_pair(pair)
    if _membership_cache is not None:
        _membership_cache.require(pair, topology)
    candidates = activation.recall.candidates
    identifiers = {c.episode_id for c in candidates}
    replayed = {r.episode_id: r for r in activation.replay.episodes}
    judgments = {r.episode_id: r for r in activation.re_evidence.judgments}
    if (len(identifiers) != len(candidates) or set(replayed) != identifiers
            or len(replayed) != len(activation.replay.episodes) or set(judgments) != identifiers):
        raise ValueError('activation stages do not describe the same complete candidate set')
    rows = []
    for candidate in candidates:
        if _cost is not None: _cost.step('original_episode_read')
        episode = pair.memory.episode(candidate.episode_id)
        if _cost is not None: _cost.step('original_lineage_check')
        replay = replayed[candidate.episode_id]
        outcomes = tuple(s.outcome for s in episode.steps)
        if (candidate.revision != episode.revision or replay.source_addresses != episode.source_addresses
                or candidate.historical_outcomes != outcomes
                or not _same_original_steps(replay.steps, episode.steps)):
            raise ValueError('activation original experience lineage changed')
        verdict = judgments[episode.episode_id]
        if _cost is not None: _cost.step('membership_lookup')
        memberships = (_membership_cache.memberships(episode) if _membership_cache is not None
                       else topology.memberships_for_episode(episode) if topology is not None else ())
        if _cost is not None: _cost.step('experience_row_and_cues')
        rows.append(CoactivatedExperience(episode.episode_id, episode.revision,
            episode.source_addresses, outcomes, memberships,
            verdict.proposition, verdict.verdict))
        if _original_cues is not None:
            _original_cues[episode.episode_id] = episode.cues
    if _cost is not None:
        _cost.step('event_construction')
        _cost.report['experience_count'] = len(rows)
    return CoactivationEvent(request_id, observed_at_ns, pair.snapshot_id,
        pair.memory.snapshot_id, pair.vrs_snapshot_id,
        topology.topology_id if topology is not None else None,
        activation.recall.query, activation.deja_vu.current_cues, tuple(rows),
        activation.re_evidence.should_abstain)


def record_coactivation(controller, *, pinned, activation, request_id, observed_at_ns,
                        _original_cues=None, timings_ns=None):
    """Trusted main-only receipt ingestion. False means an identical retry.

This records an execution observation. It cannot change VRS strengths, promote
evidence, authorize actions, or certify a worker-supplied statement as fact.
"""
    cost = _CoactivationCost(timings_ns, 'cache_access') if timings_ns is not None else None
    complete = False
    try:
        result = _record_coactivation(controller, pinned=pinned, activation=activation,
            request_id=request_id, observed_at_ns=observed_at_ns,
            _original_cues=_original_cues, _cost=cost)
        complete = True
        return result
    finally:
        if cost is not None: cost.finish(complete)


def _record_coactivation(controller, *, pinned, activation, request_id, observed_at_ns,
                         _original_cues, _cost):
    cache = read_membership_cache(controller, pinned)
    event = _event(pinned, activation, request_id, observed_at_ns, _membership_cache=cache,
                   _original_cues=_original_cues, _cost=_cost)
    if _cost is not None: _cost.step('main_lock_wait')
    with controller._lock:
        if _cost is not None: _cost.step('generation_and_retry_check')
        if (controller._owner.snapshot() is not pinned.pair or controller._runtime is not pinned.runtime
                or getattr(controller, '_vrs_region_binding', None) is not pinned.binding):
            raise ValueError('main generation changed before coactivation observation')
        events = getattr(controller, '_vrs_region_coactivation_events', None)
        previous = events.get(request_id) if events is not None else None
        if previous is not None:
            if previous != replace(event, observed_at_ns=previous.observed_at_ns):
                raise ValueError('request identity reused for a different activation')
            associations = getattr(controller, '_vrs_coactivation_associations', None)
            if _cost is not None: _cost.step('association_index_update')
            if associations is not None:
                associations.add(previous)
            return previous, False
        if events is None:
            events = {}
            controller._vrs_region_coactivation_events = events
        if _cost is not None: _cost.step('journal_insert')
        events[request_id] = event
        associations = getattr(controller, '_vrs_coactivation_associations', None)
        if _cost is not None: _cost.step('association_index_update')
        if associations is not None:
            # Keep the actual observation even if derived indexing fails. An
            # identical retry can repair that delta without recounting it.
            associations.add(event)
        return event, True


def activate_with_coactivation(controller, *, request_id, observed_at_ns, query, current_cues, judge):
    """Run unchanged full-memory activation and observe its actual candidates.

No event is stored if activation/judging fails. A stale completed observation
raises explicitly rather than writing into a different current generation.
"""
    pinned = read_region_generation(controller)
    activation = activate_memory(pinned.pair.memory, query=query, current_cues=current_cues, judge=judge)
    event, recorded = record_coactivation(controller, pinned=pinned, activation=activation,
                                         request_id=request_id, observed_at_ns=observed_at_ns)
    return activation, event, recorded
