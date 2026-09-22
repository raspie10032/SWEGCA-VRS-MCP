"""Main-owned memo of pure group membership, never evidence authority.

No original episode or learned connection is evicted. Only derived tuple results
are retained, with conservative entry charges (not an RSS measurement). Main
configuration is explicit; missing configuration preserves the uncached path.
"""
from collections import OrderedDict
from sys import getsizeof
from threading import Lock

from .mosaic_vrs_connectivity_regions import SharedExperienceBridge


class MembershipCache:
    def __init__(self, pair, topology, *, maximum_entries, maximum_estimated_bytes, _scope=None):
        if any(type(v) is not int or v <= 0 for v in (maximum_entries, maximum_estimated_bytes)):
            raise ValueError('positive explicit membership cache capacities required')
        if topology is None or pair.memory.lookup_requires_io:
            raise ValueError('membership cache requires prepared hot topology')
        topology.require_pair(pair)
        self.pair, self.topology = pair, topology
        self._scope = _scope
        self.maximum_entries, self.maximum_estimated_bytes = maximum_entries, maximum_estimated_bytes
        self._rows, self._lock = OrderedDict(), Lock()
        self.hits = self.misses = self.evictions = self.oversized = self.estimated_bytes = 0

    def require(self, pair, topology):
        if pair is not self.pair or topology is not self.topology:
            raise ValueError('membership cache generation mismatch')

    def memberships(self, episode):
        # Cues are part of the key: matching revision alone cannot hide changed
        # derivation inputs. Other lineage fields are still checked by consumers.
        key = (episode.episode_id, episode.revision, tuple(episode.cues))
        with self._lock:
            row = self._rows.get(key)
            if row is not None:
                self.hits += 1
                self._rows.move_to_end(key)
                return row[0]
            self.misses += 1
            value = self.topology.memberships_for_episode(episode)
            # Charge shared strings/objects too, conservatively. Dict/LRU entry
            # bookkeeping allowance is explicit; this is not exact allocator RSS.
            charge = (256 + getsizeof(key) + getsizeof(key[0]) + getsizeof(key[1])
                + getsizeof(key[2]) + sum(getsizeof(cue) for cue in key[2])
                + getsizeof(value) + sum(getsizeof(item) + sum(getsizeof(v) for v in item) for item in value))
            if charge > self.maximum_estimated_bytes:
                self.oversized += 1
                return value
            while self._rows and (len(self._rows) >= self.maximum_entries
                    or self.estimated_bytes + charge > self.maximum_estimated_bytes):
                _, (_, old_charge) = self._rows.popitem(last=False)
                self.estimated_bytes -= old_charge
                self.evictions += 1
            self._rows[key] = (value, charge)
            self.estimated_bytes += charge
            return value

    def bridge(self, episode):
        memberships = self.memberships(episode)
        if len(memberships) < 2:
            return None
        return SharedExperienceBridge(self.pair.snapshot_id, self.topology.topology_id,
            episode.episode_id, episode.revision, episode.source_addresses,
            tuple(step.outcome for step in episode.steps), memberships)

    def status(self):
        with self._lock:
            return dict(entries=len(self._rows), estimated_bytes=self.estimated_bytes,
                maximum_entries=self.maximum_entries, maximum_estimated_bytes=self.maximum_estimated_bytes,
                hits=self.hits, misses=self.misses, evictions=self.evictions, oversized=self.oversized,
                pair_snapshot_id=self.pair.snapshot_id, topology_id=self.topology.topology_id,
                grants_authority=False)


def _check_locked(controller, pinned):
    """Caller owns controller._lock; never re-acquire it from this helper."""
    if (controller._owner.snapshot() is not pinned.pair or controller._runtime is not pinned.runtime
            or getattr(controller, '_vrs_region_binding', None) is not pinned.binding):
        raise ValueError('main generation changed before membership cache access')


def configure_membership_cache(controller, *, pinned, maximum_entries, maximum_estimated_bytes,
                               strategy='episode', profile_inputs=False):
    """Trusted main configuration only, not a worker/wire memory grant."""
    if type(profile_inputs) is not bool or (profile_inputs and strategy != 'packed_nodes'):
        raise ValueError('input profiling requires explicit packed node strategy')
    options = dict(profile_inputs=True) if profile_inputs else {}
    if strategy == 'episode':
        factory = MembershipCache
        config = (maximum_entries, maximum_estimated_bytes)
    elif strategy == 'packed_nodes':
        # Resolve the reviewed class during configuration, outside the main lock.
        # Rebinding must not import modules or perform cold discovery on a request.
        from .mosaic_vrs_nodeset_membership_cache import NodeSetMembershipCache
        factory = NodeSetMembershipCache
        config = (maximum_entries, maximum_estimated_bytes, factory)
        if profile_inputs:
            config += (True,)
    else:
        raise ValueError('unknown membership cache strategy')
    candidate = factory(pinned.pair, pinned.topology,
        maximum_entries=maximum_entries, maximum_estimated_bytes=maximum_estimated_bytes,
        _scope=(controller._owner, pinned.runtime, pinned.binding), **options)
    with controller._lock:
        _check_locked(controller, pinned)
        if controller._owner is not candidate._scope[0]:
            raise ValueError('main owner changed before membership cache configuration')
        if getattr(controller, '_vrs_membership_cache_config', None) is not None:
            raise ValueError('membership cache already configured')
        controller._vrs_membership_cache_config = config
        controller._vrs_membership_cache = candidate
    return candidate


def membership_cache_locked(controller, pinned):
    """Only inside an existing main lock. No discovery or corpus materialization."""
    _check_locked(controller, pinned)
    config = getattr(controller, '_vrs_membership_cache_config', None)
    if config is None:
        return None
    if pinned.topology is None:
        controller._vrs_membership_cache = None
        return None
    cached = getattr(controller, '_vrs_membership_cache', None)
    scope = (controller._owner, pinned.runtime, pinned.binding)
    factory = config[2] if len(config) >= 3 else MembershipCache
    if (cached is None or type(cached) is not factory
            or cached.pair is not pinned.pair or cached.topology is not pinned.topology
            or cached._scope is None or any(a is not b for a,b in zip(cached._scope,scope))):
        cached = factory(pinned.pair, pinned.topology,
            maximum_entries=config[0], maximum_estimated_bytes=config[1], _scope=scope,
            **(dict(profile_inputs=config[3]) if len(config) == 4 else {}))
        controller._vrs_membership_cache = cached
    return cached


def read_membership_cache(controller, pinned):
    with controller._lock:
        return membership_cache_locked(controller, pinned)


def disable_membership_cache(controller):
    """Remove derived residency only. Existing pinned readers retain their view."""
    with controller._lock:
        controller._vrs_membership_cache_config = None
        controller._vrs_membership_cache = None
