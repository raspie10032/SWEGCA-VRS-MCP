"""One durable main owner with immutable hot generations and observation ingress.

SQLite is an ingress/restart boundary only. Recall reads immutable resident maps.
No model, HTTP client, subprocess, external-agent store or action executor is used here.
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
import os
import uuid
import zlib

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
from .compact_index import CompactIndex
from . import csr_cache
from . import vrs_refine
from .engine.mosaic_vrs_connectivity_regions import _csr as _engine_csr

# Local adapter (2026-09-14): generated Hangul n-gram cues (2-4 chars, produced by keys() for every
# Hangul word) stay retrieval keys in the postings but do not become VRS nodes. As nodes they joined
# every record into one component (2,620 records: 168,699 nodes / 1.64M edges, every ingest settled
# and re-regioned the whole graph). Candidate order never used VRS strengths, so recall is unchanged.
# Set VRS2_GRAPH_SUBSTRING_CUES=1 to restore the upstream node set (then rebuild the graph).
GRAPH_SUBSTRING_CUES = os.environ.get('VRS2_GRAPH_SUBSTRING_CUES') == '1'
_HANGUL_FRAGMENT = re.compile('[가-힣]{2,4}')


def graph_cue_ids(memory, row):
    """Cue ids of record ``row`` that become graph endpoints."""
    ids = [int(c) for c in memory._store['cues'][row]]
    if GRAPH_SUBSTRING_CUES:
        return ids
    vocab = memory._store['vocab']
    names = [vocab.string_of(c) for c in ids]
    blob = '\x00'.join(names)
    # a short pure-Hangul cue that also occurs inside another cue of the same record is a fragment
    return [c for c, n in zip(ids, names)
            if not (_HANGUL_FRAGMENT.fullmatch(n) and blob.count(n) >= 2)]


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
    folded = text.casefold()
    exact = re.fullmatch(r'\s*(memory:[0-9a-f]{64})\s*', folded)
    if exact:
        # An explicit original address is already the complete lookup key. Do
        # not also admit the generic token ``memory`` and the hexadecimal tail;
        # doing so turns one exact read into a whole-store lexical fanout.
        return (exact.group(1),)
    result = dict.fromkeys(re.findall(r'\w+', folded))
    result.update(dict.fromkeys(re.findall(r'memory:[0-9a-f]{64}', folded)))
    # Local adapter (2026-09-14): a mixed-script token (270인지, 폴더블8, 8월) also addresses
    # its script runs. Upstream gives such a token neither a whole-token match against the
    # separately written form ("big_chunk 270") nor the Hangul substrings below.
    for word in tuple(result):
        if not re.fullmatch('[가-힣]+', word):
            result.update(dict.fromkeys(part for part in re.findall('[가-힣]+|[^가-힣]+', word) if part != word))
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


from .compact_index import asks_and_description, LightView  # noqa: E402  (recall columns, 2026-09-18)


def asks_of(text):
    """The record's own 「찾을 때 묻는 말」: a verdict's tail line, a memory doc's section heading, or a
    log entry's parenthesized tail — never an inline mention of the phrase (a doc that *talks about*
    asks would otherwise claim every example word it quotes; measured 2026-09-15). '' when absent."""
    return asks_and_description(text)[0]


def journal_entry(request_id, body, fingerprint):
    """Validate one journal row: ('observation', row) or ('consolidation', spec). Both are fingerprinted."""
    entry = json.loads(body)
    if isinstance(entry, dict) and entry.get('kind') == 'alias':
        # hypothesis registry (2026-09-18): {canonical, aliases} — one hypothesis for several phrasings
        if digest(entry) != fingerprint or not request_id.startswith('alias:'):
            raise ValueError('stored_observation_integrity_failed')
        return 'alias', entry
    if isinstance(entry, dict) and entry.get('kind') == 'usage':
        # usage re-evidence (2026-09-18): {source: [injected, opened]} counts from the hooks' ledger
        if digest(entry) != fingerprint or not request_id.startswith('usage:'):
            raise ValueError('stored_observation_integrity_failed')
        return 'usage', entry
    if isinstance(entry, dict) and entry.get('kind') == 'consolidation':
        if digest(entry) != fingerprint or not request_id.startswith('consolidation:'):
            raise ValueError('stored_observation_integrity_failed')
        return 'consolidation', entry
    row = observation(entry)
    if digest(row) != fingerprint or row['request_id'] != request_id:
        raise ValueError('stored_observation_integrity_failed')
    return 'observation', row


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


class NodeDirectory:
    """Node ids for records and cues without a resident string per node.

    ``episode_node``: record id -> node; ``cue_node[cue_id]``: node or -1; ``node_cue[node]``:
    cue id or -1; ``node_episode``: node -> record id (records only). Names are decoded
    from the shared vocabulary on demand (``name(node)``), so ``'cue:'+string`` keys are
    never materialized for the whole graph. Supports the old ``graph.nodes[...]`` reads.
    """
    __slots__ = ('vocab', 'episode_node', 'node_episode', 'cue_node', 'node_cue', 'count')

    def __init__(self, vocab, episode_node, node_episode, cue_node, node_cue, count):
        self.vocab, self.count = vocab, count
        self.episode_node, self.node_episode = episode_node, node_episode
        self.cue_node, self.node_cue = cue_node, node_cue

    @classmethod
    def empty(cls, vocab):
        return cls(vocab, Map(), Map(), np.full(0, -1, np.int32), np.full(0, -1, np.int32), 0)

    def __len__(self):
        return self.count

    def __contains__(self, name):
        return self.get(name) is not None

    def __getitem__(self, name):
        node = self.get(name)
        if node is None:
            raise KeyError(name)
        return node

    def get(self, name):
        if name.startswith('cue:'):
            cue_id = self.vocab.id_of(name[4:])
            if cue_id is None or cue_id >= len(self.cue_node) or self.cue_node[cue_id] < 0:
                return None
            return int(self.cue_node[cue_id])
        return self.episode_node.get(name)

    def cue(self, cue_id):
        return int(self.cue_node[cue_id]) if cue_id < len(self.cue_node) else -1

    def name(self, node):
        cue_id = int(self.node_cue[node])
        return 'cue:' + self.vocab.string_of(cue_id) if cue_id >= 0 else self.node_episode[node]

    def items(self):
        for node in range(self.count):
            yield self.name(node), node

    def extend(self, episode_id, new_cue_ids):
        """Successor with one record node followed by nodes for ``new_cue_ids`` (fresh cues)."""
        first = self.count
        total = first + 1 + len(new_cue_ids)
        need = (max(new_cue_ids) + 1) if new_cue_ids else len(self.cue_node)
        cue_node = np.full(max(len(self.cue_node), need), -1, np.int32)
        cue_node[:len(self.cue_node)] = self.cue_node
        node_cue = np.full(total, -1, np.int32)
        node_cue[:first] = self.node_cue
        for offset, cue_id in enumerate(new_cue_ids, first + 1):
            cue_node[cue_id] = offset
            node_cue[offset] = cue_id
        return NodeDirectory(self.vocab, self.episode_node.set(episode_id, first),
                             self.node_episode.set(first, episode_id), frozen(cue_node, np.int32),
                             frozen(node_cue, np.int32), total)


class LazyTerms:
    """Sequence of node names for a component, decoded from the directory on access."""
    __slots__ = ('directory', 'members')

    def __init__(self, directory, members):
        self.directory, self.members = directory, members

    def __len__(self):
        return len(self.members)

    def __getitem__(self, index):
        if isinstance(index, slice):
            return tuple(self[i] for i in range(*index.indices(len(self))))
        return self.directory.name(int(self.members[index]))

    def __iter__(self):
        for member in self.members:
            yield self.directory.name(int(member))


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
    # '_names' is kept only so checkpoints written by the earlier Graph layout still unpickle.
    # '_csr' caches the level-0 regions adjacency of the whole-graph component (csr_cache);
    # it is memory-only: not pickled, rebuilt once through the engine's _csr after a restart.
    __slots__ = ('snapshot_id', 'flat', 'nodes', 'components', 'regions', 'last_receipt', '_names', '_csr', 'stable', '_labels', '_row_labels',
                 'usage', 'aliases')

    def __init__(self, snapshot_id, flat, nodes, components, regions, last_receipt, csr=None, stable=None, usage=None,
                 aliases=None):
        self.snapshot_id, self.flat, self.nodes = snapshot_id, flat, nodes
        self.components, self.regions, self.last_receipt = components, regions, last_receipt
        self._csr = csr
        self.stable = stable                 # vrs_refine.VRSVersion of the last consolidation, or None
        self.usage = dict(usage or {})       # usage re-evidence: record source -> [injected, opened] (journaled)
        self.aliases = dict(aliases or {})   # hypothesis registry: alias proposition -> canonical (journaled)
        self._labels = None                  # memory-only: core region label per node (see labels())
        self._row_labels = None              # memory-only: region label per memory row (see row_labels())

    def __getstate__(self):
        return {k: getattr(self, k) for k in self.__slots__ if k not in ('_csr', '_labels', '_row_labels') and hasattr(self, k)}

    def __setstate__(self, state):
        if isinstance(state, tuple):          # checkpoints written before __getstate__ existed
            state = state[1] or {}
        for k in self.__slots__:
            setattr(self, k, state.get(k))
        if self.usage is None:               # checkpoints written before usage existed
            self.usage = {}
        if self.aliases is None:
            self.aliases = {}

    @classmethod
    def empty(cls, identity, vocab=None):
        return cls(digest(('vrs2', identity)), FlatGraph.empty(), NodeDirectory.empty(vocab), Map(), Map(),
                   MappingProxyType({}))

    @property
    def edge_count(self):
        return len(self.flat.src)

    @property
    def alias_digest(self):
        return digest(sorted(self.aliases.items())) if self.aliases else ''

    def with_aliases(self, canonical, aliases):
        """Successor generation with alias propositions bound to a canonical one (chained into the snapshot id)."""
        merged = dict(self.aliases)
        canonical = merged.get(canonical, canonical)             # an alias of an alias lands on the root
        for a in aliases:
            if a != canonical:
                merged[str(a)] = str(canonical)
        for a, c in list(merged.items()):                        # keep the map flat
            while c in merged and merged[c] != c:
                c = merged[c]
            merged[a] = c
        snapshot_id = digest((self.snapshot_id, 'alias', digest(sorted(merged.items()))))
        receipt = dict(self.last_receipt, status='alias', parent_snapshot_id=self.snapshot_id, aliases=len(merged))
        return Graph(snapshot_id, self.flat, self.nodes, self.components, self.regions, freeze_view(receipt), self._csr,
                     self.stable, self.usage, merged)

    @property
    def usage_digest(self):
        return digest(sorted((k, list(v)) for k, v in self.usage.items())) if self.usage else ''

    def with_usage(self, counts):
        """Successor generation with usage counts merged in (same topology, strengths, stable version).
        The snapshot id chains through the counts, so a replay lands on the same pair id."""
        merged = dict(self.usage)
        for source, pair in counts.items():
            merged[str(source)] = [int(pair[0]), int(pair[1])]
        snapshot_id = digest((self.snapshot_id, 'usage', digest(sorted((k, v) for k, v in merged.items()))))
        receipt = dict(self.last_receipt, status='usage', parent_snapshot_id=self.snapshot_id, usage_sources=len(merged))
        return Graph(snapshot_id, self.flat, self.nodes, self.components, self.regions, freeze_view(receipt), self._csr,
                     self.stable, merged, self.aliases)

    @property
    def inputs(self):
        """Compatibility shim for callers that only need ``.snapshot_id``."""
        return SimpleNamespace(snapshot_id=self.snapshot_id)

    def append(self, episode, snapshot, memory):
        """One record = one generation (the pre-batch path); identical certificate chain as before."""
        return self.append_many([episode], snapshot, memory)

    def append_many(self, episodes, snapshot, memory):
        """K records enter as one generation (batch generations, 2026-09-18).

        Every ingest used to rebuild the whole flat graph and its regions (O(E) per record, E ~ 200 N):
        measured 500 ms per record at 5.6k records. A batch extends the flat arrays once and builds the
        regions once. ``snapshot`` is the fingerprint fold over the batch's added records; the journal rows
        of a batch share the resulting pair id, and replay regroups consecutive equal-pair rows into the same
        batch, so the region labels (which the consolidation certificate digests) are reproduced exactly.
        A batch of one is byte-for-byte the old single-record generation.
        """
        # Full original episodes link to shared literal cues. These are numerical
        # dependencies/navigation associations, never syllogistic entailments.
        episodes = list(episodes)
        if not episodes:
            raise ValueError('append_many_needs_at_least_one_episode')
        flat = self.flat
        vocab = memory._store['vocab']
        nodes = self.nodes if self.nodes.vocab is not None else NodeDirectory.empty(vocab)
        old_count = len(nodes)
        src, dst, sign = [], [], []
        direct, unresolved = [], []          # per new node, in directory order (record, then its fresh cues)
        node_updates, late_updates = [], {}  # existing nodes / nodes created earlier in this batch
        polarities = []
        for episode in episodes:
            row = memory._store['row_of'][episode.episode_id]
            cue_ids = graph_cue_ids(memory, row)
            fresh = [c for c in cue_ids if nodes.cue(c) < 0]
            nodes = nodes.extend(episode.episode_id, fresh)
            center = nodes[episode.episode_id]
            endpoints = [nodes.cue(c) for c in cue_ids]
            # vrs-regions (2026-09-15): the record enters as a v0.2 event. Resolved (outcome success/failure or
            # an explicit proposition polarity) -> direct tanh(1); otherwise 0 and unresolved, so its edges can
            # only decay at consolidation. Edge sign = the record's polarity. No numerical settling at ingress:
            # strengths sit at base until the next consolidation refines them region by region.
            polarity = vrs_refine.record_polarity(episode, memory.superseded)
            direct.append(np.tanh(1.0) if polarity else 0.0); unresolved.append(not polarity)
            direct.extend([0.0] * len(fresh)); unresolved.extend([True] * len(fresh))
            replaced_id = episode.steps[0].observation.get('supersedes')
            if replaced_id is not None and replaced_id in nodes.episode_node:
                replaced = nodes.episode_node[replaced_id]
                if replaced < old_count:
                    node_updates.append((replaced, 0.0, True))   # superseded: no longer resolved
                else:
                    late_updates[replaced - old_count] = True    # superseded within this batch
            src.extend(x for n in endpoints for x in (center, n))
            dst.extend(x for n in endpoints for x in (n, center))
            sign.extend([polarity or 1] * (2 * len(endpoints)))
            polarities.append(bool(polarity))
        direct = np.asarray(direct, dtype=np.float32)
        new_unresolved = np.asarray(unresolved, dtype=bool)
        for offset in late_updates:
            direct[offset] = 0.0; new_unresolved[offset] = True
        grown = flat.extend(new_direct=direct, new_src=src, new_dst=dst, new_sign=sign,
                            new_strength=[float(vrs_refine.BASE)] * len(src), new_unresolved=new_unresolved,
                            node_updates=node_updates)
        stable_id = self.stable.version_id if self.stable is not None else 'none'
        ids = episodes[0].episode_id if len(episodes) == 1 else [e.episode_id for e in episodes]
        payload = json.dumps([snapshot, 'append', ids, len(grown.src), stable_id],
                             ensure_ascii=False, sort_keys=True, separators=(',', ':'), allow_nan=False)
        settled_id = hashlib.sha256(payload.encode('utf-8')).hexdigest()
        # Region topology, memberships and portals are derived consolidation state.
        # Building them for every pending ingress batch rebuilt the same whole graph
        # hundreds of times. Keep the last stable directory for existing nodes and
        # leave new nodes explicitly pending; the idle consolidation rebuilds the
        # complete topology once before numerical refinement.
        components, directory, csr = self.components, self.regions, None
        backend, region_sweeps = 'deferred_until_idle_consolidation', []
        receipt = dict(version=vrs_refine.VERSION, parent_snapshot_id=self.snapshot_id,
            status='appended_pending_consolidation', pending_node_count=0,
            pending_edges=len(grown.src) - (self.stable.edge_count if self.stable is not None else 0),
            stable_version_id=stable_id, resolved=polarities[-1] if len(episodes) == 1 else sum(polarities),
            superseded_marked=len(node_updates) + len(late_updates),
            legacy_numerical_equivalence=False, whole_graph_convergence_claimed=False,
            logical_implication_claimed=False, cognitive_completion=False,
            persistent_state_mutated=False, authority_granted=False,
            arithmetic_vehicle='region_consolidation_at_idle',
            changed_component_nodes=len(nodes) - old_count, changed_component_edges=len(src),
            region_backend=backend, region_sweeps=region_sweeps,
            source_episode_count_added=len(episodes), historical_outcome=episodes[-1].steps[0].outcome,
            recorded_agreement_is_not_independent_factual_corroboration=True,
            logical_implication_claimed_by_regions=False, grants_authority=False)
        # usage/aliases ride along (before 2026-09-18 an append silently dropped both journaled maps)
        return Graph(settled_id, grown, nodes, components, directory, freeze_view(receipt), csr, self.stable,
                     self.usage, self.aliases)

    def rebuild_regions(self):
        """Build regions for components touched by nodes appended since consolidation.

        Unrelated component entries remain structurally shared. A pending node may
        join several older components; its graph traversal yields their full merged
        component, whose old directory entries are replaced together.
        """
        flat, nodes = self.flat, self.nodes
        pending = [node for node in range(flat.count) if node not in self.components]
        if not pending:
            return self
        seen = np.zeros(flat.count, dtype=bool)
        components, directory = self.components, self.regions
        for start in pending:
            if seen[start]:
                continue
            members = self._component_members(flat, [start])
            seen[members] = True
            previous_ids = {components[node] for node in members if node in components}
            previous = directory.get(next(iter(previous_ids))) if len(previous_ids) == 1 else None
            edges = np.flatnonzero(np.isin(flat.src, members))
            local = np.full(flat.count, -1, dtype=np.int64)
            local[members] = np.arange(len(members))
            source = SimpleNamespace(terms=LazyTerms(nodes, frozen(members, np.int64)),
                edge_source=local[flat.src[edges].astype(np.int64)], edge_target=local[flat.dst[edges].astype(np.int64)],
                edge_sign=flat.sign[edges].astype(np.int8), vrs_strength=flat.strength[edges].astype(np.float64))
            regions, _ = build_regions(source, vrs_snapshot_id=self.snapshot_id, previous=previous)
            if not regions.converged:
                raise ValueError('region_topology_pending_no_publication')
            for old in previous_ids:
                directory = directory.delete(old)
            component_id = int(members[0])
            for n in members:
                components = components.set(int(n), component_id)
            directory = directory.set(component_id, (regions, frozen(local, np.int32)))
        return Graph(self.snapshot_id, flat, nodes, components, directory, self.last_receipt, None, self.stable,
                     self.usage, self.aliases)

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
        if node not in self.components:
            return ()
        region, positions = self.regions[self.components[node]]
        return tuple((region.topology_id, n, w) for n, w in region.memberships_for_term(positions[node]))

    def strength(self, identifier):
        """Max refined strength over the record's edges (cue edges, and its proposition edge if any)."""
        node = self.nodes[identifier]
        lo, hi = self.flat.out_ptr[node], self.flat.out_ptr[node + 1]
        edges = self.flat.out_edge[lo:hi]
        best = float(self.flat.strength[edges].max()) if len(edges) else 0.0
        if self.stable is not None:
            prop = self.stable.proposition_strength(node)
            if prop is not None:
                best = max(best, prop)
        return best

    def labels(self):
        """Region label per node: the stable version's (fine) labels, padded with -1 for nodes appended since
        (pending: always in scope, never activate a region); before any consolidation, the coarse labels."""
        if self._labels is None:
            stable = self.stable
            if stable is not None and getattr(stable, 'labels', None) is not None:
                fine = np.asarray(stable.labels, np.int64)
                n = self.flat.count
                self._labels = np.concatenate([fine[:n], np.full(max(0, n - len(fine)), -1, np.int64)])
            else:
                self._labels = vrs_refine.region_labels(self)
        return self._labels

    def lazy_terms(self, members):
        return LazyTerms(self.nodes, frozen(np.asarray(members, np.int64)))

    def row_labels(self, memory):
        """Region label per memory row (rows without a graph node -> -1), cached per generation."""
        cached = self._row_labels
        if cached is not None and len(cached) == memory.count:
            return cached
        labels = self.labels()
        ids = memory._store['ids']
        node_of = self.nodes.episode_node
        out = np.full(memory.count, -1, np.int64)
        for row in range(memory.count):
            node = node_of.get(ids[row])
            if node is not None and node < len(labels):
                out[row] = labels[node]
        self._row_labels = out
        return out

    def region_of(self, identifier):
        node = self.nodes.episode_node.get(identifier)
        return None if node is None else int(self.labels()[node])

    # ── G5 (2026-09-19, R3/R4): soft multi-membership from the stable version ──
    def memberships_of(self, identifier):
        """(region, weight) fine-region memberships of a record from the last consolidation, strongest first;
        () for a record appended since or before any consolidation."""
        node = self.nodes.episode_node.get(identifier)
        if node is None or self.stable is None:
            return ()
        return self.stable.members_of(int(node))

    def membership_in(self, identifier, regions):
        """The strongest membership of the record in any of ``regions`` at SHARED_FLOOR or above, or None."""
        best = None
        for region, weight in self.memberships_of(identifier):
            if region in regions and weight >= vrs_refine.SHARED_FLOOR and (best is None or weight > best[1]):
                best = (region, weight)
        return best

    def member_rows(self, memory, regions):
        """Memory rows whose record is a member (>= SHARED_FLOOR) of one of ``regions`` while labelled elsewhere —
        the rows region scope admits beyond their own label. Empty before any consolidation."""
        stable = self.stable
        if stable is None or getattr(stable, 'member_ptr', None) is None or not regions:
            return np.zeros(0, np.int64)
        labels = self.labels()
        node_episode = self.nodes.node_episode
        row_of = memory._store['row_of']
        rows = []
        for region in regions:
            for node in stable.region_members(region):
                node = int(node)
                if node < len(labels) and int(labels[node]) in regions:
                    continue                              # already in scope by its own label
                identifier = node_episode.get(node)             # Map: node -> record id (records only)
                row = row_of.get(identifier) if identifier is not None else None
                if row is not None:
                    rows.append(int(row))
        return np.unique(np.asarray(rows, np.int64)) if rows else np.zeros(0, np.int64)

    def vrs_of(self, identifier, source=None):
        """Per-record VRS view for receipts: refined strength, kernel promotion (>= 1), the record's evidence
        weight w, its proposition's accumulator decision (SWEGCA), state/stability, pending (not consolidated)."""
        node = self.nodes.episode_node.get(identifier)
        if node is None:
            return None
        stable = self.stable
        pending = stable is None or node >= stable.node_count
        weights = getattr(stable, 'record_weight', None) if stable is not None else None
        weight = None if pending or weights is None else round(float(weights[node]), 4)
        return dict(strength=round(self.strength(identifier), 4), promoted=self.strength(identifier) >= vrs_refine.PROMOTION,
                    weight=weight, state=None if pending else round(float(stable.state[node]), 4),
                    stability=None if pending else round(float(stable.stability[node]), 4), pending=pending,
                    usage=self.usage.get(source) if source else None)   # [injected, opened] from the sessions' ledger

    def consolidate(self, memory, *, seed=vrs_refine.SEED, cycles=vrs_refine.CYCLES):
        """One consolidation chunk: refine strengths region by region from the current stable version.
        Returns the successor generation (same topology and regions; refined strengths, node states)."""
        topology = self if len(self.components) == self.flat.count else self.rebuild_regions()
        version, strength, score = vrs_refine.consolidate(topology, memory, self.stable, seed=seed, cycles=cycles)
        flat = topology.flat.with_arrays(strength=strength, score=score)
        snapshot_id = digest((self.snapshot_id, 'consolidation', version.version_id))
        receipt = dict(self.last_receipt, status='consolidated', stable_version_id=version.version_id,
                       consolidation=version.summary(), parent_snapshot_id=self.snapshot_id)
        # the level-0 adjacency cache weighs edges by strength: rebuilt at the next append
        return Graph(snapshot_id, flat, topology.nodes, topology.components, topology.regions, freeze_view(receipt), None, version, self.usage,
                     self.aliases)

    def summary(self):
        return {k: plain(v) for k, v in self.last_receipt.items() if k != 're_evidence_updates'}


