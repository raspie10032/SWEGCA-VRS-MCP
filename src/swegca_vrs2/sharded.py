"""One logical main over complete VRS shard generations.

Storage shards are an implementation boundary.  Cognition still sees one main:
Déjà vu and Recall use global cue fanout, Replay joins the original experiences,
and Re-evidence evaluates propositions across shard boundaries.  Each candidate
keeps the VRS generation, regions and shared-experience portal path of the shard
that owns its original experience.

This module does not create shards.  It prevents the resident storage split from
changing the native MCP's memory semantics while automatic routing and the
cross-shard portal directory are built on top of it.
"""
from __future__ import annotations

import math
import re
import time
from itertools import chain
from types import MappingProxyType

from .engine.mosaic_memory_activation import (
    CurrentEvidenceVerdict,
    DejaVuSignal,
    MemoryActivationReceipt,
    RecallCandidate,
    RecallResult,
    ReplayResult,
    current_experience_verdict,
    detect_deja_vu,
    recall_memory,
    replay_memory,
    re_evidence_memory,
)
from .store import (
    ASK_GATE, ASK_GATE_MAX_HITS, ASK_GATE_RARE_SHARE, DESCRIPTION_GATE,
    PROMOTION_GATE, UNBRIDGED_FACTOR, digest, graph_cue_ids, keys, text_field,
)
from . import vrs_refine


class CompositeIndex:
    """Read-only HotMemoryIndex protocol over pinned shard generations."""

    def __init__(self, owners, snapshot_id):
        self.owners = tuple(owners)
        self.snapshot_id = snapshot_id
        self.episode_count = sum(owner.memory.episode_count for _, owner in self.owners)
        self.semantic_families = tuple(chain.from_iterable(
            owner.memory.semantic_families for _, owner in self.owners))
        self._owner_cache = {}

    def owner_of(self, identifier):
        cached = self._owner_cache.get(identifier)
        if cached is not None:
            return cached
        for shard, owner in self.owners:
            if identifier in owner.memory.records:
                self._owner_cache[identifier] = (shard, owner)
                return shard, owner
        raise KeyError(identifier)

    def episode_ids_for_cue(self, cue):
        ids = {}
        for _, owner in self.owners:
            ids.update(dict.fromkeys(owner.memory.episode_ids_for_cue(cue)))
        return tuple(ids)

    def episode(self, identifier):
        return self.owner_of(identifier)[1].memory.episode(identifier)

    def episode_light(self, identifier):
        memory = self.owner_of(identifier)[1].memory
        return memory.episode_light(identifier) if hasattr(memory, 'episode_light') else memory.episode(identifier)


