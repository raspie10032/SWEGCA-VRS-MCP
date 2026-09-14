"""One durable main owner with immutable hot generations and observation ingress.

SQLite is an ingress/restart boundary only. Recall reads immutable resident maps.
No model, HTTP client, subprocess, Hermes store or action executor is used here.
"""
from __future__ import annotations

from collections import deque
from dataclasses import dataclass, fields, is_dataclass, replace
import hashlib
import json
import re
import sqlite3
from pathlib import Path
from time import perf_counter_ns
from types import MappingProxyType, SimpleNamespace
import uuid

from filelock import FileLock, Timeout
from immutables import Map
import numpy as np

from .engine.mosaic_memory_activation import (
    OUTCOMES, MemoryEpisode, MemoryStep, FullCurrentMemoryVrsSnapshot,
    AtomicFullCurrentMemoryVrsOwner, activate_memory, current_experience_verdict,
    CurrentEvidenceVerdict,
)
from .engine.mosaic_immutable_numeric import immutable_numeric_array as frozen
from .engine.mosaic_vrs_event_kernel import EventVrsInputs
from .engine.mosaic_vrs_event_signal import settle_event_signal, VERSION
from .engine.mosaic_vrs_event_delta import prepare_event_delta
from .engine.mosaic_vrs_dependency_index import EndpointDependencyIndex
from .engine.mosaic_vrs_connectivity_regions import ConnectivityRegions
from .engine.mosaic_vrs_state_update import VRSConnectionStateUpdate, VRSStateUpdateReceipt
from .engine.mosaic_memory_promotion import assess_vrs_experience_promotion

AUTHORITY = MappingProxyType({k: False for k in ('world', 'action', 'persistent_write', 'model_update', 'distribution', 'p3')})
EDGE = np.dtype([('source', '<u4'), ('target', '<u4'), ('sign', 'i1'), ('vrs_strength', '<f4')])


def plain(value):
    if is_dataclass(value):
        return {f.name: plain(getattr(value, f.name)) for f in fields(value)}
    if isinstance(value, (dict, Map, MappingProxyType)):
        return {k: plain(v) for k, v in value.items()}
    if isinstance(value, (list, tuple, set, frozenset)):
        return [plain(v) for v in value]
    if isinstance(value, np.generic):
        return value.item()
    return value


def canonical(value):
    return json.dumps(plain(value), ensure_ascii=False, sort_keys=True, separators=(',', ':'), allow_nan=False)


def digest(value):
    return hashlib.sha256(canonical(value).encode('utf-8')).hexdigest()


def freeze_view(value):
    """Share already frozen main data instead of recopying it for each view."""
    if isinstance(value, MappingProxyType):
        return value
    if type(value) is dict:
        return MappingProxyType({k: freeze_view(v) for k,v in value.items()})
    if type(value) in (list,tuple):
        return tuple(freeze_view(v) for v in value)
    return value


def keys(text):
    """Lexical address keys; Hangul substrings are retrieval cues, never claims."""
    result = dict.fromkeys(re.findall(r'\w+', text.casefold()))
    result.update(dict.fromkeys(re.findall(r'memory:[0-9a-f]{64}', text.casefold())))
    for word in tuple(result):
        if re.fullmatch('[가-힣]+', word):
            for size in range(2, min(4, len(word)) + 1):
                result.update(dict.fromkeys(word[i:i+size] for i in range(len(word)-size+1)))
    return tuple(result)


def text_field(value, name, maximum):
    if type(value) is not str or not value.strip() or len(value) > maximum:
        raise ValueError('invalid_' + name)
    # Reject invalid Unicode before touching storage.
    value.encode('utf-8')
    return value


