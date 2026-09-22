"""Derived hot association postings over main-owned activation observations.

This is a coactivation component, not a complete group-synapse score. Bootstrap
once from the main journal, then add only newly recorded events. A caller must
serialize add/query with main's generation boundary. No models, IO, hashes,
truth scores, history pruning, or pairwise graph materialization live here.
"""
from dataclasses import dataclass
from math import isfinite

from .mosaic_memory_activation import OUTCOMES
from .mosaic_vrs_coactivation import CoactivationEvent, CoactivatedExperience
from .mosaic_vrs_connectivity_regions import SharedExperienceBridge
from .mosaic_vrs_membership_cache import membership_cache_locked


@dataclass(frozen=True)
class CoactivationWitness:
    event: CoactivationEvent
    experience: CoactivatedExperience


@dataclass(frozen=True)
class ObservedSharedAssociation:
    origin_region: int
    destination_region: int
    bridge: SharedExperienceBridge
    witnesses: tuple[CoactivationWitness, ...]
    usefulness: None = None
    causal_relevance: None = None
    grants_authority: bool = False

    @property
    def activation_request_count(self):
        # Requests are executions, NOT distinct source episodes or outcomes.
        return len(self.witnesses)


@dataclass(frozen=True)
class RejectedCoactivation:
    request_id: str
    episode_id: str
    reason: str


@dataclass(frozen=True)
class CoactivationAssociationsReceipt:
    pair_snapshot_id: str
    topology_id: str
    origin_region: int
    associations: tuple[ObservedSharedAssociation, ...]
    rejected: tuple[RejectedCoactivation, ...]
    indexed_event_count: int
    relevant_event_count: int
    examined_experience_count: int
    grants_authority: bool = False
    complete_memory_search: bool = False


class CoactivationAssociations:
    """Main-owned derived index; input is trusted typed journal data, not LLM output.

    Postings store shared references to immutable original events/rows. Ingest
    costs O(total memberships in the delta), not O(all historical requests).
    Query touches only the selected topology/region postings. Missing or stale
    associations cannot restrict the ordinary full-current memory path.
    """

    def __init__(self):
        self._events = {}
        self._postings = {}

    @property
    def event_count(self):
        return len(self._events)

    def add(self, event: CoactivationEvent) -> bool:
        if not isinstance(event, CoactivationEvent) or event.grants_authority:
            raise ValueError('non-authoritative typed main observation required')
        if (not isinstance(event.request_id, str) or not event.request_id.strip()
                or type(event.observed_at_ns) is not int or event.observed_at_ns < 0):
            raise ValueError('valid request identity and integer nanoseconds required')
        previous = self._events.get(event.request_id)
        if previous is not None:
            if previous != event:
                raise ValueError('request identity reused for a different historical event')
            return False
        # Prepare all rows before changing any index. Failed/negative/pending
        # labels participate exactly like success; no usefulness inferred.
        prepared = {}
        seen = set()
        for row in event.experiences:
            if not isinstance(row, CoactivatedExperience) or row.episode_id in seen:
                raise ValueError('unique typed original experiences required')
            seen.add(row.episode_id)
            if any(outcome not in OUTCOMES for outcome in row.outcomes):
                raise ValueError('unknown original outcome')
            groups = set()
            witness = None
            for region, weight in row.memberships:
                if (type(region) is not int or region < 0 or region in groups
                        or type(weight) not in (float, int)
                        or not isfinite(weight) or weight <= 0):
                    raise ValueError('valid unique positive membership required')
                groups.add(region)
                # Memberships need separate postings, not duplicate frozen
                # event/row wrappers. Keep ungrouped originals allocation-free.
                if witness is None:
                    witness = CoactivationWitness(event, row)
                prepared.setdefault((event.topology_id, region), []).append(witness)
            if groups and event.topology_id is None:
                raise ValueError('memberships require a recorded topology')
        self._events[event.request_id] = event
        for key, witnesses in prepared.items():
            self._postings.setdefault(key, {})[event.request_id] = tuple(witnesses)
        return True

    def query(self, pair, *, topology, origin_region: int, _membership_cache=None) -> CoactivationAssociationsReceipt:
        topology.require_pair(pair)
        if _membership_cache is not None:
            _membership_cache.require(pair, topology)
        if pair.memory.lookup_requires_io:
            raise ValueError('prepare hot memory before association lookup')
        if not topology.converged:
            raise ValueError('converged topology required')
        if (type(origin_region) is not int or not 0 <= origin_region < len(topology.region_terms)
                or len(topology.region_terms[origin_region]) == 0):
            raise ValueError('existing integer origin region required')
        postings = self._postings.get((topology.topology_id, origin_region), {})
        accepted, bridges, rejected = {}, {}, []
        examined = 0
        # Revalidate one shared original once, even when many requests cite it.
        current = {}
        for rows in postings.values():
            for witness in rows:
                examined += 1
                event, row = witness.event, witness.experience
                reason = None
                if (event.pair_snapshot_id != pair.snapshot_id
                        or event.memory_snapshot_id != pair.memory.snapshot_id
                        or event.vrs_snapshot_id != pair.vrs_snapshot_id):
                    reason = 'historical_snapshot_not_current'
                else:
                    if row.episode_id not in current:
                        try:
                            episode = pair.memory.episode(row.episode_id)
                            bridge = (_membership_cache.bridge(episode) if _membership_cache is not None
                                      else topology.bridge_for_episode(pair, row.episode_id))
                            current[row.episode_id] = (episode, bridge)
                        except KeyError:
                            current[row.episode_id] = (None, None)
                    episode, bridge = current[row.episode_id]
                    if episode is None:
                        reason = 'original_address_unavailable'
                    elif (episode.revision != row.revision
                            or episode.source_addresses != row.source_addresses
                            or tuple(step.outcome for step in episode.steps) != row.outcomes):
                        reason = 'original_lineage_changed'
                    elif bridge is None:
                        reason = 'not_a_shared_original_experience'
                    elif bridge.memberships != row.memberships:
                        reason = 'recorded_memberships_changed'
                    elif origin_region not in dict(bridge.memberships):
                        reason = 'origin_not_in_shared_experience'
                    else:
                        for destination, _ in bridge.memberships:
                            if destination == origin_region:
                                continue
                            key = (destination, bridge.episode_id, bridge.revision)
                            accepted.setdefault(key, []).append(witness)
                            bridges[key] = bridge
                if reason:
                    rejected.append(RejectedCoactivation(event.request_id, row.episode_id, reason))
        associations = tuple(sorted((ObservedSharedAssociation(
            origin_region, key[0], bridges[key],
            tuple(sorted(rows, key=lambda w: w.event.request_id)))
            for key, rows in accepted.items()), key=lambda a: (
                -a.activation_request_count, a.destination_region,
                a.bridge.episode_id, a.bridge.revision)))
        return CoactivationAssociationsReceipt(pair.snapshot_id, topology.topology_id,
            origin_region, associations, tuple(sorted(rejected,
                key=lambda r: (r.request_id, r.episode_id, r.reason))),
            len(self._events), len(postings), examined)