class ShardedMain:
    """Main-shaped read owner used by ``LocalResident`` for native MCP calls."""

    def __init__(self, primary, resident):
        self.primary, self.resident = primary, resident

    def _owners(self):
        owners = [('main', self.primary)]
        missing = []
        for shard in self.resident.ids():
            owner, state = self.resident.ready(shard)
            if owner is None:
                missing.append((shard, state))
            else:
                owners.append((shard, owner))
        if missing:
            raise ValueError('complete_vrs_shards_not_ready:' + ','.join(
                f'{shard}:{state}' for shard, state in missing))
        return owners

    def _snapshot(self, owners=None):
        return self.resident.logical_snapshot()

    def _check(self):
        self.primary._check()

    def status(self):
        try:
            owners = self._owners()
        except ValueError:
            base = self.primary.status()
            missing = [dict(id=shard, state=state) for shard in self.resident.ids()
                       for owner, state in [self.resident.ready(shard)] if owner is None]
            base.update(memory_ready=False, complete_vrs_shards_ready=False,
                        pair_snapshot_id=self._snapshot(),
                        resource_budget=self.resident.budget(),
                        incomplete_shards=missing,
                        logical_main='complete_vrs_sharded_generation')
            return base
        snapshot = self._snapshot(owners)
        base = self.primary.status()
        counts = {}
        for _, owner in owners:
            for name, count in owner.memory.outcome_counts.items():
                counts[name] = counts.get(name, 0) + count
        base.update(pair_snapshot_id=snapshot,
                    hot_episode_count=sum(owner.memory.episode_count for _, owner in owners),
                    outcome_counts=counts, lookup_requires_io=False,
                    shards=[dict(id=shard, pair_snapshot_id=owner.pair.snapshot_id,
                                 records=owner.memory.episode_count,
                                 stable_version_id=(owner.graph.stable.version_id
                                                    if owner.graph.stable is not None else None))
                            for shard, owner in owners],
                    logical_main='complete_vrs_sharded_generation')
        base['resource_budget'] = self.resident.budget()
        base['memory_ready'] = base['complete_vrs_shards_ready'] = True
        return base

    def _finish_exact(self, query, pair_snapshot, signal, recalled, replayed,
                      shard, through_replay_ns, exclude_kinds, region_scope):
        """Attach current VRS Re-evidence after the direct Replay boundary."""
        finish_began = time.perf_counter_ns()
        owners = self._owners()
        by_shard = dict(owners)
        owner = by_shard.get(shard)
        if owner is None:
            raise ValueError('exact_replay_owner_not_ready')
        episode = replayed.episodes[0]
        proposition = episode.steps[0].observation.get('proposition_id')
        opponents = ()
        if proposition:
            active = []
            for _, candidate_owner in owners:
                for identifier in candidate_owner.memory.propositions.get(proposition, ()):
                    if identifier not in candidate_owner.memory.superseded:
                        active.append(candidate_owner.memory.episode_light(identifier))
            if {item.steps[0].observation.get('evidence_polarity') for item in active} == {'support', 'refute'}:
                opponents = tuple(sorted(item.episode_id for item in active))

        graph_snapshot = digest(('sharded-vrs-v1', [(name, candidate.graph.snapshot_id)
                                                    for name, candidate in owners]))

        def judge(item):
            if opponents:
                return CurrentEvidenceVerdict(item.episode_id, proposition, 'conflict',
                    'Opposing recorded claims for the same explicit proposition across VRS shards; neither is certified true.',
                    ('memory-snapshot:' + pair_snapshot, *item.source_addresses), opponents)
            return current_experience_verdict(item, memory_snapshot_id=pair_snapshot,
                vrs_snapshot_id=graph_snapshot, current_strength=owner.graph.strength(item.episode_id),
                proposition=proposition or 'experience:' + item.episode_id)

        re_evidenced = re_evidence_memory(replayed, judge=judge)
        receipt = MemoryActivationReceipt('rozephine-memory-activation-v1', pair_snapshot,
            signal, recalled, replayed, re_evidenced)
        local = owner.recall(query, None, exclude_kinds=exclude_kinds, region_scope=region_scope)
        identifier = episode.episode_id
        path = dict(local['region_navigation']['paths'].get(identifier)
                    or dict(region=None, path='pending'))
        path['shard'] = shard
        return dict(record_count=sum(candidate.memory.episode_count for _, candidate in owners),
            receipt={'activation': receipt},
            memory_selection=dict(candidate_counts={identifier: 1}, selected_cues=(identifier,),
                rejected_cues=(), function_word_cues=(),
                selection_method='main_owned_exact_replay_capsule', excluded_kinds=list(exclude_kinds or ()),
                closure_rule='direct original experience; proposition opponents evaluated by Re-evidence',
                candidate_order='exact_original_address', semantic_acceptance_claimed=False),
            vrs_selection=dict(selection_method='current_owner_vrs_after_exact_replay',
                numerical_version=vrs_refine.VERSION, logical_implication_claimed=False,
                grants_authority=False,
                stable_version_id=(owner.graph.stable.version_id if owner.graph.stable is not None else None)),
            current_strengths={identifier: owner.graph.strength(identifier)},
            current_promotions={identifier: owner.graph.strength(identifier) >= 1.0},
            current_propositions={identifier: proposition},
            superseded_by={identifier: owner.memory.superseded.get(identifier)},
            region_navigation=dict(paths={identifier: path}, shard_roots={shard: local['region_navigation']},
                                   cross_shard_portals=[], membership_is_truth=False),
            pair_snapshot_id=pair_snapshot,
            timings_ns=dict(through_replay=through_replay_ns,
                            through_re_evidence=through_replay_ns + time.perf_counter_ns() - finish_began),
            authority=MappingProxyType({key: False for key in
                ('world', 'action', 'persistent_write', 'model_update', 'distribution', 'p3')}))

    def recall(self, query, expected_snapshot, exclude_kinds=(), region_scope='all'):
        began = time.perf_counter_ns()
        pair_snapshot = self._snapshot()
        if expected_snapshot is not None and expected_snapshot != pair_snapshot:
            raise ValueError('snapshot_mismatch')
        query = text_field(query, 'query', 4096)
        exact_match = re.fullmatch(r'\s*(memory:[0-9a-f]{64})\s*', query.casefold())
        exact = self.resident.exact_replay(exact_match.group(1)) if exact_match else None
        if exact is not None:
            replay = exact['replay']
            kind = str((replay.steps[0].observation.get('metadata') or {}).get('kind') or '')
            if kind in tuple(exclude_kinds or ()):
                exact = None
        if exact is not None:
            replay = exact['replay']
            signal = DejaVuSignal(pair_snapshot, query, (replay.episode_id,),
                                  (replay.episode_id,), 1.0, 1)
            candidate = RecallCandidate(replay.episode_id, (replay.episode_id,), 1.0,
                                        exact['revision'],
                                        replay.verification_state,
                                        tuple(step.outcome for step in replay.steps))
            recalled = RecallResult(query, (candidate,), pair_snapshot)
            replayed = ReplayResult(query, (replay,))
            through_replay_ns = time.perf_counter_ns() - began
            return self._finish_exact(query, pair_snapshot, signal, recalled, replayed,
                                      exact['shard'], through_replay_ns,
                                      exclude_kinds, region_scope)
        owners = self._owners()
        # Kind masks are immutable shard views.  Pair each view with the shard's
        # complete graph; original addresses and ids do not change.
        views = []
        for shard, owner in owners:
            memory = owner.memory.masked(exclude_kinds) if exclude_kinds else owner.memory
            views.append((shard, owner, memory))
        index = CompositeIndex([(shard, _OwnerView(owner, memory))
                                for shard, owner, memory in views], pair_snapshot)

        query_cues = keys(query)
        fanout = {cue: len(index.episode_ids_for_cue(cue)) for cue in query_cues}
        selected = tuple(cue for cue in query_cues if fanout[cue])
        total = max(1, index.episode_count)
        informative = tuple(cue for cue in selected if fanout[cue] * 2 <= total)
        propositions = set()
        for cue in informative:
            for identifier in index.episode_ids_for_cue(cue):
                _, owner = index.owner_of(identifier)
                proposition = owner.memory.proposition_of(identifier)
                if proposition is not None:
                    propositions.add(proposition)
        cues = (*selected, *('proposition:' + p for p in sorted(propositions)))
        signal0 = detect_deja_vu(index, query=query, current_cues=cues)
        signal = DejaVuSignal(snapshot_id=pair_snapshot, query=signal0.query,
                             current_cues=signal0.current_cues,
                             matched_cues=signal0.matched_cues,
                             recognition_strength=signal0.recognition_strength,
                             candidate_count=signal0.candidate_count)
        recalled = recall_memory(index, signal)
        # Re-evidence always compares explicit propositions carried by the
        # activated records, including tiny stores where every lexical cue is a
        # high-fanout function word and therefore opens no additional closure.
        activated_propositions = set(propositions)
        for candidate in recalled.candidates:
            _, owner = index.owner_of(candidate.episode_id)
            proposition = owner.memory.proposition_of(candidate.episode_id)
            if proposition is not None:
                activated_propositions.add(proposition)

        idf = {cue: math.log(1.0 + (total - fanout[cue] + 0.5) / (fanout[cue] + 0.5))
               for cue in informative}
        cue_total = sum(memory.cue_total for _, _, memory in views)
        average = cue_total / total if total else 1.0
        query_tokens = [token.casefold() for token in re.findall(r'\w+', query)]
        local_roots = {shard: owner.recall(query, None, exclude_kinds=exclude_kinds,
                                            region_scope=region_scope)
                       for shard, owner, _ in views}

        def words(matched):
            longest = sorted((cue for cue in matched if cue in idf), key=len, reverse=True)
            kept = []
            for cue in longest:
                if not any(cue in prior for prior in kept):
                    kept.append(cue)
            return kept

        def order(candidate):
            shard, owner = index.owner_of(candidate.episode_id)
            memory = owner.memory
            row = memory._store['row_of'][candidate.episode_id]
            length = len(memory._store['cues'][row])
            norm = 2.2 / (1.0 + 1.2 * (1.0 - 0.3 + 0.3 * length / max(1.0, average)))
            gate = 1.0 + PROMOTION_GATE * (owner.graph.strength(candidate.episode_id) >= vrs_refine.PROMOTION)
            path = (local_roots[shard]['region_navigation']['paths'].get(candidate.episode_id) or {}).get('path')
            gate *= UNBRIDGED_FACTOR if path == 'unbridged' else 1.0
            matched_words = words(candidate.matched_cues)
            asks, description = memory.asks_of(candidate.episode_id)
            rare = [word.casefold() for word in matched_words
                    if fanout.get(word, 0) <= ASK_GATE_RARE_SHARE * total
                    and any(token == word.casefold() or token.startswith(word.casefold())
                            for token in query_tokens)]
            ask_hits = sum(1 for word in rare if asks and word in asks)
            desc_hits = sum(1 for word in rare if description and word in description)
            gate *= (1.0 + ASK_GATE * min(ASK_GATE_MAX_HITS, ask_hits))
            gate *= (1.0 + DESCRIPTION_GATE * min(ASK_GATE_MAX_HITS, desc_hits))
            return (-sum(idf[cue] for cue in matched_words) * norm * gate,
                    -candidate.cue_overlap, candidate.episode_id)

        recalled = RecallResult(query, tuple(sorted(recalled.candidates, key=order)), pair_snapshot,
                                source_dependencies=recalled.source_dependencies)
        replayed = replay_memory(index, recalled)

        opponents = {}
        for proposition in activated_propositions:
            active = []
            for _, owner, _ in views:
                for identifier in owner.memory.propositions.get(proposition, ()):
                    if identifier not in owner.memory.superseded:
                        active.append(owner.memory.episode_light(identifier))
            if {episode.steps[0].observation.get('evidence_polarity') for episode in active} == {'support', 'refute'}:
                opponents[proposition] = tuple(sorted(episode.episode_id for episode in active))

        graph_snapshot = digest(('sharded-vrs-v1', [(shard, owner.graph.snapshot_id)
                                                    for shard, owner, _ in views]))

        def judge(episode):
            shard, owner = index.owner_of(episode.episode_id)
            proposition = episode.steps[0].observation.get('proposition_id')
            if proposition in opponents and episode.episode_id not in owner.memory.superseded:
                return CurrentEvidenceVerdict(episode.episode_id, proposition, 'conflict',
                    'Opposing recorded claims for the same explicit proposition across VRS shards; neither is certified true.',
                    ('memory-snapshot:' + pair_snapshot, *episode.source_addresses), opponents[proposition])
            return current_experience_verdict(episode, memory_snapshot_id=pair_snapshot,
                vrs_snapshot_id=graph_snapshot, current_strength=owner.graph.strength(episode.episode_id),
                proposition=proposition or 'experience:' + episode.episode_id)

        re_evidenced = re_evidence_memory(replayed, judge=judge)
        receipt = MemoryActivationReceipt('rozephine-memory-activation-v1', pair_snapshot,
            signal, recalled, replayed, re_evidenced)
        identifiers = [candidate.episode_id for candidate in recalled.candidates]
        paths = {}
        for identifier in identifiers:
            shard, _ = index.owner_of(identifier)
            path = dict(local_roots[shard]['region_navigation']['paths'].get(identifier)
                        or dict(region=None, path='pending'))
            path['shard'] = shard
            paths[identifier] = path
        cross_portals = _cross_shard_portals(index, identifiers, pair_snapshot)
        active = {(shard, int(region)) for shard, root in local_roots.items()
                  for region in root['region_navigation'].get('active_regions', ())}
        for identifier, path in paths.items():
            if path.get('path') in ('local', 'member'):
                continue
            endpoint = (path['shard'], path.get('region'))
            if endpoint[1] is None:
                continue
            for portal in cross_portals:
                regions = [tuple(region) for region in portal['regions']]
                if endpoint not in regions:
                    continue
                other = regions[1] if regions[0] == endpoint else regions[0]
                if other in active:
                    path.update(path='cross_shard_portal', portal=portal['regions'],
                                via=portal['keys'][0])
                    break
        return dict(record_count=index.episode_count, receipt={'activation': receipt},
            memory_selection=dict(candidate_counts=fanout, selected_cues=cues,
                rejected_cues=tuple(cue for cue in query_cues if cue not in selected),
                function_word_cues=tuple(cue for cue in selected if cue not in informative),
                selection_method='global_cues_and_proposition_closure_across_complete_vrs_shards',
                excluded_kinds=list(exclude_kinds or ()),
                closure_rule='global propositions of records matched by an informative cue',
                candidate_order='global_bm25_x_per_shard_vrs_promotion_x_asks_then_cue_overlap',
                semantic_acceptance_claimed=False),
            vrs_selection=dict(selection_method='complete_per_shard_regions_and_shared_experience_portals',
                numerical_version=vrs_refine.VERSION, logical_implication_claimed=False,
                grants_authority=False, shard_versions={shard:
                    (owner.graph.stable.version_id if owner.graph.stable is not None else None)
                    for shard, owner, _ in views}),
            current_strengths={identifier: index.owner_of(identifier)[1].graph.strength(identifier)
                               for identifier in identifiers},
            current_promotions={identifier: index.owner_of(identifier)[1].graph.strength(identifier) >= 1.0
                                for identifier in identifiers},
            current_propositions={identifier: index.owner_of(identifier)[1].memory.proposition_of(identifier)
                                  for identifier in identifiers},
            superseded_by={identifier: index.owner_of(identifier)[1].memory.superseded.get(identifier)
                           for identifier in identifiers},
            region_navigation=dict(paths=paths, shard_roots={shard: root['region_navigation']
                                                             for shard, root in local_roots.items()},
                                   cross_shard_portals=cross_portals,
                                   membership_is_truth=False),
            pair_snapshot_id=pair_snapshot, authority=MappingProxyType({
                key: False for key in ('world', 'action', 'persistent_write', 'model_update', 'distribution', 'p3')}))