def observation(arguments):
    required = {'request_id', 'text', 'source', 'revision'}
    optional = {'outcome', 'cues', 'proposition', 'polarity', 'supersedes', 'metadata'}
    if type(arguments) is not dict or not required <= arguments.keys() or arguments.keys()-required-optional:
        raise ValueError('invalid_observation_fields')
    result = {k: text_field(arguments[k], k, 65536 if k == 'text' else 1024 if k == 'source' else 128) for k in required}
    result['outcome'] = arguments.get('outcome', 'pending')
    if result['outcome'] not in OUTCOMES:
        raise ValueError('invalid_outcome')
    cues = arguments.get('cues', [])
    if type(cues) is not list or len(cues) > 128:
        raise ValueError('invalid_cues')
    result['cues'] = [text_field(c, 'cue', 128) for c in cues]
    result['proposition'] = arguments.get('proposition')
    result['polarity'] = arguments.get('polarity')
    if (result['proposition'] is None) != (result['polarity'] is None):
        raise ValueError('proposition_and_polarity_required_together')
    if result['proposition'] is not None:
        text_field(result['proposition'], 'proposition', 512)
        if result['polarity'] not in ('support', 'refute'):
            raise ValueError('invalid_polarity')
    result['supersedes'] = arguments.get('supersedes')
    if result['supersedes'] is not None:
        text_field(result['supersedes'], 'supersedes', 128)
    metadata = arguments.get('metadata', {})
    if type(metadata) is not dict or len(canonical(metadata).encode('utf-8')) > 16384:
        raise ValueError('invalid_metadata')
    result['metadata'] = json.loads(canonical(metadata))
    return result


@dataclass(frozen=True)
class HotIndex:
    snapshot_id: str
    records: Map
    postings: Map
    outcome_counts: Map
    propositions: Map
    superseded: Map
    semantic_families: tuple = ()
    lookup_requires_io: bool = False

    @property
    def episode_count(self):
        return len(self.records)

    def episode(self, identifier):
        return self.records[identifier]

    def episode_ids_for_cue(self, cue):
        return self.postings.get(cue, ())

    def iter_episode_ids(self):
        return iter(self.records)

    def append(self, row):
        identifier = 'memory:' + digest({k: v for k, v in row.items() if k != 'request_id'})
        if identifier in self.records:
            return self, identifier
        previous = row['supersedes']
        if previous is not None:
            old = self.records[previous]
            if old.source_addresses != (row['source'],) or old.revision == row['revision'] or previous in self.superseded:
                raise ValueError('invalid_source_revision_successor')
        cues = tuple(dict.fromkeys((*keys(row['text']), *(c.casefold() for c in row['cues']))))
        if row['proposition'] is not None:
            cues = (*cues, 'proposition:' + row['proposition'])
        if not cues:
            cues = ('source:' + row['source'],)
        # Every returned original address can be retrieved directly through the
        # same four-stage API, independently of lexical similarity.
        cues = (*cues, identifier)
        episode = MemoryEpisode(identifier, cues,
            (MemoryStep('external_observation', {'text': row['text'], 'metadata': row['metadata'],
                'proposition_id': row['proposition'], 'evidence_polarity': row['polarity'],
                'supersedes': previous, 'current_truth_claimed': False}, (),
                'Recorded external observation; truth and action authority not granted.',
                row['outcome'], (row['source'],)),), (row['source'],), row['revision'], 'unverified')
        postings = self.postings
        for cue in cues:
            postings = postings.set(cue, postings.get(cue, frozenset()) | {identifier})
        propositions = self.propositions
        if row['proposition']:
            p = row['proposition']
            propositions = propositions.set(p, propositions.get(p, frozenset()) | {identifier})
        return HotIndex(digest((self.snapshot_id, identifier)), self.records.set(identifier, episode),
            postings, self.outcome_counts.set(row['outcome'], self.outcome_counts[row['outcome']] + 1),
            propositions, self.superseded if previous is None else self.superseded.set(previous, identifier)), identifier


