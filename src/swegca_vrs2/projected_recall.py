"""Disk-native natural Recall over exact capsules and current VRS projections.

This is a read projection of the complete VRS main.  It does not replace the
graph, regions, memberships, portals or consolidation.  Those remain owned by
each complete shard generation; this module joins their immutable projections
to the original Replay capsules so a cold read never deserializes a checkpoint.
"""
from __future__ import annotations

import math
import re
from types import MappingProxyType

from . import vrs_refine
from .engine.mosaic_memory_activation import (
    CurrentEvidenceVerdict, DejaVuSignal, MemoryActivationReceipt,
    RecallCandidate, RecallResult, ReplayedEpisode, ReplayResult,
    current_experience_verdict, re_evidence_memory,
)
from .store import (
    ASK_GATE, ASK_GATE_MAX_HITS, ASK_GATE_RARE_SHARE, DESCRIPTION_GATE,
    PROMOTION_GATE, UNBRIDGED_FACTOR, digest, keys,
    KEYED_PARTNERS_IN_SCOPE, PORTAL_SCORE_FLOOR, REGION_ACTIVATION,
    REGION_SCOPE_AUTO_CANDIDATES, REGION_SCOPE_FLOOR,
    REGION_SCOPE_INFORMATIVE_ONLY, REGION_SCOPE_MASS_SHARE,
    REGION_SCOPE_MIN_HITS, REGION_SCOPE_TOP, REGION_SCOPE_TOP_ROWS,
)


AUTHORITY = MappingProxyType({key: False for key in
    ('world', 'action', 'persistent_write', 'model_update', 'distribution', 'p3')})