def _mapping_proxy(data):
    return MappingProxyType(data)


# pickle cannot name the mappingproxy type; rebuild frozen views through a module function.
copyreg.pickle(MappingProxyType, lambda m: (_mapping_proxy, (dict(m),)))
UNBRIDGED_FACTOR = 1.0   # order factor for candidates reached only through an unpromoted region pair (1.0 = receipt only)
PROMOTION_GATE = 0.25    # promoted (strength >= 1.0) records rank as if 25% stronger (0 = ordering ignores VRS; recall-bench toggles it)
ASK_GATE = 0.5           # per rare query word found in the record's own 「찾을 때 묻는 말」, up to ASK_GATE_MAX_HITS (measured 2026-09-15: 0 / .25 / .5 / 1.0 -> MRR .871 / .944 / .950 / .956, no regressions with the rarity bar)
ASK_GATE_MAX_HITS = 3
DESCRIPTION_GATE = 1.0   # per rare query word found in a memory doc's front-matter description (14 fresh questions: 0 / .25 / .5 / 1.0 -> MRR .416 / .524 / .605 / .742; the tuned 15 go .956 -> .856)
ASK_GATE_RARE_SHARE = 0.05   # an ask hit counts only for a word carried by at most this share of the store
REGION_SCOPE_FLOOR = 3   # region-scoped recall falls back to the whole store below this many candidates
REGION_SCOPE_AUTO_CANDIDATES = 2000   # 'auto' scope applies the region restriction only above this many whole-store candidates
REGION_SCOPE_INFORMATIVE_ONLY = True   # activate regions from informative cues only (function words activate everything)
REGION_SCOPE_MIN_HITS = 1              # ('cues' activation) a region is active when at least this many informative cues sit in it
REGION_ACTIVATION = 'rows'             # 'rows': regions of the lexically strongest rows; 'mass': regions by summed cue mass; 'cues': by cue-node labels
REGION_SCOPE_TOP_ROWS = 100            # ('rows') how many strongest rows activate their regions (50 lost a fresh answer entirely; 100 lost none — 14 fresh questions, 2026-09-15)
REGION_SCOPE_TOP = 8                   # ('mass') the top regions by mass are active ...
REGION_SCOPE_MASS_SHARE = 0.25         # ... plus every region with at least this share of the top region's mass
PORTAL_SCORE_FLOOR = 0.05              # portal partners join the scope only at or above this promoted share
# G5 (2026-09-19, R4): a region pair keyed by a shared experience is a partner in scope whatever its edge score.
# Measured on the live copy (5.7k, scope forced, 20 prompts): excluded rows median 436 -> 145, top-10 vs the
# whole store unchanged (9.8/10); 45 % of crossings then go through a named shared experience. A/B on four
# scoped prompts: +15-30 ms per judgment (165->186, 139->176, 158->201, 145->170) for the extra rows scored.
# At 60k+ (where 'auto' scope applies to most prompts) this is to be re-measured against the 1 s line.
KEYED_PARTNERS_IN_SCOPE = True
BUNDLE_LIMIT = 60_000     # recommended records per bundle (one store, one process): docs/SIZING.md (2026-09-19)
CHECKPOINT_EVERY = 64     # in-ingest safety bound on crash replay; the daemon checkpoints after a
                          # quiet spell instead (CHECKPOINT_IDLE), close() always checkpoints
