"""One durable main owner with immutable hot generations and observation ingress.

SQLite is an ingress/restart boundary only. Recall reads immutable resident maps.
No model, HTTP client, subprocess, Hermes store or action executor is used here.
"""
from __future__ import annotations

from dataclasses import dataclass, fields, is_dataclass, replace
import copyreg
import hashlib
import json
import pickle
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
    AtomicFullCurrentMemoryVrsOwner, current_experience_verdict,
    CurrentEvidenceVerdict, MemoryActivationReceipt, RecallResult,
    detect_deja_vu, recall_memory, replay_memory, re_evidence_memory,
)
import math

from .engine.mosaic_vrs_event_signal import VERSION
from .engine.mosaic_vrs_state_update import VRSConnectionStateUpdate, VRSStateUpdateReceipt
from .engine.mosaic_memory_promotion import assess_vrs_experience_promotion
from .flat_vrs import FlatGraph, settle, frozen  # noqa: F401 — tests import frozen from here
from .fast_regions import build_regions

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


class Graph:
    """One immutable VRS generation on flat arrays (local Windows-scale adapter, 2026-09-14).

    Same construction and rules as the ported composition: nodes are records plus
    ``cue:`` terms, edges record<->cue both ways (sign 1, strength .5), a new record
    gets direct .1, explicit same-proposition re-evidence edits outgoing strengths
    once per ingress, and the event signal settles to a float32 fixed point or the
    generation is not published. Representation and arithmetic vehicle come from
    ``flat_vrs``; see that module for the (bit-verified) equivalence.

    Connectivity regions are derived topology, prepared cold at ingress for the
    changed connected component only (unchanged components stay shared), exactly
    as the ported composition. Large components use ``fast_regions`` (same rule,
    vectorized, warm-started from the previous generation); see that module.
    """
    __slots__ = ('snapshot_id', 'flat', 'nodes', 'components', 'regions', 'last_receipt', '_names')

    def __init__(self, snapshot_id, flat, nodes, components, regions, last_receipt):
        self.snapshot_id, self.flat, self.nodes = snapshot_id, flat, nodes
        self.components, self.regions, self.last_receipt = components, regions, last_receipt
        self._names = None

    @classmethod
    def empty(cls, identity):
        return cls(digest(('vrs2', identity)), FlatGraph.empty(), Map(), Map(), Map(), MappingProxyType({}))

    @property
    def edge_count(self):
        return len(self.flat.src)

    @property
    def inputs(self):
        """Compatibility shim for callers that only need ``.snapshot_id``."""
        return SimpleNamespace(snapshot_id=self.snapshot_id)

    def names(self):
        if self._names is None:
            names = [None] * len(self.nodes)
            for name, index in self.nodes.items():
                names[index] = name
            self._names = tuple(names)
        return self._names

    def append(self, episode, snapshot, memory):
        # Full original episodes link to shared literal cues. These are numerical
        # dependencies/navigation associations, never syllogistic entailments.
        flat, nodes = self.flat, self.nodes
        new_names = [episode.episode_id] + [c for c in episode.cues if 'cue:' + c not in nodes]
        names = [episode.episode_id] + ['cue:' + c for c in new_names[1:]]
        for name in names:
            nodes = nodes.set(name, len(nodes))
        center = nodes[episode.episode_id]
        endpoints = [nodes['cue:' + c] for c in episode.cues]
        src = [x for n in endpoints for x in (center, n)]
        dst = [x for n in endpoints for x in (n, center)]
        direct = np.zeros(len(names), dtype=np.float32)
        direct[0] = .1                       # presence is a numerical observation signal, not truth
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
        for old in sorted(prior, key=lambda e: e.episode_id):
            opposing = old.steps[0].observation.get('evidence_polarity') != obs.get('evidence_polarity')
            retracted = opposing and old.episode_id == replaced_id
            conflict = opposing and not retracted
            # Another report from the same source/revision is retained, but is
            # not another independent strength update for this proposition.
            duplicate_source = old.source_addresses == episode.source_addresses and old.revision == episode.revision
            old_node = self.nodes[old.episode_id]
            for edge in sorted(int(e) for e in flat.out_edge[flat.out_ptr[old_node]:flat.out_ptr[old_node + 1]]):
                previous = float(flat.strength[edge])
                value = previous if conflict or duplicate_source else previous * (.995 if retracted else 1.01)
                action = 'abstain_conflict' if conflict else 'preserve_unresolved' if duplicate_source else 'weaken' if retracted else 'reinforce'
                verdict = 'conflict' if conflict else 'available' if duplicate_source else 'refute' if retracted else 'support'
                connection = 'vrs-edge:' + str(edge)
                updates.append(VRSConnectionStateUpdate(old.episode_id, connection, proposition, verdict,
                    previous, value, action, assess_vrs_experience_promotion(snapshot_id=snapshot,
                        connection_id=connection, previous_strength=previous, current_strength=value),
                    source_judgments=((episode.episode_id, proposition, verdict),)))
        # If any record opposes this new claim, do not reinforce matching records
        # on the same unresolved proposition either (falsification first).
        if any(u.verdict == 'conflict' for u in updates):
            updates = [replace(u, verdict='conflict', current_strength=u.previous_strength,
                update_action='abstain_conflict', promotion=assess_vrs_experience_promotion(
                    snapshot_id=snapshot, connection_id=u.connection_id,
                    previous_strength=u.previous_strength, current_strength=u.previous_strength)) for u in updates]
        strength_receipt = VRSStateUpdateReceipt(snapshot, tuple(updates))
        # Stored strength is rounded to float32 before arithmetic; an edit that does
        # not change the stored value is not a seed (engine _bind_strength_updates).
        edits, seeds = [], [center, *endpoints]
        for u in updates:
            edge = int(u.connection_id[len('vrs-edge:'):])
            value = np.float32(u.current_strength)
            if value.view(np.uint32) != np.float32(flat.strength[edge]).view(np.uint32):
                edits.append((edge, value))
                seeds.append(int(flat.dst[edge]))
        grown = flat.extend(new_direct=direct, new_src=src, new_dst=dst, new_sign=[1] * len(src),
                            new_strength=[.5] * len(src), strength_updates=edits)
        settled, fields = settle(grown, seeds, maximum_rounds=512)
        if fields['pending_node_count']:
            raise ValueError('vrs_signal_pending_no_publication')
        changed = np.flatnonzero(settled.view(np.uint32) != grown.score.view(np.uint32))
        score_ids = [int(i) for i in changed]
        # Same bytes as digest((snapshot, 'settled', [(i, score), ...])) without the generic
        # plain() recursion over thousands of pairs.
        payload = json.dumps([snapshot, 'settled', [[i, float(settled[i])] for i in score_ids]],
                             ensure_ascii=False, sort_keys=True, separators=(',', ':'), allow_nan=False)
        settled_id = hashlib.sha256(payload.encode('utf-8')).hexdigest()
        grown = grown.with_score(settled)
        # Recompute only the changed connected component. Modularity within that
        # component is global; unchanged disconnected components stay shared.
        members = self._component_members(grown, [center, *(self.nodes[e.episode_id] for e in prior)])
        edge_mask = np.isin(grown.src, members)
        edges = np.flatnonzero(edge_mask)
        local = np.full(len(nodes), -1, dtype=np.int64)
        local[members] = np.arange(len(members))
        all_names = self.names() + tuple(names)
        source = SimpleNamespace(terms=tuple(all_names[i] for i in members),
            edge_source=local[grown.src[edges].astype(np.int64)],
            edge_target=local[grown.dst[edges].astype(np.int64)],
            edge_sign=grown.sign[edges].astype(np.int8),
            vrs_strength=grown.strength[edges].astype(np.float64))
        # A merge can only happen through the new record's own edges, so the previous
        # component ids are exactly those of its endpoints that already existed.
        previous_ids = {self.components[n] for n in endpoints if n in self.components}
        previous = self.regions[next(iter(previous_ids))] if len(previous_ids) == 1 else None
        regions, backend = build_regions(source, vrs_snapshot_id=settled_id, previous=previous)
        if not regions.converged:
            raise ValueError('region_topology_pending_no_publication')
        component_id = int(members[0])
        components, directory = self.components, self.regions
        for old in previous_ids:
            directory = directory.delete(old)
        for n in members:
            n = int(n)
            if components.get(n) != component_id:
                components = components.set(n, component_id)
        directory = directory.set(component_id, (regions, Map({int(n): i for i, n in enumerate(members)})))
        receipt = dict(version=fields['version'], parent_snapshot_id=self.snapshot_id,
            status=fields['status'], pending_node_count=0, rounds=fields['rounds'],
            node_evaluations=fields['node_evaluations'], edge_evaluations=fields['edge_evaluations'],
            changed_scores=len(score_ids), changed_strengths=len(edits),
            legacy_numerical_equivalence=False, whole_graph_convergence_claimed=False,
            logical_implication_claimed=False, cognitive_completion=False,
            persistent_state_mutated=False, authority_granted=False,
            strength_updates_reapplied_during_iterations=0, numerical_compatibility_controls_strength=False,
            strength_storage_dtype='<f4', storage_binding_verified=True,
            input_strength_proposal_count=len(updates),
            arithmetic_vehicle='flat_vectorized_same_rule',
            changed_component_nodes=len(members), changed_component_edges=len(edges),
            global_recomputation_reason='Only affected connected component: modularity equivalence cannot be guaranteed by local moves alone.',
            region_backend=backend, region_sweeps=list(regions.sweeps),
            source_episode_count_added=1, historical_outcome=episode.steps[0].outcome,
            re_evidence_updates=plain(strength_receipt),
            recorded_agreement_is_not_independent_factual_corroboration=True,
            logical_implication_claimed_by_regions=False, grants_authority=False)
        successor = Graph(settled_id, grown, nodes, components, directory, freeze_view(receipt))
        successor._names = all_names
        return successor

    @staticmethod
    def _component_members(flat, starts):
        """Sorted node ids of the connected component(s) reachable from ``starts``."""
        seen = np.zeros(flat.count, dtype=bool)
        frontier = np.unique(np.asarray(starts, np.int64))
        seen[frontier] = True
        while len(frontier):
            lo, hi = flat.out_ptr[frontier], flat.out_ptr[frontier + 1]
            counts = hi - lo
            total = int(counts.sum())
            if not total:
                break
            idx = np.repeat(lo, counts) + (np.arange(total) - np.repeat(np.cumsum(counts) - counts, counts))
            reached = flat.dst[flat.out_edge[idx]].astype(np.int64)
            reached = reached[~seen[reached]]
            frontier = np.unique(reached)
            seen[frontier] = True
        return np.flatnonzero(seen)

    def memberships(self, identifier):
        node = self.nodes[identifier]
        region, positions = self.regions[self.components[node]]
        return tuple((region.topology_id, n, w) for n, w in region.memberships_for_term(positions[node]))

    def strength(self, identifier):
        node = self.nodes[identifier]
        lo, hi = self.flat.out_ptr[node], self.flat.out_ptr[node + 1]
        edges = self.flat.out_edge[lo:hi]
        return float(self.flat.strength[edges].max()) if len(edges) else 0.0

    def summary(self):
        return {k: plain(v) for k, v in self.last_receipt.items() if k != 're_evidence_updates'}


