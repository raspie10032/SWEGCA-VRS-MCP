"""Anonymous familiarity -> regions -> portal -> one Recall/Replay/Re-evidence.

Main supplies a pinned pair, its derived index and evidence judge. Navigation
expands cue addressability, never facts, beliefs or authority. This is not a
complete transitive graph search or operating-service installation.
"""
from dataclasses import dataclass
from math import fsum
from time import perf_counter_ns

from .mosaic_memory_activation import (
    DejaVuSignal, MemoryActivationReceipt, detect_deja_vu, recall_memory,
    replay_memory, re_evidence_memory,
)
from .mosaic_vrs_portal_lifecycle import PortalPlan, PortalCandidate, plan_portals
from .mosaic_vrs_local_navigation import RegionNavigationPage, start_region_navigation, next_region_cues


@dataclass(frozen=True)
class RegionPreactivation:
    pair_snapshot_id: str
    topology_id: str
    signal: DejaVuSignal
    regions: tuple[tuple[int, float], ...]
    matched_term_count: int
    memory_identifiers_exposed: bool = False
    grants_authority: bool = False


def preactivate_regions(pair, *, topology, signal):
    """Only semantic-key/membership lookups, before any episode content read."""
    if pair.memory.lookup_requires_io:
        raise ValueError('prepare hot memory before region preactivation')
    if signal.snapshot_id != pair.memory.snapshot_id:
        raise ValueError('anonymous signal belongs to another memory snapshot')
    topology.require_pair(pair)
    if not topology.converged:
        raise ValueError('incomplete region topology')
    nodes = {node for cue in signal.matched_cues for node in topology.source.address_index.term_ids(cue)}
    groups = {}
    for node in sorted(nodes):
        for group, weight in topology.memberships_for_term(node):
            groups[group] = groups.get(group, 0.0) + weight
    total = fsum(groups.values())
    regions = tuple(sorted(((group, weight / total) for group, weight in groups.items()),
                           key=lambda row: (-row[1], row[0]))) if total else ()
    return RegionPreactivation(pair.snapshot_id, topology.topology_id, signal, regions, len(nodes))


@dataclass(frozen=True)
class PortalActivationReceipt:
    pair_snapshot_id: str
    region_preactivation: RegionPreactivation | None
    plans: tuple[PortalPlan, ...]
    selected: PortalCandidate | None
    deferred_regions: tuple[int, ...]
    navigation_cues: tuple[str, ...]
    navigation_failures: tuple[tuple[int | None, str], ...]
    activation: MemoryActivationReceipt
    elapsed_ns: tuple[tuple[str, int], ...]
    navigation_page: RegionNavigationPage | None = None
    complete_transitive_search: bool = False
    action_authorized: bool = False
    persistent_write_authorized: bool = False