CHECKPOINT_IDLE = 5.0     # seconds without requests before a resident main writes its checkpoint
KEEP_LIVE_ROWS = 8        # live journal rows kept by compact()
CHECKPOINT_MAGIC = b'Z1'  # zlib-compressed checkpoint blob; a bare pickle is the older form
ARCHIVE_MAGIC = b'A1'     # zlib-compressed journal segment: JSON lines of observation rows
SEGMENT_ROWS = 64         # archive segment size — partial decompression granularity


class Main:
    """Sole owner for one state directory; transaction commit precedes publication.

    Restart: load the last checkpoint (pickled hot index, flat VRS generation and
    request table), verify it against the journal row it claims, then replay only
    the observations after it. Without a matching checkpoint the whole journal is
    replayed as before. The journal remains the source of truth; a checkpoint is a
    verified cache written every CHECKPOINT_EVERY ingests and on close.
    """
    # One generation tuple, replaced atomically: recall reads it once without the owner's lock
    # while an ingest builds and publishes the successor (2026-09-15).
    _generation = (None, None, None, None)

    def _set_generation(self, memory, graph, pair, operations):
        self._generation = (memory, graph, pair, operations)

    @property
    def memory(self):
        return self._generation[0]

    @memory.setter
    def memory(self, value):
        m, g, p, o = self._generation
        self._generation = (value, g, p, o)

    @property
    def graph(self):
        return self._generation[1]

    @graph.setter
    def graph(self, value):
        m, g, p, o = self._generation
        self._generation = (m, value, p, o)

    @property
    def pair(self):
        return self._generation[2]

    @pair.setter
    def pair(self, value):
        m, g, p, o = self._generation
        self._generation = (m, g, value, o)

    @property
    def operations(self):
        return self._generation[3]

    @operations.setter
    def operations(self, value):
        m, g, p, o = self._generation
        self._generation = (m, g, p, value)

    def __init__(self, state_dir, *, allow_ingest=False, bundle_limit=None, defer_checkpoints=False):
        self.directory = Path(state_dir).expanduser().resolve()
        self.directory.mkdir(parents=True, exist_ok=True)
        self.lock = FileLock(self.directory / 'owner.lock')
        try:
            self.lock.acquire(timeout=0)
        except Timeout:
            raise ValueError('state_directory_already_owned') from None
        self.closed, self.allow_ingest = False, allow_ingest
        self.defer_checkpoints = bool(defer_checkpoints)
        self.bundle_limit = int(bundle_limit) if bundle_limit else BUNDLE_LIMIT
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
            # G7 resident layer (2026-09-19): the index alone, for a warm view in another process (resident.py)
            self.db.execute('CREATE TABLE IF NOT EXISTS checkpoint_warm (id INTEGER PRIMARY KEY CHECK(id=1), seq INTEGER NOT NULL, pair TEXT NOT NULL, blob BLOB NOT NULL)')
            # Lossless hard compression (2026-09-14): journal rows older than the checkpoint
            # move into compressed segments. archive + observations is still the whole journal.
            self.db.execute('CREATE TABLE IF NOT EXISTS journal_archive (segment INTEGER PRIMARY KEY, first_seq INTEGER NOT NULL, last_seq INTEGER NOT NULL, last_pair TEXT NOT NULL, rows INTEGER NOT NULL, blob BLOB NOT NULL)')
            self.db.execute('DROP TABLE IF EXISTS vrs_overlay')      # the interim overlay (pre vrs-regions)
            identity = self.db.execute('SELECT value FROM identity WHERE id=1').fetchone()
            if identity is None:
                identity = (str(uuid.uuid4()),)
                self.db.execute('INSERT INTO identity VALUES (1,?)', identity)
            self.identity = identity[0]
            # HotIndex (ported) stays importable for older checkpoints; the live index is compact.
            memory = CompactIndex.empty(self.identity)
            graph = Graph.empty(self.identity, memory._store['vocab'])
            self._set_generation(memory, graph, FullCurrentMemoryVrsSnapshot(memory, graph.snapshot_id), Map())
            started = perf_counter_ns()
            after = self._load_checkpoint()
            # The journal stays the source of truth: every row the checkpoint claims to
            # cover is re-validated (fingerprint and request identity) before use.
            for _, req, body, fingerprint, _ in self._journal_rows(0, after):
                kind, row = journal_entry(req, body, fingerprint)
            replayed = 0
            pending = []          # observation rows sharing one pair id = one batch generation (2026-09-18)

            def flush():
                if not pending:
                    return
                expected = pending[0][3]
                memory, snapshot, episodes, operations = self.memory, self.graph.snapshot_id, [], self.operations
                for req, row, fingerprint, _ in pending:
                    memory2, identifier = memory.append(row)
                    if memory2 is not memory:
                        episodes.append(memory2.episode(identifier))
                        snapshot = digest((snapshot, fingerprint))
                    memory = memory2
                    operations = operations.set(req, (fingerprint, identifier, expected))
                graph = self.graph.append_many(episodes, snapshot, memory) if episodes else self.graph
                pair = FullCurrentMemoryVrsSnapshot(memory, graph.snapshot_id)
                if pair.snapshot_id != expected:
                    raise ValueError('stored_generation_integrity_failed')
                self._set_generation(memory, graph, pair, operations)
                pending.clear()

            for _, req, body, fingerprint, expected in self._journal_rows(after, None):
                kind, row = journal_entry(req, body, fingerprint)
                if kind in ('alias', 'usage', 'consolidation'):
                    flush()
                    memory, identifier = self.memory, None
                    if kind == 'alias':
                        graph = self.graph.with_aliases(row['canonical'], row['aliases'])
                    elif kind == 'usage':
                        graph = self.graph.with_usage(row['counts'])
                    else:
                        graph = self.graph.consolidate(memory, seed=row['seed'], cycles=row['cycles'])
                        if graph.stable.version_id != row['version_id']:
                            raise ValueError('stored_generation_integrity_failed')
                    pair = FullCurrentMemoryVrsSnapshot(memory, graph.snapshot_id)
                    if pair.snapshot_id != expected:
                        raise ValueError('stored_generation_integrity_failed')
                    self._set_generation(memory, graph, pair, self.operations.set(req, (fingerprint, identifier, pair.snapshot_id)))
                else:
                    if pending and pending[0][3] != expected:
                        flush()
                    pending.append((req, row, fingerprint, expected))
                replayed += 1
            flush()
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
        journal = self._journal_pair(seq)
        if journal != pair:
            self.restore['checkpoint'] = 'stale_ignored'
            return 0
        try:
            if blob[:2] == CHECKPOINT_MAGIC:
                blob = zlib.decompress(blob[2:])
            state = pickle.loads(blob)
            if state['pair'] != pair or state['identity'] != self.identity:
                raise ValueError('checkpoint identity mismatch')
        except Exception as error:
            self.restore['checkpoint'] = 'unreadable_ignored: ' + type(error).__name__
            return 0
        self._set_generation(state['memory'], state['graph'], self.pair, state['operations'])
        if isinstance(self.memory, HotIndex):        # older uncompressed checkpoint: convert once
            self.memory = CompactIndex.from_hot(self.memory)
            self._dirty += 1
            self.restore['converted'] = 'hot_index_to_compact'
        if isinstance(self.graph.nodes, Map):          # older Graph with string node keys
            vocab = self.memory._store['vocab']
            count = len(self.graph.nodes)
            episode_node, node_episode = Map(), Map()
            cue_node = np.full(vocab.count, -1, np.int32)
            node_cue = np.full(count, -1, np.int32)
            for name, node in self.graph.nodes.items():
                if name.startswith('cue:'):
                    cue_id = vocab.id_of(name[4:])
                    if cue_id is None:
                        raise ValueError('checkpoint_conversion_unknown_cue')
                    cue_node[cue_id] = node
                    node_cue[node] = cue_id
                else:
                    episode_node = episode_node.set(name, node)
                    node_episode = node_episode.set(node, name)
            self.graph.nodes = NodeDirectory(vocab, episode_node, node_episode, frozen(cue_node, np.int32),
                                             frozen(node_cue, np.int32), count)
            self._dirty += 1
            self.restore['converted_graph'] = 'string_nodes_to_directory'
        if getattr(self.graph, '_names', None) is not None:
            self.graph._names = None                  # old layout kept every node name resident
        if any(not isinstance(pos, np.ndarray) for _, pos in self.graph.regions.values()):
            self.graph = self.graph.rebuild_regions()  # old layout: duplicated edge arrays, name tuples, position Maps
            self._dirty += 1
            self.restore['converted_regions'] = 'rebuilt_shared_layout'
        self._set_generation(self.memory, self.graph, FullCurrentMemoryVrsSnapshot(self.memory, self.graph.snapshot_id), self.operations)
        if self.pair.snapshot_id != pair:
            raise ValueError('checkpoint_generation_integrity_failed')
        self.restore['checkpoint'] = f'loaded seq {seq}'
        return seq

    def rebuild_from_journal(self, progress=None, drop_consolidations=False):
        """Rebuild index and graph from the whole journal under the current rules (e.g. after
        changing the node set). Row contents and fingerprints are untouched; the per-row pair ids
        are re-derived and rewritten, because a VRS snapshot id digests the settled values and so
        certifies the arithmetic that produced them — a new node rule is a new certificate chain.
        Returns (rows, seconds)."""
        self._check()
        started = perf_counter_ns()
        memory = CompactIndex.empty(self.identity)
        graph = Graph.empty(self.identity, memory._store['vocab'])
        operations = Map()
        pair = FullCurrentMemoryVrsSnapshot(memory, graph.snapshot_id)
        pairs = {}
        rows = 0
        bodies = {}
        dropped = []
        pending = []

        def flush():
            nonlocal memory, graph, operations, pair, rows
            if not pending:
                return
            graph_snapshot, episodes, recorded = graph.snapshot_id, [], []
            for seq, req, row, fingerprint, _ in pending:
                memory2, identifier = memory.append(row)
                if memory2 is not memory:
                    episodes.append(memory2.episode(identifier))
                    graph_snapshot = digest((graph_snapshot, fingerprint))
                memory = memory2
                recorded.append((seq, req, fingerprint, identifier))
            if episodes:
                graph = graph.append_many(episodes, graph_snapshot, memory)
            pair = FullCurrentMemoryVrsSnapshot(memory, graph.snapshot_id)
            for seq, req, fingerprint, identifier in recorded:
                pairs[seq] = pair.snapshot_id
                operations = operations.set(req, (fingerprint, identifier, pair.snapshot_id))
            rows += len(pending)
            if progress and rows % 50 == 0:
                progress(rows, pending[-1][0], graph)
            pending.clear()

        for seq, req, body, fingerprint, expected in self._journal_rows(0, None):
            kind, row = journal_entry(req, body, fingerprint)
            if kind not in ('alias', 'usage', 'consolidation'):
                if pending and pending[0][4] != expected:
                    flush()
                pending.append((seq, req, row, fingerprint, expected))
                continue
            flush()
            if kind == 'consolidation' and drop_consolidations:
                # a consolidation row certifies a derived computation under the rules of its day; when the
                # rules change (vrs-regions -> SWEGCA evidence, 2026-09-15) the rows are dropped and the
                # daemon consolidates afresh — observations, the source of truth, are untouched
                dropped.append(seq)
                continue
            if kind == 'alias':
                graph = graph.with_aliases(row['canonical'], row['aliases'])
                identifier = None
            elif kind == 'usage':
                graph = graph.with_usage(row['counts'])
                identifier = None
            elif kind == 'consolidation':
                # a consolidation is re-derived under the current rules: its version id is a certificate
                # like the pair id, so the row body (and fingerprint) are rewritten with the new one
                graph = graph.consolidate(memory, seed=row['seed'], cycles=row['cycles'])
                version = graph.stable
                row = dict(row, edge_count=version.edge_count, node_count=version.node_count,
                           labels_digest=version.labels_digest, version_id=version.version_id, parent_id=version.parent_id)
                fingerprint = digest(row)
                bodies[seq] = (canonical(row), fingerprint)
                identifier = None
            pair = FullCurrentMemoryVrsSnapshot(memory, graph.snapshot_id)
            pairs[seq] = pair.snapshot_id
            operations = operations.set(req, (fingerprint, identifier, pair.snapshot_id))
            rows += 1
            if progress and rows % 50 == 0:
                progress(rows, seq, graph)
        flush()
        self.db.execute('BEGIN IMMEDIATE')
        try:
            for seq, value in pairs.items():
                self.db.execute('UPDATE observations SET pair=? WHERE seq=?', (value, seq))
            for seq, (body, fingerprint) in bodies.items():
                self.db.execute('UPDATE observations SET body=?, fingerprint=? WHERE seq=?', (body, fingerprint, seq))
            for seq in dropped:
                self.db.execute('DELETE FROM observations WHERE seq=?', (seq,))
            for segment, blob in self.db.execute('SELECT segment, blob FROM journal_archive').fetchall():
                if blob[:2] != ARCHIVE_MAGIC:
                    raise ValueError('journal_archive_segment_corrupt')
                lines = []
                for line in zlib.decompress(blob[2:]).decode('utf-8').splitlines():
                    seq, req, body, fingerprint, _ = json.loads(line)
                    if seq in dropped:
                        continue
                    if seq in bodies:
                        body, fingerprint = bodies[seq]
                    lines.append(json.dumps([seq, req, body, fingerprint, pairs[seq]], ensure_ascii=False, separators=(',', ':')))
                if not lines:
                    self.db.execute('DELETE FROM journal_archive WHERE segment=?', (segment,))
                    continue
                last_seq = json.loads(lines[-1])[0]
                self.db.execute('UPDATE journal_archive SET blob=?, last_pair=? WHERE segment=?',
                                (ARCHIVE_MAGIC + zlib.compress('\n'.join(lines).encode('utf-8'), 6), pairs[last_seq], segment))
            self.db.execute('DELETE FROM checkpoint')          # the old checkpoint certifies the old chain
            self.db.execute('COMMIT')
        except BaseException:
            if self.db.in_transaction:
                self.db.execute('ROLLBACK')
            raise
        self.owner.replace(self.pair.snapshot_id, pair)
        self._set_generation(memory, graph, pair, operations)
        self._dirty += 1
        self.checkpoint()
        return rows, (perf_counter_ns() - started) / 1e9

    @property
    def dirty(self):
        """Ingests since the last checkpoint (a resident main flushes these when idle)."""
        return self._dirty

    def checkpoint_if_dirty(self):
        """Checkpoint when there is anything to save; returns the receipt or None."""
        return self.checkpoint() if self._dirty else None

    def checkpoint_prepare(self):
        """Grab the current generation for a checkpoint (call under the owner's lock; cheap)."""
        self._check()
        head = self._journal_head()
        if head is None:
            return None
        seq, pair = head
        if pair != self.pair.snapshot_id:
            raise ValueError('checkpoint_does_not_match_journal_head')
        return dict(seq=seq, pair=pair, identity=self.identity, memory=self.memory, graph=self.graph,
                    operations=self.operations, started=perf_counter_ns())

    @staticmethod
    def checkpoint_serialize(prepared):
        """Pickle + compress a prepared generation. Needs no lock: generations are immutable.
        protocol 4 on purpose: protocol 5 unpickles every array as a view on the one big blob,
        so any live array keeps the whole blob resident (measured: 2x the data)."""
        raw = pickle.dumps(dict(identity=prepared['identity'], pair=prepared['pair'], memory=prepared['memory'],
                                graph=prepared['graph'], operations=prepared['operations']), protocol=4)
        # G7 (2026-09-19): the warm blob — the memory index alone — beside the full one; a resident in another
        # process loads this to hold the bundle warm (measured on the live store: see docs/VRS_REGIONS.md)
        warm = CHECKPOINT_MAGIC + zlib.compress(pickle.dumps(prepared['memory'], protocol=4), 6)
        return len(raw), CHECKPOINT_MAGIC + zlib.compress(raw, 6), warm

    def checkpoint_commit(self, prepared, serialized):
        """Write a serialized generation if it is still the current one (under the lock)."""
        if prepared['pair'] != self.pair.snapshot_id:
            return None                                # a newer generation exists; its own checkpoint will follow
        return self._checkpoint_write(prepared['seq'], prepared['pair'], serialized, prepared['started'])

    def checkpoint(self):
        """Write the current verified state as a checkpoint for the latest journal row."""
        prepared = self.checkpoint_prepare()
        if prepared is None:
            return None
        return self._checkpoint_write(prepared['seq'], prepared['pair'], self.checkpoint_serialize(prepared), prepared['started'])

    def _checkpoint_write(self, seq, pair, serialized, started):
        raw_len, blob, warm = (*serialized, None)[:3]
        self.db.execute('BEGIN IMMEDIATE')
        try:
            self.db.execute('INSERT OR REPLACE INTO checkpoint(id, seq, pair, blob) VALUES (1,?,?,?)', (seq, pair, blob))
            if warm is not None:
                self.db.execute('INSERT OR REPLACE INTO checkpoint_warm(id, seq, pair, blob) VALUES (1,?,?,?)', (seq, pair, warm))
            self.db.execute('COMMIT')
        except BaseException:
            if self.db.in_transaction:
                self.db.execute('ROLLBACK')
            raise
        self.db.execute('PRAGMA wal_checkpoint(TRUNCATE)')
        self._dirty = 0
        return dict(seq=seq, bytes=len(blob), raw_bytes=raw_len, elapsed_ns=perf_counter_ns() - started)

    # ── journal access across archive segments and live rows ────────────
    def _journal_rows(self, after, upto):
        """Yield (seq, request_id, body, fingerprint, pair) for after < seq <= upto (None = all)."""
        for first, last, blob in self.db.execute(
                'SELECT first_seq, last_seq, blob FROM journal_archive WHERE last_seq>? ORDER BY segment', (after,)):
            if upto is not None and first > upto:
                break
            if blob[:2] != ARCHIVE_MAGIC:
                raise ValueError('journal_archive_segment_corrupt')
            for line in zlib.decompress(blob[2:]).decode('utf-8').splitlines():
                seq, req, body, fingerprint, pair = json.loads(line)
                if seq <= after:
                    continue
                if upto is not None and seq > upto:
                    return
                yield seq, req, body, fingerprint, pair
        query = 'SELECT seq,request_id,body,fingerprint,pair FROM observations WHERE seq>?'
        params = [after]
        if upto is not None:
            query += ' AND seq<=?'
            params.append(upto)
        yield from self.db.execute(query + ' ORDER BY seq', params)

    def _journal_pair(self, seq):
        row = self.db.execute('SELECT pair FROM observations WHERE seq=?', (seq,)).fetchone()
        if row is not None:
            return row[0]
        for found, *_ , pair in self._journal_rows(seq - 1, seq):
            if found == seq:
                return pair
        return None

    def _journal_head(self):
        row = self.db.execute('SELECT seq, pair FROM observations ORDER BY seq DESC LIMIT 1').fetchone()
        if row is not None:
            return tuple(row)
        row = self.db.execute('SELECT last_seq, last_pair FROM journal_archive ORDER BY segment DESC LIMIT 1').fetchone()
        return tuple(row) if row is not None else None

    def export_observations(self, after_sequence=0, *, max_records=512,
                            max_bytes=768 * 1024):
        """Return exact original observation envelopes from the VRS journal.

        Session assimilation consumes this main-owned export instead of a
        second transcript/outbox database. Consolidation, usage and alias rows
        remain in the session VRS; only external observations enter another
        main. ``next_sequence`` advances over every journal kind, so retries are
        idempotent and a caller never needs to inspect SQLite directly.
        """
        self._check()
        after_sequence = max(0, int(after_sequence))
        max_records = max(1, min(int(max_records), 4096))
        max_bytes = max(4096, min(int(max_bytes), 896 * 1024))
        head = self._journal_head()
        head_sequence = 0 if head is None else int(head[0])
        rows, used, next_sequence = [], 64, after_sequence
        for seq, request_id, body, fingerprint, _ in self._journal_rows(after_sequence, None):
            kind, value = journal_entry(request_id, body, fingerprint)
            encoded = canonical(value).encode('utf-8') if kind == 'observation' else b''
            if kind == 'observation' and rows and (len(rows) >= max_records
                                                    or used + len(encoded) + 64 > max_bytes):
                break
            next_sequence = int(seq)
            if kind != 'observation':
                continue
            rows.append(dict(sequence=int(seq), episode_id='memory:' + digest(
                {key: item for key, item in value.items() if key != 'request_id'}),
                observation=value))
            used += len(encoded) + 64
            if len(rows) >= max_records:
                break
        return dict(status='experience_export', rows=rows,
            after_sequence=after_sequence, next_sequence=next_sequence,
            head_sequence=head_sequence, complete=next_sequence >= head_sequence,
            grants_authority=False)

    def compact(self, *, keep_live=KEEP_LIVE_ROWS, vacuum=True):
        """Lossless hard compression: checkpoint, archive covered journal rows, vacuum.

        Rows at or below the checkpoint seq (minus ``keep_live`` recent rows) are
        written as one zlib segment of JSON lines and deleted from ``observations``.
        ``archive + observations`` remains the complete journal: restart re-validates
        every archived row's fingerprint and a stale checkpoint replays through the
        archive, so nothing is lost and the same pair ids are reproduced.
        """
        self._check()
        started = perf_counter_ns()
        checkpoint = self.checkpoint() if self._dirty or self._journal_head() else None
        row = self.db.execute('SELECT seq FROM checkpoint WHERE id=1').fetchone()
        archived = 0
        if row is not None:
            cutoff = int(row[0]) - int(keep_live)
            rows = self.db.execute('SELECT seq,request_id,body,fingerprint,pair FROM observations '
                                   'WHERE seq<=? ORDER BY seq', (cutoff,)).fetchall()
            if rows:
                self.db.execute('BEGIN IMMEDIATE')
                try:
                    # segments of SEGMENT_ROWS rows: reading one row decompresses one small block
                    for at in range(0, len(rows), SEGMENT_ROWS):
                        part = rows[at:at + SEGMENT_ROWS]
                        lines = '\n'.join(json.dumps(list(r), ensure_ascii=False, separators=(',', ':')) for r in part)
                        blob = ARCHIVE_MAGIC + zlib.compress(lines.encode('utf-8'), 6)
                        self.db.execute('INSERT INTO journal_archive(first_seq,last_seq,last_pair,rows,blob) VALUES (?,?,?,?,?)',
                                        (part[0][0], part[-1][0], part[-1][4], len(part), blob))
                    self.db.execute('DELETE FROM observations WHERE seq<=?', (cutoff,))
                    self.db.execute('COMMIT')
                except BaseException:
                    if self.db.in_transaction:
                        self.db.execute('ROLLBACK')
                    raise
                archived = len(rows)
        if vacuum:
            self.db.execute('VACUUM')
        self.db.execute('PRAGMA wal_checkpoint(TRUNCATE)')
        size = sum(f.stat().st_size for f in self.directory.glob('memory.sqlite3*'))
        return dict(status='compacted', archived_rows=archived, checkpoint=checkpoint,
                    segments=self.db.execute('SELECT COUNT(*) FROM journal_archive').fetchone()[0],
                    live_rows=self.db.execute('SELECT COUNT(*) FROM observations').fetchone()[0],
                    disk_bytes=size, elapsed_ns=perf_counter_ns() - started)

    def _check(self):
        if self.closed:
            raise ValueError('main_closed')

    # ── VRS consolidation (vrs-regions) ──────────────────────────────────
    def consolidation_stale(self):
        """True when the graph has edges the stable version has not refined, or it has not converged."""
        graph = self.graph
        stable = graph.stable
        # one refinement per generation: stale when the graph grew since the last consolidation (every
        # append grows it, and a supersede or new verdict arrives as an append), never because strengths
        # could still move — a connection's strength is how many generations it stayed stable
        if len(graph.flat.src) == 0:
            return False
        return (stable is None or stable.edge_count != len(graph.flat.src) or stable.node_count != graph.flat.count
                or (getattr(stable, 'usage_digest', '') or '') != graph.usage_digest    # usage changed since last refinement
                or (getattr(stable, 'alias_digest', '') or '') != graph.alias_digest)   # registry changed

    def consolidate_prepare(self):
        """Freeze the current generation (cheap; under the owner's lock)."""
        self._check()
        memory, graph, pair, _ = self._generation
        return SimpleNamespace(memory=memory, graph=graph, pair=pair)

    @staticmethod
    def consolidate_run(prepared, *, seed=vrs_refine.SEED, cycles=vrs_refine.CYCLES):
        """Refine outside the lock: generations are immutable, so this needs no lock at all."""
        return prepared.graph.consolidate(prepared.memory, seed=seed, cycles=cycles)

    def consolidate_commit(self, prepared, graph):
        """Publish a consolidated generation as a journal row (under the lock). If an ingest landed since
        ``prepare``, the result is discarded (None) and the next idle pass starts over."""
        self._check()
        if not self.allow_ingest:
            raise ValueError('observation_ingress_disabled')
        if self.graph is not prepared.graph:
            return None
        version = graph.stable
        body = dict(kind='consolidation', seed=version.seed, cycles=version.cycles, edge_count=version.edge_count,
                    node_count=version.node_count, labels_digest=version.labels_digest, version_id=version.version_id,
                    parent_id=version.parent_id)
        fingerprint = digest(body)
        request_id = 'consolidation:' + version.version_id[:40]
        pair = FullCurrentMemoryVrsSnapshot(prepared.memory, graph.snapshot_id)
        operations = self.operations.set(request_id, (fingerprint, None, pair.snapshot_id))
        self.db.execute('BEGIN IMMEDIATE')
        try:
            self.db.execute('INSERT INTO observations(request_id,body,fingerprint,pair) VALUES (?,?,?,?)',
                            (request_id, canonical(body), fingerprint, pair.snapshot_id))
            self.db.execute('COMMIT')
        except BaseException:
            if self.db.in_transaction:
                self.db.execute('ROLLBACK')
            raise
        self.owner.replace(self.pair.snapshot_id, pair)
        self._set_generation(prepared.memory, graph, pair, operations)
        self._dirty += 1
        return dict(version.summary(), pair_snapshot_id=pair.snapshot_id, stale=self.consolidation_stale())

    def usage_update(self, counts):
        """Journal usage counts {source: [injected, opened]} (under the lock) and chain the graph through them.
        Usage is association evidence for the next consolidation, never a promotion (see vrs_refine.USAGE_CAP)."""
        self._check()
        if not self.allow_ingest:
            raise ValueError('observation_ingress_disabled')
        counts = {str(k): [int(v[0]), int(v[1])] for k, v in dict(counts).items()}
        # only sources the store knows (live records), and only when a count actually moves
        live = {self.memory.episode(e).source_addresses[0] for e in self.memory.iter_episode_ids()
                if e not in self.memory.superseded}
        known = {source: pair for source, pair in counts.items()
                 if source in live and self.graph.usage.get(source) != pair}
        if not known:
            return dict(status='unchanged', sources=0)
        body = dict(kind='usage', counts=dict(sorted(known.items())))
        fingerprint = digest(body)
        request_id = 'usage:' + fingerprint[:40]
        memory, graph = self.memory, self.graph.with_usage(known)
        pair = FullCurrentMemoryVrsSnapshot(memory, graph.snapshot_id)
        operations = self.operations.set(request_id, (fingerprint, None, pair.snapshot_id))
        self.db.execute('BEGIN IMMEDIATE')
        try:
            self.db.execute('INSERT INTO observations(request_id,body,fingerprint,pair) VALUES (?,?,?,?)',
                            (request_id, canonical(body), fingerprint, pair.snapshot_id))
            self.db.execute('COMMIT')
        except BaseException:
            if self.db.in_transaction:
                self.db.execute('ROLLBACK')
            raise
        self.owner.replace(self.pair.snapshot_id, pair)
        self._set_generation(memory, graph, pair, operations)
        self._dirty += 1
        return dict(status='usage_recorded', sources=len(known), usage_sources=len(graph.usage),
                    pair_snapshot_id=pair.snapshot_id, stale=self.consolidation_stale())

    def alias_update(self, canonical, aliases):
        _canonical_json = globals()['canonical']
        """Journal a hypothesis-registry binding: the alias propositions' evidence folds into the canonical one.
        Bindings are declarations (journaled, replayed, rebuilt), not evidence — they change which observations
        share a hypothesis, and the accumulator decides as before."""
        self._check()
        if not self.allow_ingest:
            raise ValueError('observation_ingress_disabled')
        canonical = str(canonical).strip()
        aliases = sorted({str(a).strip() for a in aliases if str(a).strip() and str(a).strip() != canonical})
        if not canonical or not aliases:
            raise ValueError('alias_binding_empty')
        known = set(self.memory.propositions)
        unknown = [a for a in [canonical, *aliases] if a not in known and a not in self.graph.aliases]
        if unknown:
            raise ValueError('unknown_proposition: ' + '; '.join(unknown)[:300])
        if all(self.graph.aliases.get(a) == self.graph.aliases.get(canonical, canonical) for a in aliases):
            return dict(status='unchanged')
        body = dict(kind='alias', canonical=canonical, aliases=aliases)
        fingerprint = digest(body)
        request_id = 'alias:' + fingerprint[:40]
        memory, graph = self.memory, self.graph.with_aliases(canonical, aliases)
        pair = FullCurrentMemoryVrsSnapshot(memory, graph.snapshot_id)
        operations = self.operations.set(request_id, (fingerprint, None, pair.snapshot_id))
        self.db.execute('BEGIN IMMEDIATE')
        try:
            self.db.execute('INSERT INTO observations(request_id,body,fingerprint,pair) VALUES (?,?,?,?)',
                            (request_id, _canonical_json(body), fingerprint, pair.snapshot_id))
            self.db.execute('COMMIT')
        except BaseException:
            if self.db.in_transaction:
                self.db.execute('ROLLBACK')
            raise
        self.owner.replace(self.pair.snapshot_id, pair)
        self._set_generation(memory, graph, pair, operations)
        self._dirty += 1
        return dict(status='alias_recorded', canonical=canonical, aliases=aliases, registry=len(graph.aliases),
                    pair_snapshot_id=pair.snapshot_id, stale=self.consolidation_stale())

    def consolidate(self, **options):
        """Prepare + run + commit (tests, manual command). Returns the version summary or None if raced."""
        prepared = self.consolidate_prepare()
        return self.consolidate_commit(prepared, self.consolidate_run(prepared, **options))

    def consolidate_until_converged(self, limit=64, **options):
        """Run consolidation chunks until the stable version converges (tests, migration)."""
        last = None
        for _ in range(limit):
            if not self.consolidation_stale():
                break
            last = self.consolidate(**options)
        return last

    def status(self):
        self._check()
        graph = self.graph
        return dict(status='ready', identity=self.identity, pair_snapshot_id=self.pair.snapshot_id,
            vrs_stable=None if graph.stable is None else graph.stable.summary(),
            vrs_pending_edges=len(graph.flat.src) - (graph.stable.edge_count if graph.stable is not None else 0),
            consolidation_stale=self.consolidation_stale(),
            memory_snapshot_id=self.memory.snapshot_id, vrs_snapshot_id=self.graph.snapshot_id,
            hot_episode_count=self.memory.episode_count, outcome_counts=dict(self.memory.outcome_counts),
            lookup_requires_io=False, internal_llm_calls=0, final_utterance_calls=0,
            backend='standalone_native_vrs2', native_engine_bundled=True, scheduled_dialogue_available=True,
            writes_enabled=self.allow_ingest, authority=dict(AUTHORITY), numerical_version=VERSION,
            vrs_node_count=len(self.graph.nodes), vrs_edge_count=self.graph.edge_count,
            last_vrs_event=self.graph.summary(), restore=dict(self.restore),
            checkpoint_pending_ingests=self._dirty, bundle=self.bundle())

    def bundle(self):
        """Fill of this bundle against the recommended size (soft: nothing is refused; see docs/SIZING.md)."""
        count = self.memory.episode_count
        return dict(records=count, limit=self.bundle_limit, fill=round(count / self.bundle_limit, 3),
                    over=count > self.bundle_limit)

    def ingest(self, arguments):
        """One observation = one generation (unchanged chain); see ``ingest_many``."""
        return self.ingest_many([arguments])['results'][0]

    def ingest_many(self, batch):
        """K observations enter as one generation (batch generations, 2026-09-18).

        All-or-nothing: rows are validated first, memory rows appended in order, the graph extended once
        (``Graph.append_many``), and the K journal rows written in one transaction with the *same* pair id —
        that shared id is what tells replay to regroup them. Rows already journaled with the same content
        are idempotent replays (skipped, reported); a reused request id with other content rejects the whole
        batch. ``results`` holds one per-row receipt in input order, shaped like the old ``ingest`` result.
        """
        self._check()
        if not self.allow_ingest:
            raise ValueError('observation_ingress_disabled')
        began = perf_counter_ns()
        if type(batch) is not list or not batch:
            raise ValueError('ingest_many_needs_a_nonempty_list')
        rows, seen = [], {}
        for arguments in batch:
            row = observation(arguments)
            fingerprint = digest(row)
            request_id = row['request_id']
            existing = self.operations.get(request_id) or seen.get(request_id)
            if existing is not None:
                if existing[0] != fingerprint:
                    raise ValueError('request_id_reused_with_different_content')
                rows.append((row, fingerprint, existing))       # idempotent replay: not journaled again
                continue
            seen[request_id] = (fingerprint, None, None)
            rows.append((row, fingerprint, None))
        memory, graph_snapshot, episodes, fresh = self.memory, self.graph.snapshot_id, [], []
        for row, fingerprint, existing in rows:
            if existing is not None:
                continue
            memory2, identifier = memory.append(row)
            if memory2 is not memory:
                episodes.append(memory2.episode(identifier))
                graph_snapshot = digest((graph_snapshot, fingerprint))
            memory = memory2
            fresh.append((row, fingerprint, identifier))
        graph = self.graph.append_many(episodes, graph_snapshot, memory) if episodes else self.graph
        pair = FullCurrentMemoryVrsSnapshot(memory, graph.snapshot_id)
        operations = self.operations
        for row, fingerprint, identifier in fresh:
            operations = operations.set(row['request_id'], (fingerprint, identifier, pair.snapshot_id))
        if fresh:
            self.db.execute('BEGIN IMMEDIATE')
            try:
                self.db.executemany('INSERT INTO observations(request_id,body,fingerprint,pair) VALUES (?,?,?,?)',
                    [(row['request_id'], canonical(row), fingerprint, pair.snapshot_id) for row, fingerprint, _ in fresh])
                self.db.execute('COMMIT')
            except BaseException:
                if self.db.in_transaction:
                    self.db.execute('ROLLBACK')
                if memory is not self.memory:
                    self.memory.truncate_to(self.memory.count)   # the successor never became durable
                raise
            self.owner.replace(self.pair.snapshot_id, pair)
            self._set_generation(memory, graph, pair, operations)
            self._dirty += len(fresh)
            if self._dirty >= CHECKPOINT_EVERY and not self.defer_checkpoints:
                self.checkpoint()
        added_ids = {e.episode_id for e in episodes}
        summary = graph.summary() if episodes else None
        results, cursor = [], 0
        for row, fingerprint, existing in rows:
            if existing is not None:
                results.append(dict(status='observation_recorded', episode_id=existing[1], pair_snapshot_id=existing[2],
                    current_pair_snapshot_id=pair.snapshot_id, idempotent_replay=True, grants_authority=False))
                continue
            _, _, identifier = fresh[cursor]; cursor += 1
            added = identifier in added_ids
            results.append(dict(status='observation_recorded', episode_id=identifier, pair_snapshot_id=pair.snapshot_id,
                revision=row['revision'], source=row['source'], outcome=row['outcome'],
                idempotent_replay=False, observation_only=True, grants_authority=False,
                current_truth_claimed=False, distinct_source_episode_added=int(added),
                vrs_event=summary if added else {'status': 'unchanged_duplicate_observation'},
                elapsed_ns=perf_counter_ns() - began))
        return dict(status='observations_recorded', count=len(rows), journaled=len(fresh), added=len(episodes),
                    pair_snapshot_id=pair.snapshot_id, results=results, bundle=self.bundle(),
                    elapsed_ns=perf_counter_ns() - began)

    def recall(self, query, expected_snapshot, exclude_kinds=(), region_scope='all'):
        """``expected_snapshot`` None = whatever generation is current (lock-free hook path).

        ``region_scope``: 'all' generates candidates from the whole store; 'regions' (G6) generates them
        only from the regions the matched cues activate plus the regions reachable through a candidate
        portal, falling back to the whole store when fewer than ``REGION_SCOPE_FLOOR`` candidates
        remain. Either way every record stays addressable and the receipt says what the scope excluded.
        """
        self._check()
        memory, graph, pair, _ = self._generation       # one atomic read (lock-free readers)
        if expected_snapshot is not None and expected_snapshot != pair.snapshot_id:
            raise ValueError('snapshot_mismatch')
        query = text_field(query, 'query', 4096)
        if exclude_kinds:
            memory = memory.masked(exclude_kinds)      # same generation, postings filtered by record kind
        full_memory = memory
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
        # recall columns (2026-09-18): a list read per posting row, not an episode build per row
        propositions = {memory.proposition_of_row(int(r)) for cue in informative for r in memory.rows_for_cue(cue)} - {None}
        cues = (*selected, *('proposition:' + p for p in sorted(propositions)))
        # Candidate order: BM25 over the matched informative cues (tf is 1 in a cue set;
        # idf from postings fanout; length normalization from the record's cue count),
        # then the engine's Jaccard. The engine states candidate order is not semantic
        # acceptance and the whole set stays addressable. Plain rarity sums let long
        # records win by matching many middling words of the instruction; Jaccard alone
        # ranks short records above rich ones (v0.2 verdict obs:cue-overlap-penalizes-rich-documents).
        idf = {c: math.log(1.0 + (total - fanout[c] + 0.5) / (fanout[c] + 0.5)) for c in informative}
        cue_counts = memory._store['cues'] if hasattr(memory, '_store') else None
        rows_of = memory._store['row_of'] if cue_counts is not None else None
        # The index generation carries its exact cue total. Graph deliberately
        # excludes redundant Hangul fragment endpoints, so its edge count cannot
        # stand in for this value. Keeping the scalar on the immutable generation
        # makes the BM25 length normalization exact and O(1), including pinned views.
        average = (memory.cue_total / max(1, memory.episode_count)) if cue_counts else 1.0
        k1, b = 1.2, 0.3   # b measured over 15 known-answer queries: .75 MRR .63, .5 .69, .3 .69 (top3 12/15), .15 .65, 0 .38
        opponents = {}
        for p in propositions:
            fetch = getattr(memory, 'episode_light', memory.episode)     # polarity only: no cue strings needed
            active = [fetch(i) for i in memory.propositions[p] if i not in memory.superseded]
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
        # G6 region scope (vrs-regions): the matched cues' regions plus their candidate-portal partners
        # bound candidate generation; the excluded rows are counted for the receipt; too few -> whole store
        scope = dict(requested=region_scope, applied='all', allowed_regions=[], excluded_rows=0, fallback=None)
        if region_scope == 'auto':
            # scope only when the whole-store candidate set is large enough for the restriction to pay:
            # below the threshold the receipts and the ranks stay those of the whole store (measured 2026-09-15:
            # at 5k records the safe scope removes 4% of candidates and a strict one loses known answers)
            matched_all = set()
            for c in selected:
                matched_all.update(full_memory.episode_ids_for_cue(c))
            region_scope = 'regions' if len(matched_all) >= REGION_SCOPE_AUTO_CANDIDATES else 'all'
            scope.update(requested='auto', auto_candidates=len(matched_all), auto_threshold=REGION_SCOPE_AUTO_CANDIDATES)
        if region_scope == 'regions' and graph.stable is not None and len(graph.labels()):
            vocab0 = memory._store['vocab'] if hasattr(memory, '_store') else None
            row_labels = graph.row_labels(full_memory)
            if REGION_ACTIVATION == 'rows':
                # déjà vu as cheap lexical familiarity per row (sum of idf over the informative cues a row
                # carries), then the regions of the strongest rows are active: the engine's recall then
                # runs only there (+ portal partners). The strongest rows are in scope by construction, so
                # the top ranks are those of the whole store; only the long tail is cut (measured 2026-09-15).
                row_of = full_memory._store['row_of']
                acc = {}
                for c in informative:
                    for i in full_memory.episode_ids_for_cue(c):
                        r_ = row_of[i]
                        acc[r_] = acc.get(r_, 0.0) + idf[c]
                ranked_rows = sorted(acc.items(), key=lambda kv: -kv[1])[:REGION_SCOPE_TOP_ROWS]
                active0 = {int(row_labels[r_]) for r_, v in ranked_rows if row_labels[r_] >= 0}
                scope['top_rows'] = len(ranked_rows)
            elif REGION_ACTIVATION == 'mass':
                # activation by cue-hit mass: a region is active in proportion to the idf-weighted matched cues
                # its own records carry (a record's region, not its cues' regions — a record links to cues in
                # many regions, so cue-node labels miss the region the answer sits in; measured 2026-09-15)
                mass = {}
                for c in informative:
                    rows_c = full_memory.episode_ids_for_cue(c)
                    if not rows_c:
                        continue
                    labels_c = row_labels[[full_memory._store['row_of'][i] for i in rows_c]]
                    labels_c = labels_c[labels_c >= 0]
                    if len(labels_c):
                        u_, n_ = np.unique(labels_c, return_counts=True)
                        for r0, k in zip(u_.tolist(), n_.tolist()):
                            mass[r0] = mass.get(r0, 0.0) + idf[c] * k
                ranked = sorted(mass.items(), key=lambda kv: -kv[1])
                top = ranked[:REGION_SCOPE_TOP]
                floor_mass = (ranked[0][1] * REGION_SCOPE_MASS_SHARE) if ranked else 0.0
                active0 = {r0 for r0, v in top} | {r0 for r0, v in ranked if v >= floor_mass}
                scope['region_mass'] = [(r0, round(v, 2)) for r0, v in ranked[:12]]
            else:
                hits = {}
                for c in (informative if REGION_SCOPE_INFORMATIVE_ONLY else selected):
                    cue_id = vocab0.id_of(c) if vocab0 is not None else None
                    node = graph.nodes.cue(cue_id) if cue_id is not None else -1
                    if node >= 0 and graph.labels()[node] >= 0:
                        r0 = int(graph.labels()[node]); hits[r0] = hits.get(r0, 0) + 1
                active0 = {r0 for r0, n in hits.items() if n >= REGION_SCOPE_MIN_HITS} or set(hits)
            allowed = set(active0)
            keyed_partners = 0
            for (a, b_), portal in (getattr(graph.stable, 'portals', None) or {}).items():
                if a not in active0 and b_ not in active0:
                    continue
                # G6: a pair with a promoted connector edge; G5/R4: a pair keyed by a shared experience — the
                # experience that belongs to both regions is the key, whatever its edges' promotion state
                keyed = bool(portal.get('keys')) and KEYED_PARTNERS_IN_SCOPE
                if (portal['status'] == 'candidate' and portal['score'] >= PORTAL_SCORE_FLOOR) or keyed:
                    allowed.update((a, b_))
                    keyed_partners += keyed
            scope['keyed_partners'] = keyed_partners
            if allowed:
                mask = np.isin(row_labels, list(allowed)) | (row_labels < 0)     # pending rows stay visible
                # G5 (R3): a record belongs to every region it is a member of — the members of the active
                # regions are in scope whatever their own label (measured live: +1.25 regions per record at .2)
                member_rows = graph.member_rows(full_memory, set(active0))
                if len(member_rows):
                    mask[member_rows] = True
                scope['member_rows'] = int(len(member_rows))
                scoped = full_memory.masked_rows(mask)
                matched_rows = set()
                for c in selected:
                    matched_rows.update(full_memory.episode_ids_for_cue(c))
                kept = set()
                for c in selected:
                    kept.update(scoped.episode_ids_for_cue(c))
                excluded = len(matched_rows - kept)
                if len(kept) >= REGION_SCOPE_FLOOR:
                    memory = scoped
                    scope.update(applied='regions', allowed_regions=sorted(allowed), excluded_rows=excluded)
                else:
                    scope.update(fallback='fewer_than_floor', allowed_regions=sorted(allowed), excluded_rows=0,
                                 would_exclude=excluded)
        signal = detect_deja_vu(memory, query=query, current_cues=cues)
        # recall columns (2026-09-18): the engine's recall_memory on cue ids — same RecallResult, no episode builds
        recalled = memory.recall_candidates(signal) if hasattr(memory, 'recall_candidates') else recall_memory(memory, signal)
        # G6 (vrs-regions): region preactivation after déjà vu — the matched cues' regions are active;
        # a candidate is 'local' when its record sits in an active region, 'portal' when it is reached
        # through a region pair with a promoted bridge (a candidate portal of the stable version), and
        # 'unbridged' otherwise. Nothing is dropped: the path is a receipt (and, if measured useful,
        # a small order factor); rejected paths and their reasons are listed for the caller.
        labels = graph.labels() if graph.stable is not None else None
        navigation = {}
        active_regions = set()
        if labels is not None and len(labels):
            vocab = memory._store['vocab'] if hasattr(memory, '_store') else None
            cue_region = {}
            for c in selected:
                cue_id = vocab.id_of(c) if vocab is not None else None
                node = graph.nodes.cue(cue_id) if cue_id is not None else -1
                if node >= 0 and labels[node] >= 0:
                    cue_region[c] = int(labels[node]); active_regions.add(int(labels[node]))
            stable = graph.stable
            node_episode = graph.nodes.node_episode
            for candidate in recalled.candidates:
                region = graph.region_of(candidate.episode_id)
                if region is None or region < 0:
                    navigation[candidate.episode_id] = dict(region=None, path='pending')
                elif region in active_regions:
                    navigation[candidate.episode_id] = dict(region=region, path='local')
                else:
                    # G5 (R3): the record is itself a member of an active region — reached as a member, no crossing
                    member = graph.membership_in(candidate.episode_id, active_regions)
                    if member is not None:
                        navigation[candidate.episode_id] = dict(region=region, path='member', in_region=member[0], weight=round(member[1], 4))
                        continue
                    best = None
                    for c in candidate.matched_cues:
                        r = cue_region.get(c)
                        if r is None or r == region:
                            continue
                        portal = stable.portal(r, region)
                        if portal is None or not (portal['status'] == 'candidate' or portal.get('keys')):
                            continue
                        # G5 (R4): a pair keyed by a shared experience is a crossing in its own right and is
                        # preferred over an edge-only candidate pair
                        rank = (bool(portal.get('keys')), portal['score'])
                        if best is None or rank > best[2]:
                            best = ((min(r, region), max(r, region)), portal, rank)
                    if best is not None:
                        entry = dict(region=region, path='portal', portal=best[0], score=best[1]['score'])
                        pair_keys = best[1].get('keys') or ()
                        if pair_keys:
                            # the shared experience the crossing goes through: one original address, its revision
                            # and outcome — replayable against this version (R4), never a synthetic link
                            key = pair_keys[0]
                            via_id = node_episode.get(key['node'])
                            if via_id is not None:
                                via = full_memory.episode_light(via_id) if hasattr(full_memory, 'episode_light') else full_memory.episode(via_id)
                                entry['via'] = dict(episode_id=via_id, revision=via.revision, weights=list(key['weights']),
                                                    strength=key['strength'], outcome=via.steps[0].outcome, shared=best[1].get('shared'))
                        else:
                            entry['via'] = None
                            entry['bridge'] = 'edges only (no shared experience at the floor)'
                        navigation[candidate.episode_id] = entry
                    else:
                        navigation[candidate.episode_id] = dict(region=region, path='unbridged',
                            reason='no shared experience and no promoted bridge from an active region')
        def words(matched):
            # a matched cue that is a substring of another matched cue is the same word
            # (Hangul 2-4-gram cues): score each word once, by its longest matched form
            longest = sorted((c for c in matched if c in idf), key=len, reverse=True)
            kept = []
            for c in longest:
                if not any(c in k for k in kept):
                    kept.append(c)
            return kept

        promotion_gate = PROMOTION_GATE   # vrs-regions: measured 15 known-answer queries under the old kernel rule:
        #                        MRR .550 -> .673, stable across consolidations, while weighting by the raw strength
        #                        value collapses once pending edges hit the floor
        unbridged_factor = UNBRIDGED_FACTOR
        ask_gate, desc_gate = ASK_GATE, DESCRIPTION_GATE
        query_tokens = [t.casefold() for t in re.findall(r"\w+", str(query))]
        def ask_hits(row, matched_words):
            # the record's own 「찾을 때 묻는 말」 (verdict tail line / memory-doc section / log-entry tail):
            # a query word found there is the author's declared phrasing, the strongest signal we have.
            # Without this a doc that gains an asks section gets *longer* and BM25's length term pushes
            # it below short log entries that merely mention the words (measured 2026-09-15: rank 7 -> 17).
            # A doc's front-matter description counts too, at its own weight (measured on 14 fresh
            # questions: MRR .416 -> .605 with descriptions; the tuned 15 lose a little at equal weight).
            if not ask_gate and not desc_gate:
                return 0.0
            asks, description = memory.asks_of(row.episode_id)      # casefolded, from the recall columns
            # only rare words count (a broad asks list such as the setup doc's would otherwise catch
            # every question that shares a common word with it)
            # ... and only words that *open* a query token (a Korean stem): a matched fragment such as
            # 자는 (from 청약일자는) is not the query's word, and as a substring it hits 사용자는 in an
            # unrelated doc's asks/description (2026-09-18: a 3-word doc outranked the 12-word answer)
            rare = [w.casefold() for w in matched_words if fanout.get(w, 0) <= ASK_GATE_RARE_SHARE * total
                    and any(t == w.casefold() or t.startswith(w.casefold()) for t in query_tokens)]
            a = sum(1 for w in rare if asks and w in asks)
            d = sum(1 for w in rare if description and w in description)
            return (1.0 + ask_gate * min(ASK_GATE_MAX_HITS, a)) * (1.0 + desc_gate * min(ASK_GATE_MAX_HITS, d))
        def order(row):
            length = len(cue_counts[rows_of[row.episode_id]]) if cue_counts is not None else average
            norm = (k1 + 1.0) / (1.0 + k1 * (1.0 - b + b * length / average))
            gate = 1.0 + promotion_gate * (graph.strength(row.episode_id) >= vrs_refine.PROMOTION)
            path = navigation.get(row.episode_id, {}).get('path')
            gate *= unbridged_factor if path == 'unbridged' else 1.0
            matched_words = words(row.matched_cues)
            gate *= ask_hits(row, matched_words)
            return (-sum(idf[c] for c in matched_words) * norm * gate, -row.cue_overlap, row.episode_id)
        recalled = RecallResult(recalled.query, tuple(sorted(recalled.candidates, key=order)),
                                recalled.snapshot_id, source_dependencies=recalled.source_dependencies)
        replayed = replay_memory(LightView(memory) if hasattr(memory, 'episode_light') else memory, recalled)
        re_evidenced = re_evidence_memory(replayed, judge=judge)
        receipt = MemoryActivationReceipt(schema_version='rozephine-memory-activation-v1',
            snapshot_id=memory.snapshot_id, deja_vu=signal, recall=recalled,
            replay=replayed, re_evidence=re_evidenced)
        ids = [c.episode_id for c in receipt.recall.candidates]
        selection = dict(candidate_counts={c: fanout[c] for c in candidates},
            selected_cues=cues, rejected_cues=tuple(c for c in candidates if c not in selected),
            function_word_cues=tuple(c for c in selected if c not in informative),
            selection_method='all_matching_lexical_keys_and_explicit_proposition_closure',
            excluded_kinds=list(exclude_kinds or ()),
            closure_rule='propositions of records matched by an informative cue (fanout below half the store)',
            candidate_order='bm25_over_matched_informative_words_x_promotion_gate_x_asks_gate_then_cue_overlap',
            asks_gate=ask_gate,
            semantic_acceptance_claimed=False)
        return dict(record_count=memory.episode_count, receipt={'activation': receipt}, memory_selection=selection,
            vrs_selection=dict(selection_method='region_consolidated_strengths_and_connectivity_regions',
                numerical_version=vrs_refine.VERSION, logical_implication_claimed=False, grants_authority=False,
                stable_version_id=graph.stable.version_id if graph.stable is not None else None,
                pending_edges=len(graph.flat.src) - (graph.stable.edge_count if graph.stable is not None else 0),
                promotion_gate=promotion_gate),
            current_strengths={i: graph.strength(i) for i in ids}, current_promotions={i: graph.strength(i) >= 1.0 for i in ids},
            current_propositions={i: memory.proposition_of(i) for i in ids},
            region_memberships={i: graph.memberships(i) for i in ids},
            region_navigation=dict(active_regions=sorted(active_regions), paths={i: navigation.get(i) for i in ids},
                shared=(graph.stable.shared_counts() if graph.stable is not None and hasattr(graph.stable, 'shared_counts') else None),
                path_counts={k: sum(1 for i in ids if (navigation.get(i) or {}).get('path') == k)
                             for k in ('local', 'member', 'portal', 'unbridged', 'pending')},
                crossings_keyed=sum(1 for i in ids if (navigation.get(i) or {}).get('via')),
                rejected=[dict(episode_id=i, **navigation[i]) for i in ids if navigation.get(i, {}).get('path') == 'unbridged'],
                portals={f'{a}:{b}': dict(score=p['score'], promoted=p['promoted'], bridges=p['bridges'])
                         for (a, b), p in ((getattr(graph.stable, 'portals', None) or {}).items() if graph.stable is not None else ())
                         if a in active_regions or b in active_regions},
                unbridged_factor=unbridged_factor, scope=scope, semantic_acceptance_claimed=False,
                restricts_memory_access=(scope['applied'] == 'regions'),
                restriction_rule='candidates from active regions and candidate-portal partners; whole store when fewer than the floor; every record stays addressable by id'),
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