class ProjectedRecall:
    def __init__(self, resident, pair_snapshot, query, exclude_kinds=(), region_scope='all'):
        self.resident = resident
        self.pair_snapshot = pair_snapshot
        self.query = query
        self.exclude = frozenset(exclude_kinds or ())
        self.region_scope = region_scope
        self.exact = {}
        self.current = {}
        self.postings = {}

    def _check_memory(self):
        self.resident.admit_read_resources()

    def _exact(self, identifier):
        if identifier not in self.exact:
            if len(self.exact) % 256 == 0:
                self._check_memory()
            exact = self.resident.exact_replay(identifier)
            if exact is None:
                raise ValueError('cue_directory_exact_replay_missing:' + identifier)
            self.exact[identifier] = exact
        return self.exact[identifier]

    @staticmethod
    def _kind(exact):
        return str(exact.get('kind') or '')

    def matches(self, cue):
        cached = self.postings.get(cue)
        if cached is not None:
            return cached
        rows = []
        for shard, identifier in self.resident.cue_shards.matches_for(cue):
            exact = self._exact(identifier)
            if exact['shard'] != shard:
                raise ValueError('cue_directory_shard_mismatch:' + identifier)
            if self._kind(exact) not in self.exclude:
                rows.append(identifier)
        cached = tuple(dict.fromkeys(rows))
        self.postings[cue] = cached
        return cached

    def _current(self, identifier):
        if identifier not in self.current:
            exact = self._exact(identifier)
            # Natural ranking and local navigation need current VRS facts, but
            # per-cue strengths are used only to build cross-shard portals.
            # Keep that larger expansion for the actual cross-shard case.
            current = self.resident.current_vrs(exact, include_cue_strengths=False)
            if current is None:
                raise ValueError('read_projection_not_ready:' + exact['shard'])
            if current['pair_snapshot_id'] != self.resident.pair_ids.get(exact['shard']):
                raise ValueError('read_projection_pair_mismatch')
            self.current[identifier] = current
        return self.current[identifier]

    @staticmethod
    def _proposition(exact):
        return exact['proposition']

    def _proposition_ids(self, proposition):
        result = []
        for shard, identifier in self.resident.exact.proposition_experiences(proposition):
            exact = self._exact(identifier)
            if exact['shard'] != shard:
                raise ValueError('experience_proposition_route_shard_mismatch')
            if self._kind(exact) not in self.exclude:
                result.append(identifier)
        return tuple(dict.fromkeys(result))

    @staticmethod
    def _portal_map(current):
        return {tuple(int(value) for value in row['pair']): row
                for row in current.get('portals', ())}

    def _navigation(self, candidates, selected, scope):
        shards = sorted({self._exact(row.episode_id)['shard'] for row in candidates})
        roots, paths = {}, {}
        for shard in shards:
            active = {region for cue in selected
                      for region in (self.resident.region_for_cue(shard, cue),)
                      if region is not None}
            sample = next((self._current(row.episode_id) for row in candidates
                           if self._exact(row.episode_id)['shard'] == shard), None)
            portal_rows = tuple(sample.get('portals', ())) if sample is not None else ()
            shard_scope = (scope.get('by_shard') or {}).get(shard, scope)
            roots[shard] = dict(active_regions=sorted(active), portals=portal_rows,
                                membership_is_truth=False,
                                scope={key: value for key, value in shard_scope.items()
                                       if key != 'by_shard'})
            portal_map = self._portal_map(sample or {})
            for candidate in candidates:
                identifier = candidate.episode_id
                if self._exact(identifier)['shard'] != shard:
                    continue
                current = self._current(identifier)
                region = current['region']
                if region is None or region < 0:
                    paths[identifier] = dict(region=None, path='pending', shard=shard)
                    continue
                if region in active:
                    paths[identifier] = dict(region=region, path='local', shard=shard)
                    continue
                members = [(member, weight) for member, weight in current['memberships']
                           if member in active and weight >= vrs_refine.SHARED_FLOOR]
                if members:
                    member, weight = max(members, key=lambda item: item[1])
                    paths[identifier] = dict(region=region, path='member', in_region=member,
                                             weight=round(weight, 4), shard=shard)
                    continue
                best = None
                for cue in candidate.matched_cues:
                    cue_region = self.resident.region_for_cue(shard, cue)
                    if cue_region is None or cue_region == region:
                        continue
                    pair = (min(cue_region, region), max(cue_region, region))
                    portal = portal_map.get(pair)
                    if portal is None:
                        continue
                    value = portal.get('value') or {}
                    experience_keys = portal.get('experience_keys') or ()
                    raw_keys = value.get('keys') or ()
                    if not (value.get('status') == 'candidate' or raw_keys):
                        continue
                    if raw_keys and not experience_keys:
                        raise ValueError('read_projection_portal_experience_key_missing')
                    rank = (bool(experience_keys), float(value.get('score', 0.0)))
                    if best is None or rank > best[2]:
                        best = (pair, portal, rank)
                if best is None:
                    paths[identifier] = dict(region=region, path='unbridged', shard=shard,
                        reason='no shared experience and no promoted bridge from an active region')
                else:
                    pair, portal, _ = best
                    value = portal.get('value') or {}
                    experience_keys = portal.get('experience_keys') or ()
                    entry = dict(region=region, path='portal', portal=pair,
                                 score=float(value.get('score', 0.0)), shard=shard)
                    if experience_keys:
                        entry['via'] = dict(experience_keys[0])
                    else:
                        entry.update(via=None,
                            bridge='edges only (no shared experience at the floor)')
                    paths[identifier] = entry
        return roots, paths

    def _scope_candidates(self, candidates, selected, informative, idf, lexical_count):
        requested = self.region_scope
        scope = dict(requested=requested, applied='all', allowed_regions=[],
                     excluded_rows=0, fallback=None)
        if requested == 'auto':
            scope.update(auto_candidates=lexical_count,
                         auto_threshold=REGION_SCOPE_AUTO_CANDIDATES)
            if lexical_count < REGION_SCOPE_AUTO_CANDIDATES:
                return candidates, scope
        elif requested != 'regions':
            return candidates, scope

        by_shard, allowed_all, kept = {}, [], []
        for shard in sorted({self._exact(row.episode_id)['shard'] for row in candidates}):
            rows = [row for row in candidates if self._exact(row.episode_id)['shard'] == shard]
            active = set()
            detail = dict(requested=requested, applied='all', allowed_regions=[],
                          excluded_rows=0, fallback=None)
            if REGION_ACTIVATION == 'rows':
                scored = []
                for row in rows:
                    value = sum(idf.get(cue, 0.0) for cue in row.matched_cues)
                    if value:
                        scored.append((value, row.episode_id))
                scored.sort(key=lambda item: (-item[0], item[1]))
                strongest = scored[:REGION_SCOPE_TOP_ROWS]
                active = {self._current(identifier)['region'] for _, identifier in strongest}
                active.discard(None); active.discard(-1)
                detail['top_rows'] = len(strongest)
            elif REGION_ACTIVATION == 'mass':
                mass = {}
                for cue in informative:
                    for identifier in self.matches(cue):
                        if self._exact(identifier)['shard'] != shard:
                            continue
                        region = self._current(identifier)['region']
                        if region is not None and region >= 0:
                            mass[region] = mass.get(region, 0.0) + idf[cue]
                ranked = sorted(mass.items(), key=lambda item: (-item[1], item[0]))
                floor = ranked[0][1] * REGION_SCOPE_MASS_SHARE if ranked else 0.0
                active = {region for region, _ in ranked[:REGION_SCOPE_TOP]}
                active.update(region for region, value in ranked if value >= floor)
                detail['region_mass'] = [(region, round(value, 2))
                                         for region, value in ranked[:12]]
            else:
                hits = {}
                source = informative if REGION_SCOPE_INFORMATIVE_ONLY else selected
                for cue in source:
                    region = self.resident.region_for_cue(shard, cue)
                    if region is not None:
                        hits[region] = hits.get(region, 0) + 1
                active = {region for region, count in hits.items()
                          if count >= REGION_SCOPE_MIN_HITS} or set(hits)

            allowed = set(active)
            sample = next((self._current(row.episode_id) for row in rows), None)
            keyed_partners = 0
            for portal in (() if sample is None else sample.get('portals', ())):
                pair = tuple(int(value) for value in portal['pair'])
                if not active.intersection(pair):
                    continue
                value = portal.get('value') or {}
                keyed = bool(value.get('keys')) and KEYED_PARTNERS_IN_SCOPE
                if ((value.get('status') == 'candidate'
                     and float(value.get('score', 0.0)) >= PORTAL_SCORE_FLOOR) or keyed):
                    allowed.update(pair)
                    keyed_partners += int(keyed)
            detail['keyed_partners'] = keyed_partners
            shard_kept = []
            if allowed:
                for row in rows:
                    current = self._current(row.episode_id)
                    region = current['region']
                    member = any(member_region in active and weight >= vrs_refine.SHARED_FLOOR
                                 for member_region, weight in current['memberships'])
                    if region is None or region < 0 or region in allowed or member:
                        shard_kept.append(row)
            else:
                shard_kept = list(rows)
                detail['fallback'] = 'no_active_regions'
            kept.extend(shard_kept)
            detail['allowed_regions'] = sorted(allowed)
            detail['excluded_rows'] = len(rows) - len(shard_kept)
            detail['applied'] = 'regions' if allowed else 'all'
            by_shard[shard] = detail
            allowed_all.extend([shard, region] for region in sorted(allowed))

        if len(kept) < REGION_SCOPE_FLOOR:
            scope.update(fallback='fewer_than_floor',
                         would_exclude=len(candidates) - len(kept), by_shard=by_shard)
            return candidates, scope
        kept_ids = {row.episode_id for row in kept}
        ordered = [row for row in candidates if row.episode_id in kept_ids]
        scope.update(applied='regions', allowed_regions=allowed_all,
                     excluded_rows=len(candidates) - len(ordered), by_shard=by_shard)
        return ordered, scope

    @staticmethod
    def _words(matched, idf):
        longest = sorted((cue for cue in matched if cue in idf), key=len, reverse=True)
        kept = []
        for cue in longest:
            if not any(cue in prior for prior in kept):
                kept.append(cue)
        return kept

    def _cross_shard_portals(self, candidates, shards):
        if len(shards) < 2:
            return []
        portals = {}
        for candidate in candidates:
            identifier = candidate.episode_id
            exact, current = self._exact(identifier), self._current(identifier)
            source_shard, source_region = exact['shard'], current['region']
            if source_region is None or source_region < 0:
                continue
            full_current = self.resident.current_vrs(exact)
            if (full_current is None or full_current['pair_snapshot_id']
                    != current['pair_snapshot_id']):
                raise ValueError('read_projection_pair_mismatch')
            cue_strengths = full_current.get('cue_strengths') or ()
            if len(cue_strengths) != len(exact['cues']):
                raise ValueError('read_projection_cue_strength_alignment_changed')
            weighted = [(cue, strength) for cue, strength in zip(exact['cues'], cue_strengths)
                        if strength > 0]
            total = sum(strength for _, strength in weighted)
            if total <= 0:
                continue
            own = max((weight for region, weight in current['memberships']
                       if region == source_region), default=1.0)
            replay = exact['replay']
            for target_shard in shards:
                if target_shard == source_shard:
                    continue
                mass = {}
                for cue, strength in weighted:
                    target_region = self.resident.region_for_cue(target_shard, cue)
                    if target_region is not None:
                        mass[target_region] = mass.get(target_region, 0.0) + strength
                for target_region, value in mass.items():
                    weight = value / total
                    if weight < vrs_refine.SHARED_FLOOR:
                        continue
                    endpoints = sorted(((source_shard, int(source_region)),
                                        (target_shard, int(target_region))))
                    pair = tuple(endpoints)
                    portal = portals.setdefault(pair, dict(
                        regions=[list(endpoints[0]), list(endpoints[1])], keys=[], shared=0,
                        version=self.pair_snapshot,
                        rule='original_experience_strength_mass_membership'))
                    portal['keys'].append(dict(episode_id=identifier,
                        revision=exact['revision'], outcome=replay.steps[0].outcome,
                        weights=[round(float(own), 6), round(float(weight), 6)],
                        strength=round(float(current['strength']), 6), source_shard=source_shard))
        result = []
        for pair in sorted(portals):
            portal = portals[pair]
            unique = {key['episode_id']: key for key in portal['keys']}
            portal['keys'] = sorted(unique.values(), key=lambda key:
                (-min(key['weights']) * key['strength'], key['episode_id']))[:vrs_refine.PORTAL_KEYS]
            portal['shared'] = len(unique)
            result.append(portal)
        return result

    def run(self):
        self._check_memory()
        query_cues = keys(self.query)
        fanout = {cue: len(self.matches(cue)) for cue in query_cues}
        selected = tuple(cue for cue in query_cues if fanout[cue])
        total = max(1, self.resident.logical_record_count())
        informative = tuple(cue for cue in selected if fanout[cue] * 2 <= total)
        propositions = {self._proposition(self._exact(identifier))
                        for cue in informative for identifier in self.matches(cue)} - {None}
        activation_cues = (*selected, *('proposition:' + value for value in sorted(propositions)))
        matched_all = tuple(cue for cue in activation_cues if self.matches(cue))
        identifiers = {identifier for cue in matched_all for identifier in self.matches(cue)}
        lexical_ids = {identifier for cue in selected for identifier in self.matches(cue)}

        candidates = []
        current_cues = set(activation_cues)
        for identifier in identifiers:
            exact = self._exact(identifier)
            episode_cues = set(exact['cues'])
            matched_cues = tuple(cue for cue in exact['cues'] if cue in current_cues)
            candidates.append(RecallCandidate(identifier, matched_cues,
                len(matched_cues) / len(episode_cues.union(current_cues)), exact['revision'],
                exact['replay'].verification_state,
                tuple(step.outcome for step in exact['replay'].steps)))
        candidates.sort(key=lambda row: (-row.cue_overlap, row.episode_id))

        idf = {cue: math.log(1.0 + (total - fanout[cue] + 0.5) / (fanout[cue] + 0.5))
               for cue in informative}
        candidates, scope = self._scope_candidates(
            candidates, selected, informative, idf, len(lexical_ids))
        scoped_ids = {row.episode_id for row in candidates}
        matched = tuple(cue for cue in activation_cues
                        if any(identifier in scoped_ids for identifier in self.matches(cue)))
        signal = DejaVuSignal(self.pair_snapshot, self.query, tuple(activation_cues), matched,
            len(matched) / max(1, len(activation_cues)), len(scoped_ids))
        roots, paths = self._navigation(candidates, selected, scope)
        average = self.resident.logical_cue_total() / total if total else 1.0
        query_tokens = [token.casefold() for token in re.findall(r'\w+', self.query)]

        def order(candidate):
            exact, current = self._exact(candidate.episode_id), self._current(candidate.episode_id)
            length = exact['cue_count']
            norm = 2.2 / (1.0 + 1.2 * (1.0 - 0.3 + 0.3 * length / max(1.0, average)))
            gate = 1.0 + PROMOTION_GATE * (current['strength'] >= vrs_refine.PROMOTION)
            gate *= UNBRIDGED_FACTOR if paths[candidate.episode_id]['path'] == 'unbridged' else 1.0
            words = self._words(candidate.matched_cues, idf)
            rare = [word.casefold() for word in words
                    if fanout.get(word, 0) <= ASK_GATE_RARE_SHARE * total
                    and any(token == word.casefold() or token.startswith(word.casefold())
                            for token in query_tokens)]
            gate *= 1.0 + ASK_GATE * min(ASK_GATE_MAX_HITS,
                                        sum(1 for word in rare if exact['asks'] and word in exact['asks']))
            gate *= 1.0 + DESCRIPTION_GATE * min(ASK_GATE_MAX_HITS,
                sum(1 for word in rare if exact['description'] and word in exact['description']))
            return (-sum(idf[cue] for cue in words) * norm * gate,
                    -candidate.cue_overlap, candidate.episode_id)

        candidates = tuple(sorted(candidates, key=order))
        recalled = RecallResult(self.query, candidates, self.pair_snapshot)
        episodes = []
        for candidate in candidates:
            source = self._exact(candidate.episode_id)['replay']
            episodes.append(ReplayedEpisode(candidate.episode_id, candidate.matched_cues,
                source.steps, source.source_addresses, source.verification_state))
        replayed = ReplayResult(self.query, tuple(episodes))

        active_propositions = set(propositions)
        active_propositions.update(self._proposition(self._exact(row.episode_id))
                                   for row in candidates)
        active_propositions.discard(None)
        opponents = {}
        for proposition in active_propositions:
            active = [identifier for identifier in self._proposition_ids(proposition)
                      if self._current(identifier)['superseded_by'] is None]
            polarities = {self._exact(identifier)['polarity']
                          for identifier in active}
            if polarities == {'support', 'refute'}:
                opponents[proposition] = tuple(sorted(active))
        graph_snapshot = digest(('sharded-vrs-projection-v1', sorted(
            {(self._exact(identifier)['shard'], self._current(identifier)['graph_snapshot_id'])
             for identifier in scoped_ids})))

        def judge(episode):
            proposition = episode.steps[0].observation.get('proposition_id')
            current = self._current(episode.episode_id)
            if proposition in opponents and current['superseded_by'] is None:
                return CurrentEvidenceVerdict(episode.episode_id, proposition, 'conflict',
                    'Opposing recorded claims for the same explicit proposition across VRS shards; neither is certified true.',
                    ('memory-snapshot:' + self.pair_snapshot, *episode.source_addresses),
                    opponents[proposition])
            return current_experience_verdict(episode, memory_snapshot_id=self.pair_snapshot,
                vrs_snapshot_id=graph_snapshot, current_strength=current['strength'],
                proposition=proposition or 'experience:' + episode.episode_id)

        re_evidenced = re_evidence_memory(replayed, judge=judge)
        receipt = MemoryActivationReceipt('rozephine-memory-activation-v1', self.pair_snapshot,
                                           signal, recalled, replayed, re_evidenced)
        shards = sorted(roots)
        cross_portals = self._cross_shard_portals(candidates, shards)
        active_regions = {(shard, region) for shard, root in roots.items()
                          for region in root['active_regions']}
        for identifier, path in paths.items():
            if path['path'] in ('local', 'member') or path.get('region') is None:
                continue
            endpoint = (path['shard'], path['region'])
            for portal in cross_portals:
                regions = [tuple(region) for region in portal['regions']]
                if endpoint not in regions:
                    continue
                other = regions[1] if regions[0] == endpoint else regions[0]
                if other in active_regions:
                    path.update(path='cross_shard_portal', portal=portal['regions'],
                                via=portal['keys'][0])
                    break
        versions = {shard: next((self._current(row.episode_id)['stable_version_id']
                                 for row in candidates
                                 if self._exact(row.episode_id)['shard'] == shard), None)
                    for shard in shards}
        ids = [row.episode_id for row in candidates]
        return dict(record_count=self.resident.logical_record_count(), receipt={'activation': receipt},
            memory_selection=dict(candidate_counts=fanout, selected_cues=activation_cues,
                rejected_cues=tuple(cue for cue in query_cues if cue not in selected),
                function_word_cues=tuple(cue for cue in selected if cue not in informative),
                selection_method='disk_global_cues_and_proposition_closure_across_complete_vrs_shards',
                excluded_kinds=list(self.exclude),
                closure_rule='global propositions of records matched by an informative cue',
                candidate_order='global_bm25_x_per_shard_vrs_promotion_x_asks_then_cue_overlap',
                semantic_acceptance_claimed=False),
            vrs_selection=dict(selection_method='projected_complete_regions_and_shared_experience_portals',
                numerical_version=vrs_refine.VERSION, logical_implication_claimed=False,
                grants_authority=False, shard_versions=versions),
            current_strengths={identifier: self._current(identifier)['strength'] for identifier in ids},
            current_promotions={identifier: self._current(identifier)['strength'] >= 1.0 for identifier in ids},
            current_propositions={identifier: self._proposition(self._exact(identifier)) for identifier in ids},
            superseded_by={identifier: self._current(identifier)['superseded_by'] for identifier in ids},
            region_navigation=dict(paths=paths, shard_roots=roots,
                cross_shard_portals=cross_portals, membership_is_truth=False),
            pair_snapshot_id=self.pair_snapshot, authority=AUTHORITY)


def projected_recall(resident, pair_snapshot, query, exclude_kinds=(), region_scope='all'):
    return ProjectedRecall(resident, pair_snapshot, query, exclude_kinds, region_scope).run()