def activate_with_portals(pair, *, topology, associations, policy, observed_at_ns,
                          query, current_cues, judge, navigation_term_budget, revocations=()):
    """Use one immutable hot generation; a rejected shortcut is not lost memory.

    Regions are visited in anonymous cue-membership order until one eligible
    source-bound portal is available. Deferred origins remain explicit, not a
    claim of exhaustive exploration. All ordinary matched cues survive. No
    catch covers Recall/Replay/evidence-judge failures.
    """
    if pair.memory.lookup_requires_io:
        raise ValueError('prepare hot memory before portal activation')
    if type(navigation_term_budget) is not int or navigation_term_budget <= 0:
        raise ValueError('explicit positive navigation term work budget required')
    began = perf_counter_ns()
    signal = detect_deja_vu(pair.memory, query=query, current_cues=current_cues)
    familiar = perf_counter_ns()
    region_signal, plans, selected, deferred, navigation, failures = None, [], None, (), (), []
    navigation_page = None
    # Materialize once for multiple origin attempts, not an exhausted generator.
    revocations = tuple(revocations)
    try:
        if topology is None or associations is None:
            raise ValueError('portal topology or prepared associations unavailable')
        region_signal = preactivate_regions(pair, topology=topology, signal=signal)
        for position, (origin, _) in enumerate(region_signal.regions):
            try:
                source = associations.query(pair, topology=topology, origin_region=origin)
                plan = plan_portals(source, policy=policy, observed_at_ns=observed_at_ns,
                                    revocations=revocations)
                plans.append(plan)
                if plan.selected is None:
                    continue
                candidate = plan.selected
                # Last lineage check protects callers reusing a stale proposal.
                current = topology.bridge_for_episode(pair, candidate.key.episode_id)
                if current != candidate.association.bridge:
                    raise ValueError('portal original changed before recall')
                groups = dict(current.memberships)
                if origin not in groups or candidate.key.destination_region not in groups:
                    raise ValueError('portal original does not join requested regions')
                cursor = start_region_navigation(pair, topology=topology, bridge=current,
                    origin_region=origin, destination_region=candidate.key.destination_region)
                navigation_page = next_region_cues(pair, topology=topology,
                    cursor=cursor, work_budget=navigation_term_budget)
                navigation = navigation_page.cues
                selected = candidate
                deferred = tuple(region for region, _ in region_signal.regions[position + 1:])
                break
            except (ValueError, KeyError) as exc:
                failures.append((origin, str(exc)))
    except (ValueError, KeyError) as exc:
        failures.append((None, str(exc)))
    navigated = perf_counter_ns()
    recalled = recall_memory(pair.memory, signal, navigation_cues=navigation)
    recalled_at = perf_counter_ns()
    replayed = replay_memory(pair.memory, recalled)
    replayed_at = perf_counter_ns()
    evaluated = re_evidence_memory(replayed, judge=judge)
    finished = perf_counter_ns()
    activation = MemoryActivationReceipt('rozephine-memory-activation-v1', pair.memory.snapshot_id,
                                        signal, recalled, replayed, evaluated)
    return PortalActivationReceipt(pair.snapshot_id, region_signal, tuple(plans), selected,
        deferred, navigation, tuple(failures), activation, (
            ('deja_vu', familiar - began), ('navigation', navigated - familiar),
            ('recall', recalled_at - navigated), ('replay', replayed_at - recalled_at),
            ('re_evidence', finished - replayed_at), ('total', finished - began)), navigation_page)


def activate_main_with_portals(controller, *, policy, observed_at_ns, query,
                               current_cues, judge, navigation_term_budget, revocations=()):
    """Trusted main entry point; no cold index bootstrap or model/action dispatch.

    The trusted evidence callback receives immutable replay, outside the main
    lock. Association reads acquire the existing generation lock. A final CAS
    check rejects an obsolete result, including changes during the callback.
    Operational request dispatch and lifecycle persistence are separate work.
    """
    from .mosaic_vrs_region_publication import read_region_generation
    from .mosaic_vrs_coactivation_navigation import query_main_coactivation_associations
    pinned = read_region_generation(controller)
    owner = controller._owner

    class MainAssociations:
        def query(self, pair, *, topology, origin_region):
            if pair is not pinned.pair or topology is not pinned.topology:
                raise ValueError('portal query changed the pinned generation')
            return query_main_coactivation_associations(controller, pinned=pinned,
                                                       origin_region=origin_region)

    result = activate_with_portals(pinned.pair, topology=pinned.topology,
        associations=MainAssociations(), policy=policy, observed_at_ns=observed_at_ns,
        query=query, current_cues=current_cues, judge=judge,
        navigation_term_budget=navigation_term_budget, revocations=revocations)
    with controller._lock:
        if (controller._owner is not owner or owner.snapshot() is not pinned.pair
                or controller._runtime is not pinned.runtime
                or getattr(controller, '_vrs_region_binding', None) is not pinned.binding):
            raise ValueError('main generation changed during portal activation')
    return result