class _OwnerView:
    """One pinned memory view paired with its unchanged complete VRS graph."""

    def __init__(self, owner, memory):
        self.memory, self.graph, self.pair = memory, owner.graph, owner.pair


def _cross_shard_portals(index, identifiers, pair_snapshot):
    """Experience-derived connectors between local shard regions.

    The same membership rule used inside a shard is extended over its original
    record-to-cue strength mass.  When at least ``SHARED_FLOOR`` of one original
    experience's mass lands on a cue region in another shard, that original
    experience is the replayable key.  No cue-only or synthetic portal is made.
    """
    portals = {}
    for identifier in identifiers:
        source_shard, source = index.owner_of(identifier)
        graph, memory = source.graph, source.memory
        if graph.stable is None:
            continue
        source_region = graph.region_of(identifier)
        if source_region is None or source_region < 0:
            continue
        row = memory._store['row_of'][identifier]
        cue_ids = graph_cue_ids(memory, row)
        center = graph.nodes.episode_node.get(identifier)
        if center is None:
            continue
        lo, hi = int(graph.flat.out_ptr[center]), int(graph.flat.out_ptr[center + 1])
        edge_strength = {int(graph.flat.dst[edge]): float(graph.flat.strength[edge])
                         for edge in range(lo, hi)}
        weighted_cues = []
        for cue_id in cue_ids:
            node = graph.nodes.cue(cue_id)
            strength = edge_strength.get(node)
            if node >= 0 and strength is not None:
                weighted_cues.append((memory._store['vocab'].string_of(cue_id), strength))
        total = sum(strength for _, strength in weighted_cues)
        if total <= 0:
            continue
        own = max((weight for region, weight in graph.memberships_of(identifier)
                   if region == source_region), default=1.0)
        episode = memory.episode_light(identifier)
        for target_shard, target in index.owners:
            if target_shard == source_shard or target.graph.stable is None:
                continue
            labels = target.graph.labels()
            mass = {}
            vocab = target.memory._store['vocab']
            for cue, strength in weighted_cues:
                cue_id = vocab.id_of(cue)
                node = target.graph.nodes.cue(cue_id) if cue_id is not None else -1
                if node >= 0 and node < len(labels) and labels[node] >= 0:
                    region = int(labels[node])
                    mass[region] = mass.get(region, 0.0) + strength
            for target_region, value in mass.items():
                weight = value / total
                if weight < vrs_refine.SHARED_FLOOR:
                    continue
                endpoints = sorted(((source_shard, int(source_region)),
                                    (target_shard, int(target_region))))
                pair = tuple(endpoints)
                portal = portals.setdefault(pair, dict(regions=[list(endpoints[0]), list(endpoints[1])],
                    keys=[], shared=0, version=pair_snapshot,
                    rule='original_experience_strength_mass_membership'))
                portal['keys'].append(dict(episode_id=identifier, revision=episode.revision,
                    outcome=episode.steps[0].outcome, weights=[round(float(own), 6), round(float(weight), 6)],
                    strength=round(float(graph.strength(identifier)), 6), source_shard=source_shard))
    result = []
    for pair in sorted(portals):
        portal = portals[pair]
        unique = {key['episode_id']: key for key in portal['keys']}
        portal['keys'] = sorted(unique.values(), key=lambda key:
            (-min(key['weights']) * key['strength'], key['episode_id']))[:vrs_refine.PORTAL_KEYS]
        portal['shared'] = len(unique)
        result.append(portal)
    return result