@dataclass(frozen=True)
class Graph:
    inputs: EventVrsInputs
    nodes: Map
    components: Map
    regions: Map
    last_receipt: object

    @classmethod
    def empty(cls, identity):
        edges = frozen(np.empty(0, dtype=EDGE))
        zeros = frozen(np.empty(0, dtype=np.float32))
        inputs = EventVrsInputs(digest(('vrs2', identity)), zeros, zeros, edges, zeros,
            frozen(np.empty(0, dtype=bool)), EndpointDependencyIndex.build(edges))
        return cls(inputs, Map(), Map(), Map(), MappingProxyType({}))

    def append(self, episode, snapshot, memory):
        # Full original episodes link to shared literal cues. These are numerical
        # dependencies/navigation associations, never syllogistic entailments.
        nodes = self.nodes
        new_names = [episode.episode_id] + [c for c in episode.cues if 'cue:' + c not in nodes]
        names = [episode.episode_id] + ['cue:' + c for c in new_names[1:]]
        for name in names:
            nodes = nodes.set(name, len(nodes))
        center = nodes[episode.episode_id]
        endpoints = [nodes['cue:' + c] for c in episode.cues]
        added = np.array([(a, b, 1, .5) for n in endpoints for a, b in ((center, n), (n, center))], dtype=EDGE)
        count = len(names)
        direct = np.zeros(count, dtype=np.float32)
        # Presence is a numerical observation signal, not historical truth.
        direct[0] = .1
        inputs = prepare_event_delta(self.inputs, snapshot_id=snapshot, appended_direct=direct,
            appended_score=np.zeros(count, dtype=np.float32), appended_unresolved=np.ones(count, dtype=bool),
            appended_edges=added, appended_strength=added['vrs_strength'].copy())
        # Re-evidence compares only explicit, source-bound propositions. Shared
        # keywords/outcome labels cannot reinforce a proposition. An actual
        # opposing claim keeps the conflict and preserves its numerical strength.
        obs = episode.steps[0].observation
        proposition = obs.get('proposition_id')
        prior = [] if proposition is None else [memory.episode(i) for i in memory.propositions.get(proposition, ())
            if i != episode.episode_id and i not in memory.superseded]
        replaced_id = obs.get('supersedes')
        if replaced_id is not None and proposition is not None:
            replaced = memory.episode(replaced_id)
            if replaced.steps[0].observation.get('proposition_id') == proposition:
                prior.append(replaced)
        updates = []
        for old in sorted(prior, key=lambda e:e.episode_id):
            opposing = old.steps[0].observation.get('evidence_polarity') != obs.get('evidence_polarity')
            retracted = opposing and old.episode_id == replaced_id
            conflict = opposing and not retracted
            # Another report from the same source/revision is retained, but is
            # not another independent strength update for this proposition.
            duplicate_source = old.source_addresses == episode.source_addresses and old.revision == episode.revision
            for edge in self.inputs.dependencies.edges_for(self.nodes[old.episode_id], direction='outgoing'):
                edge = int(edge)
                previous = float(inputs.strength[edge])
                value = previous if conflict or duplicate_source else previous * (.995 if retracted else 1.01)
                action = 'abstain_conflict' if conflict else 'preserve_unresolved' if duplicate_source else 'weaken' if retracted else 'reinforce'
                verdict = 'conflict' if conflict else 'available' if duplicate_source else 'refute' if retracted else 'support'
                connection = 'vrs-edge:' + str(edge)
                updates.append(VRSConnectionStateUpdate(old.episode_id,connection,proposition,verdict,
                    previous,value,action,assess_vrs_experience_promotion(snapshot_id=snapshot,
                        connection_id=connection,previous_strength=previous,current_strength=value),
                    source_judgments=((episode.episode_id,proposition,verdict),)))
        # If any record opposes this new claim, do not reinforce matching records
        # on the same unresolved proposition either (falsification first).
        if any(u.verdict == 'conflict' for u in updates):
            updates = [replace(u, verdict='conflict', current_strength=u.previous_strength,
                update_action='abstain_conflict', promotion=assess_vrs_experience_promotion(
                    snapshot_id=snapshot,connection_id=u.connection_id,
                    previous_strength=u.previous_strength,current_strength=u.previous_strength)) for u in updates]
        strength_receipt = VRSStateUpdateReceipt(snapshot, tuple(updates))
        proposal = settle_event_signal(inputs, changed_nodes=(center, *endpoints),
            strength_updates=strength_receipt, maximum_rounds=512)
        if proposal.pending_nodes:
            raise ValueError('vrs_signal_pending_no_publication')
        # Immutable numeric successor; no full parent vector materialization.
        score_ids = tuple(sorted(proposal.scores))
        strength_ids = tuple(sorted(proposal.strengths))
        settled = prepare_event_delta(inputs, snapshot_id=digest((snapshot, 'settled', [(i, float(proposal.scores[i])) for i in score_ids])),
            appended_direct=np.empty(0, dtype=np.float32), appended_score=np.empty(0, dtype=np.float32),
            appended_unresolved=np.empty(0, dtype=bool), appended_edges=np.empty(0, dtype=EDGE),
            appended_strength=np.empty(0, dtype=np.float32), score_indices=score_ids,
            score_values=np.array([proposal.scores[i] for i in score_ids], dtype=np.float32),
            strength_indices=strength_ids, strength_values=np.array([proposal.strengths[i] for i in strength_ids],dtype=np.float32))
        # Recompute only the changed connected component. Modularity within that
        # component is global; unchanged disconnected components stay shared.
        members, queue = set(), deque((center, *(self.nodes[e.episode_id] for e in prior)))
        edge_ids = set()
        while queue:
            n = queue.popleft()
            if n in members:
                continue
            members.add(n)
            for e in settled.dependencies.edges_for(n, direction='outgoing'):
                e = int(e); edge_ids.add(e)
                queue.append(int(settled.edges['target'][e]))
        ordered, edges = sorted(members), sorted(edge_ids)
        local = {n: i for i, n in enumerate(ordered)}
        source = SimpleNamespace(terms=tuple(ordered),
            edge_source=np.array([local[int(settled.edges['source'][e])] for e in edges], dtype=np.int64),
            edge_target=np.array([local[int(settled.edges['target'][e])] for e in edges], dtype=np.int64),
            edge_sign=np.array([int(settled.edges['sign'][e]) for e in edges], dtype=np.int8),
            vrs_strength=np.array([float(settled.strength[e]) for e in edges]))
        regions = ConnectivityRegions.build(source, vrs_snapshot_id=settled.snapshot_id)
        if not regions.converged:
            raise ValueError('region_topology_pending_no_publication')
        component_id = min(ordered)
        components, directory = self.components, self.regions
        for old in {components[n] for n in members if n in components}:
            directory = directory.delete(old)
        for n in ordered:
            components = components.set(n, component_id)
        directory = directory.set(component_id, (regions, Map(local)))
        receipt = proposal.receipt() | dict(changed_component_nodes=len(ordered), changed_component_edges=len(edges),
            global_recomputation_reason='Only affected connected component: modularity equivalence cannot be guaranteed by local moves alone.',
            source_episode_count_added=1, historical_outcome=episode.steps[0].outcome,
            re_evidence_updates=plain(strength_receipt),
            recorded_agreement_is_not_independent_factual_corroboration=True,
            logical_implication_claimed=False, grants_authority=False)
        return Graph(settled, nodes, components, directory, freeze_view(receipt))

    def memberships(self, identifier):
        node = self.nodes[identifier]
        region, positions = self.regions[self.components[node]]
        return tuple((region.topology_id, n, w) for n, w in region.memberships_for_term(positions[node]))

    def strength(self, identifier):
        edges = self.inputs.dependencies.edges_for(self.nodes[identifier], direction='outgoing')
        return max((float(self.inputs.strength[int(e)]) for e in edges), default=0.0)

    def summary(self):
        return {k: plain(v) for k,v in self.last_receipt.items() if k != 're_evidence_updates'}