def _mapping_proxy(data):
    return MappingProxyType(data)


# pickle cannot name the mappingproxy type; rebuild frozen views through a module function.
copyreg.pickle(MappingProxyType, lambda m: (_mapping_proxy, (dict(m),)))
CHECKPOINT_EVERY = 8      # bounds crash replay to a few ingests; close() always checkpoints


class Main:
    """Sole owner for one state directory; transaction commit precedes publication.

    Restart: load the last checkpoint (pickled hot index, flat VRS generation and
    request table), verify it against the journal row it claims, then replay only
    the observations after it. Without a matching checkpoint the whole journal is
    replayed as before. The journal remains the source of truth; a checkpoint is a
    verified cache written every CHECKPOINT_EVERY ingests and on close.
    """
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
        self._dirty = 0
        self.restore = {}
        try:
            # check_same_thread=False: the loopback daemon serves requests from handler threads
            # and serializes every main call under one lock (loopback.Daemon.handle).
            self.db = sqlite3.connect(self.directory / 'memory.sqlite3', isolation_level=None,
                                      check_same_thread=False)
            self.db.execute('PRAGMA journal_mode=WAL')
            self.db.execute('PRAGMA synchronous=FULL')
            self.db.execute('CREATE TABLE IF NOT EXISTS identity (id INTEGER PRIMARY KEY CHECK(id=1), value TEXT NOT NULL)')
            self.db.execute('CREATE TABLE IF NOT EXISTS observations (seq INTEGER PRIMARY KEY, request_id TEXT UNIQUE NOT NULL, body TEXT NOT NULL, fingerprint TEXT NOT NULL, pair TEXT NOT NULL)')
            self.db.execute('CREATE TABLE IF NOT EXISTS checkpoint (id INTEGER PRIMARY KEY CHECK(id=1), seq INTEGER NOT NULL, pair TEXT NOT NULL, blob BLOB NOT NULL)')
            identity = self.db.execute('SELECT value FROM identity WHERE id=1').fetchone()
            if identity is None:
                identity = (str(uuid.uuid4()),)
                self.db.execute('INSERT INTO identity VALUES (1,?)', identity)
            self.identity = identity[0]
            self.memory = HotIndex(digest(('memory', self.identity)), Map(), Map(), Map({o: 0 for o in OUTCOMES}), Map(), Map())
            self.graph = Graph.empty(self.identity)
            self.operations = Map()
            self.pair = FullCurrentMemoryVrsSnapshot(self.memory, self.graph.snapshot_id)
            started = perf_counter_ns()
            after = self._load_checkpoint()
            # The journal stays the source of truth: every row the checkpoint claims to
            # cover is re-validated (fingerprint and request identity) before use.
            for req, body, fingerprint in self.db.execute(
                    'SELECT request_id,body,fingerprint FROM observations WHERE seq<=? ORDER BY seq', (after,)):
                row = observation(json.loads(body))
                if digest(row) != fingerprint or row['request_id'] != req:
                    raise ValueError('stored_observation_integrity_failed')
            replayed = 0
            for req, body, fingerprint, expected in self.db.execute(
                    'SELECT request_id,body,fingerprint,pair FROM observations WHERE seq>? ORDER BY seq', (after,)):
                row = observation(json.loads(body))
                if digest(row) != fingerprint or row['request_id'] != req:
                    raise ValueError('stored_observation_integrity_failed')
                memory, identifier = self.memory.append(row)
                graph = self.graph if memory is self.memory else self.graph.append(memory.episode(identifier), digest((self.graph.snapshot_id, fingerprint)), memory)
                pair = FullCurrentMemoryVrsSnapshot(memory, graph.snapshot_id)
                if pair.snapshot_id != expected:
                    raise ValueError('stored_generation_integrity_failed')
                self.memory, self.graph, self.pair = memory, graph, pair
                self.operations = self.operations.set(req, (fingerprint, identifier, pair.snapshot_id))
                replayed += 1
            self._dirty = replayed
            self.restore.update(replayed_after_checkpoint=replayed, elapsed_ns=perf_counter_ns() - started)
            self.owner = AtomicFullCurrentMemoryVrsOwner(self.pair)
        except BaseException:
            self.close()
            raise

    def _load_checkpoint(self):
        """Return the journal seq the loaded state corresponds to (0 = nothing loaded)."""
        row = self.db.execute('SELECT seq, pair, blob FROM checkpoint WHERE id=1').fetchone()
        if row is None:
            self.restore['checkpoint'] = 'none'
            return 0
        seq, pair, blob = row
        journal = self.db.execute('SELECT pair FROM observations WHERE seq=?', (seq,)).fetchone()
        if journal is None or journal[0] != pair:
            self.restore['checkpoint'] = 'stale_ignored'
            return 0
        try:
            state = pickle.loads(blob)
            if state['pair'] != pair or state['identity'] != self.identity:
                raise ValueError('checkpoint identity mismatch')
        except Exception as error:
            self.restore['checkpoint'] = 'unreadable_ignored: ' + type(error).__name__
            return 0
        self.memory, self.graph, self.operations = state['memory'], state['graph'], state['operations']
        self.pair = FullCurrentMemoryVrsSnapshot(self.memory, self.graph.snapshot_id)
        if self.pair.snapshot_id != pair:
            raise ValueError('checkpoint_generation_integrity_failed')
        self.restore['checkpoint'] = f'loaded seq {seq}'
        return seq

    def checkpoint(self):
        """Write the current verified state as a checkpoint for the latest journal row."""
        self._check()
        row = self.db.execute('SELECT seq, pair FROM observations ORDER BY seq DESC LIMIT 1').fetchone()
        if row is None:
            return None
        seq, pair = row
        if pair != self.pair.snapshot_id:
            raise ValueError('checkpoint_does_not_match_journal_head')
        started = perf_counter_ns()
        blob = pickle.dumps(dict(identity=self.identity, pair=pair, memory=self.memory,
                                 graph=self.graph, operations=self.operations), protocol=pickle.HIGHEST_PROTOCOL)
        self.db.execute('BEGIN IMMEDIATE')
        try:
            self.db.execute('INSERT OR REPLACE INTO checkpoint(id, seq, pair, blob) VALUES (1,?,?,?)', (seq, pair, blob))
            self.db.execute('COMMIT')
        except BaseException:
            if self.db.in_transaction:
                self.db.execute('ROLLBACK')
            raise
        self._dirty = 0
        return dict(seq=seq, bytes=len(blob), elapsed_ns=perf_counter_ns() - started)

    def _check(self):
        if self.closed:
            raise ValueError('main_closed')

    def status(self):
        self._check()
        return dict(status='ready', identity=self.identity, pair_snapshot_id=self.pair.snapshot_id,
            memory_snapshot_id=self.memory.snapshot_id, vrs_snapshot_id=self.graph.snapshot_id,
            hot_episode_count=len(self.memory.records), outcome_counts=dict(self.memory.outcome_counts),
            lookup_requires_io=False, internal_llm_calls=0, final_utterance_calls=0,
            backend='standalone_native_vrs2', native_engine_bundled=True, scheduled_dialogue_available=True,
            writes_enabled=self.allow_ingest, authority=dict(AUTHORITY), numerical_version=VERSION,
            vrs_node_count=len(self.graph.nodes), vrs_edge_count=self.graph.edge_count,
            last_vrs_event=self.graph.summary(), restore=dict(self.restore),
            checkpoint_pending_ingests=self._dirty)

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
        graph = self.graph if not added else self.graph.append(memory.episode(identifier), digest((self.graph.snapshot_id, fingerprint)), memory)
        pair = FullCurrentMemoryVrsSnapshot(memory, graph.snapshot_id)
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
        self._dirty += 1
        if self._dirty >= CHECKPOINT_EVERY:
            self.checkpoint()
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
        fanout = {c: len(memory.episode_ids_for_cue(c)) for c in candidates}
        selected = tuple(c for c in candidates if fanout[c])
        # Local adapter (2026-09-14): a cue carried by half the store or more is a
        # function word for this corpus (Korean one-character particles such as
        # 왜/안 match nearly every record). Such cues still generate candidates —
        # nothing is dropped — but they neither open proposition closure nor weigh
        # in candidate order; otherwise every proposition in the store is pulled
        # in by one particle and its closure cue inflates unrelated records.
        total = max(1, memory.episode_count)
        informative = tuple(c for c in selected if fanout[c] * 2 <= total)
        # Complete explicit same-proposition evidence closure, including opponents
        # whose text has no query overlap. Historical outcomes alone never conflict.
        propositions = {memory.episode(i).steps[0].observation.get('proposition_id')
            for cue in informative for i in memory.episode_ids_for_cue(cue)} - {None}
        cues = (*selected, *('proposition:' + p for p in sorted(propositions)))
        # Candidate order: rarity-weighted matched cues first, then the engine's Jaccard.
        # The engine states candidate order is not semantic acceptance; the whole set
        # stays addressable. Jaccard alone ranks short records above rich ones that
        # match more of the question (v0.2 verdict obs:cue-overlap-penalizes-rich-documents).
        weight = {c: math.log(1.0 + total / fanout[c]) for c in informative}
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
                vrs_snapshot_id=graph.snapshot_id, current_strength=graph.strength(episode.episode_id),
                proposition=p or 'experience:' + episode.episode_id)
        signal = detect_deja_vu(memory, query=query, current_cues=cues)
        recalled = recall_memory(memory, signal)
        def order(row):
            return (-sum(weight.get(c, 0.0) for c in row.matched_cues), -row.cue_overlap, row.episode_id)
        recalled = RecallResult(recalled.query, tuple(sorted(recalled.candidates, key=order)),
                                recalled.snapshot_id, source_dependencies=recalled.source_dependencies)
        replayed = replay_memory(memory, recalled)
        re_evidenced = re_evidence_memory(replayed, judge=judge)
        receipt = MemoryActivationReceipt(schema_version='rozephine-memory-activation-v1',
            snapshot_id=memory.snapshot_id, deja_vu=signal, recall=recalled,
            replay=replayed, re_evidence=re_evidenced)
        ids = [c.episode_id for c in receipt.recall.candidates]
        selection = dict(candidate_counts={c: fanout[c] for c in candidates},
            selected_cues=cues, rejected_cues=tuple(c for c in candidates if c not in selected),
            function_word_cues=tuple(c for c in selected if c not in informative),
            selection_method='all_matching_lexical_keys_and_explicit_proposition_closure',
            closure_rule='propositions of records matched by an informative cue (fanout below half the store)',
            candidate_order='rarity_weighted_matched_cues_then_cue_overlap',
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
        try:
            if self.db is not None and self._dirty and self.allow_ingest and hasattr(self, 'owner'):
                self.checkpoint()
        except Exception:
            pass                        # 체크포인트는 캐시다 — 닫기를 막지 않는다
        self.closed = True
        if self.db is not None:
            self.db.close()
        self.lock.release()
