"""Resumable cue navigation over an existing region, without eager expansion.

Pages are scheduling units, not memory shards or cognitive access limits. Main
retains the original topology/experience; a cursor owns no belief or authority.
Term work does not bound how many episode postings a single broad cue may have.
"""
from dataclasses import dataclass, replace
from bisect import bisect_right

from .mosaic_vrs_connectivity_regions import SharedExperienceBridge


@dataclass(frozen=True)
class RegionNavigationCursor:
    bridge: SharedExperienceBridge
    origin_region: int
    destination_region: int
    seed_nodes: tuple[int, ...]
    seed_offset: int = 0
    region_offset: int = 0
    emitted_terms: int = 0


@dataclass(frozen=True)
class RegionNavigationPage:
    cursor: RegionNavigationCursor
    next_cursor: RegionNavigationCursor | None
    term_ids: tuple[int, ...]
    cues: tuple[str, ...]
    examined_terms: int
    remaining_terms: int
    destination_complete: bool
    complete_memory_search: bool = False
    grants_authority: bool = False


def _seeds(pair, topology, bridge, origin, destination):
    topology.require_pair(pair)
    if pair.memory.lookup_requires_io or not topology.converged:
        raise ValueError('current converged hot region required')
    if (type(origin) is not int or type(destination) is not int or origin == destination
            or bridge.grants_authority):
        raise ValueError('non-authoritative region transition required')
    current = topology.bridge_for_episode(pair, bridge.episode_id)
    if current != bridge:
        raise ValueError('shared original or navigation generation changed')
    groups = dict(bridge.memberships)
    if origin not in groups or destination not in groups:
        raise ValueError('shared original does not join requested regions')
    episode = pair.memory.episode(bridge.episode_id)
    nodes = {node for cue in episode.cues for node in topology.source.address_index.term_ids(cue)}
    # Direct destination-core terms from the shared original lead. All other
    # destination terms remain reachable through the indexed continuation.
    return tuple(sorted(node for node in nodes if int(topology.core_labels[node]) == destination))


def start_region_navigation(pair, *, topology, bridge, origin_region, destination_region):
    seeds = _seeds(pair, topology, bridge, origin_region, destination_region)
    return RegionNavigationCursor(bridge, origin_region, destination_region, seeds)


def next_region_cues(pair, *, topology, cursor, work_budget):
    """At most work_budget indexed visits; do not scan/rank a full destination.

    A page can be empty when skipping a previously emitted seed, but its cursor
    still advances. Exhausting all pages equals the complete destination cue set;
    it does not itself prove completed Recall, transitive search or cognition.
    Cursor values are main-owned continuation state, not authentication tokens.
    """
    if type(work_budget) is not int or work_budget <= 0:
        raise ValueError('explicit positive integer navigation work budget required')
    if not isinstance(cursor, RegionNavigationCursor):
        raise ValueError('typed region cursor required')
    seeds = _seeds(pair, topology, cursor.bridge, cursor.origin_region, cursor.destination_region)
    terms = topology.region_terms[cursor.destination_region]
    if (seeds != cursor.seed_nodes
            or type(cursor.seed_offset) is not int or not 0 <= cursor.seed_offset <= len(seeds)
            or type(cursor.region_offset) is not int or not 0 <= cursor.region_offset <= len(terms)
            or type(cursor.emitted_terms) is not int or not 0 <= cursor.emitted_terms <= len(terms)):
        raise ValueError('navigation cursor content changed')
    if cursor.region_offset and cursor.seed_offset != len(seeds):
        raise ValueError('region cursor skipped its shared-original seed phase')
    skipped = bisect_right(seeds, int(terms[cursor.region_offset - 1])) if cursor.region_offset else 0
    if cursor.emitted_terms != cursor.seed_offset + cursor.region_offset - skipped:
        raise ValueError('navigation cursor progress count changed')
    seed_offset, offset, emitted = cursor.seed_offset, cursor.region_offset, cursor.emitted_terms
    nodes, visited, seed_set = [], 0, set(seeds)
    while visited < work_budget and emitted < len(terms):
        if seed_offset < len(seeds):
            node = seeds[seed_offset]
            seed_offset += 1
        elif offset < len(terms):
            node = int(terms[offset])
            offset += 1
            if node in seed_set:
                visited += 1
                continue
        else:
            break
        visited += 1
        nodes.append(node)
        emitted += 1
    done = emitted == len(terms)
    successor = None if done else replace(cursor, seed_offset=seed_offset,
        region_offset=offset, emitted_terms=emitted)
    return RegionNavigationPage(cursor, successor, tuple(nodes),
        tuple(topology.terms[node] for node in nodes), visited, len(terms) - emitted, done)