class Main:
    """Sole owner for one state directory; transaction commit precedes publication."""
    def __init__(self, state_dir, *, allow_ingest=False):
        self.directory = Path(state_dir).expanduser().resolve()
        self.directory.mkdir(parents=True, exist_ok=True)
        self.lock = FileLock(self.directory / 'owner.lock')
        try:
            self.lock.acquire(timeout=0)
        except Timeout:
            raise ValueError('state_directory_already_owned') from None
        self.closed, self.allow_ingest = False, allow_ingest
        self.db = None
        try:
            self.db = sqlite3.connect(self.directory / 'memory.sqlite3', isolation_level=None)
            self.db.execute('PRAGMA journal_mode=WAL')
            self.db.execute('PRAGMA synchronous=FULL')
            self.db.execute('CREATE TABLE IF NOT EXISTS identity (id INTEGER PRIMARY KEY CHECK(id=1), value TEXT NOT NULL)')
            self.db.execute('CREATE TABLE IF NOT EXISTS observations (seq INTEGER PRIMARY KEY, request_id TEXT UNIQUE NOT NULL, body TEXT NOT NULL, fingerprint TEXT NOT NULL, pair TEXT NOT NULL)')
            identity = self.db.execute('SELECT value FROM identity WHERE id=1').fetchone()
            if identity is None:
                identity = (str(uuid.uuid4()),)
                self.db.execute('INSERT INTO identity VALUES (1,?)', identity)
            self.identity = identity[0]
            self.memory = HotIndex(digest(('memory', self.identity)), Map(), Map(), Map({o: 0 for o in OUTCOMES}), Map(), Map())
            self.graph = Graph.empty(self.identity)
            self.operations = Map()
            self.pair = FullCurrentMemoryVrsSnapshot(self.memory, self.graph.inputs.snapshot_id)
            for req, body, fingerprint, expected in self.db.execute('SELECT request_id,body,fingerprint,pair FROM observations ORDER BY seq'):
                row = observation(json.loads(body))
                if digest(row) != fingerprint or row['request_id'] != req:
                    raise ValueError('stored_observation_integrity_failed')
                memory, identifier = self.memory.append(row)
                graph = self.graph if memory is self.memory else self.graph.append(memory.episode(identifier), digest((self.graph.inputs.snapshot_id, fingerprint)), memory)
                pair = FullCurrentMemoryVrsSnapshot(memory, graph.inputs.snapshot_id)
                if pair.snapshot_id != expected:
                    raise ValueError('stored_generation_integrity_failed')
                self.memory, self.graph, self.pair = memory, graph, pair
                self.operations = self.operations.set(req, (fingerprint, identifier, pair.snapshot_id))
            self.owner = AtomicFullCurrentMemoryVrsOwner(self.pair)
        except BaseException:
            self.close()
            raise

    def _check(self):
        if self.closed:
            raise ValueError('main_closed')

    def status(self):
        self._check()
        return dict(status='ready', identity=self.identity, pair_snapshot_id=self.pair.snapshot_id,
            memory_snapshot_id=self.memory.snapshot_id, vrs_snapshot_id=self.graph.inputs.snapshot_id,
            hot_episode_count=len(self.memory.records), outcome_counts=dict(self.memory.outcome_counts),
            lookup_requires_io=False, internal_llm_calls=0, final_utterance_calls=0,
            backend='standalone_native_vrs2', native_engine_bundled=True, scheduled_dialogue_available=True,
            writes_enabled=self.allow_ingest, authority=dict(AUTHORITY), numerical_version=VERSION,
            vrs_node_count=len(self.graph.nodes), vrs_edge_count=len(self.graph.inputs.edges),
            last_vrs_event=self.graph.summary())

    def ingest(self, arguments):
        self._check()
        if not self.allow_ingest:
            raise ValueError('observation_ingress_disabled')
        began = perf_counter_ns()
        row = observation(arguments)
        fingerprint = digest(row)
        existing = self.operations.get(row['request_id'])
        if existing is not None:
            if existing[0] != fingerprint:
                raise ValueError('request_id_reused_with_different_content')
            return dict(status='observation_recorded', episode_id=existing[1], pair_snapshot_id=existing[2],
                current_pair_snapshot_id=self.pair.snapshot_id, idempotent_replay=True, grants_authority=False)
        memory, identifier = self.memory.append(row)
        added = memory is not self.memory
        graph = self.graph if not added else self.graph.append(memory.episode(identifier), digest((self.graph.inputs.snapshot_id, fingerprint)), memory)
        pair = FullCurrentMemoryVrsSnapshot(memory, graph.inputs.snapshot_id)
        operations = self.operations.set(row['request_id'], (fingerprint, identifier, pair.snapshot_id))
        self.db.execute('BEGIN IMMEDIATE')
        try:
            self.db.execute('INSERT INTO observations(request_id,body,fingerprint,pair) VALUES (?,?,?,?)',
                (row['request_id'], canonical(row), fingerprint, pair.snapshot_id))
            self.db.execute('COMMIT')
        except BaseException:
            if self.db.in_transaction:
                self.db.execute('ROLLBACK')
            raise
        self.owner.replace(self.pair.snapshot_id, pair)
        self.memory, self.graph, self.pair, self.operations = memory, graph, pair, operations
        return dict(status='observation_recorded', episode_id=identifier, pair_snapshot_id=pair.snapshot_id,
            revision=row['revision'], source=row['source'], outcome=row['outcome'],
            idempotent_replay=False, observation_only=True, grants_authority=False,
            current_truth_claimed=False, distinct_source_episode_added=int(added),
            vrs_event=graph.summary() if added else {'status':'unchanged_duplicate_observation'}, elapsed_ns=perf_counter_ns()-began)

    def recall(self, query, expected_snapshot):
        self._check()
        if expected_snapshot != self.pair.snapshot_id:
            raise ValueError('snapshot_mismatch')
        query = text_field(query, 'query', 4096)
        memory, graph, pair = self.memory, self.graph, self.pair
        candidates = keys(query)
        selected = tuple(c for c in candidates if memory.episode_ids_for_cue(c))
        # Complete explicit same-proposition evidence closure, including opponents
        # whose text has no query overlap. Historical outcomes alone never conflict.
        propositions = {memory.episode(i).steps[0].observation.get('proposition_id')
            for cue in selected for i in memory.episode_ids_for_cue(cue)} - {None}
        cues = (*selected, *('proposition:' + p for p in sorted(propositions)))
        opponents = {}
        for p in propositions:
            active = [memory.episode(i) for i in memory.propositions[p] if i not in memory.superseded]
            if {e.steps[0].observation['evidence_polarity'] for e in active} == {'support', 'refute'}:
                opponents[p] = tuple(sorted(e.episode_id for e in active))
        def judge(episode):
            p = episode.steps[0].observation.get('proposition_id')
            if p in opponents and episode.episode_id not in memory.superseded:
                return CurrentEvidenceVerdict(episode.episode_id, p, 'conflict',
                    'Opposing recorded claims for the same explicit proposition; neither is certified true.',
                    ('memory-snapshot:' + memory.snapshot_id, *episode.source_addresses), opponents[p])
            return current_experience_verdict(episode, memory_snapshot_id=memory.snapshot_id,
                vrs_snapshot_id=graph.inputs.snapshot_id, current_strength=graph.strength(episode.episode_id),
                proposition=p or 'experience:' + episode.episode_id)
        receipt = activate_memory(memory, query=query, current_cues=cues, judge=judge)
        ids = [c.episode_id for c in receipt.recall.candidates]
        selection = dict(candidate_counts={c: len(memory.episode_ids_for_cue(c)) for c in candidates},
            selected_cues=cues, rejected_cues=tuple(c for c in candidates if c not in selected),
            selection_method='all_matching_lexical_keys_and_explicit_proposition_closure',
            semantic_acceptance_claimed=False)
        return dict(receipt={'activation': receipt}, memory_selection=selection,
            vrs_selection=dict(selection_method='native_event_signal_and_connectivity_regions',
                numerical_version=VERSION, logical_implication_claimed=False, grants_authority=False),
            current_strengths={i: graph.strength(i) for i in ids}, current_promotions={i: graph.strength(i) >= 1.0 for i in ids},
            current_propositions={i: memory.episode(i).steps[0].observation.get('proposition_id') for i in ids},
            region_memberships={i: graph.memberships(i) for i in ids},
            last_vrs_event=graph.last_receipt,
            superseded_by={i: memory.superseded.get(i) for i in ids},
            pair_snapshot_id=pair.snapshot_id, grants_authority=False)

    def close(self):
        if self.closed:
            return
        self.closed = True
        if self.db is not None:
            self.db.close()
        self.lock.release()