def install_coactivation_associations(controller):
    """One cold bootstrap from the main journal, published only if still current.

    Requires record_coactivation's delta hook before installation. Main retains
    the journal; this index owns no original cognition or independent history.
    """
    with controller._lock:
        existing = getattr(controller, '_vrs_coactivation_associations', None)
        if existing is not None:
            return existing
        events = getattr(controller, '_vrs_region_coactivation_events', None)
        observed = tuple(events.values()) if events is not None else ()
    index = CoactivationAssociations()
    for event in observed:
        index.add(event)
    with controller._lock:
        if (getattr(controller, '_vrs_coactivation_associations', None) is not None
                or getattr(controller, '_vrs_region_coactivation_events', None) is not events
                or (len(events) if events is not None else 0) != len(observed)):
            raise ValueError('main journal changed during association bootstrap; retry cold setup')
        controller._vrs_coactivation_associations = index
    return index


def query_main_coactivation_associations(controller, *, pinned, origin_region):
    """Read a consistent hot association view; no implicit cold bootstrap."""
    with controller._lock:
        if (controller._owner.snapshot() is not pinned.pair
                or controller._runtime is not pinned.runtime
                or getattr(controller, '_vrs_region_binding', None) is not pinned.binding):
            raise ValueError('main generation changed before association lookup')
        index = getattr(controller, '_vrs_coactivation_associations', None)
        events = getattr(controller, '_vrs_region_coactivation_events', None)
        if index is None or index.event_count != (len(events) if events is not None else 0):
            raise ValueError('association index requires cold preparation or missing-delta repair')
        if pinned.topology is None:
            raise ValueError('association topology unavailable; use ordinary memory activation')
        return index.query(pinned.pair, topology=pinned.topology, origin_region=origin_region,
                           _membership_cache=membership_cache_locked(controller, pinned))
