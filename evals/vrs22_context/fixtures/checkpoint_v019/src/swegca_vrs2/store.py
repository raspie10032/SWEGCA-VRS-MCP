"""Data-only checkpoint fixture types with no persistent storage backend."""
from __future__ import annotations

from dataclasses import asdict, dataclass, is_dataclass
import hashlib
import json
from types import SimpleNamespace

from immutables import Map
import numpy as np


@dataclass(frozen=True)
class MemoryStep:
    phase: str
    observation: dict
    relations: tuple
    judgment: str
    outcome: str
    evidence_refs: tuple


@dataclass(frozen=True)
class MemoryEpisode:
    episode_id: str
    cues: tuple
    steps: tuple
    source_addresses: tuple
    revision: str
    verification_state: str


class HotIndex:
    def __init__(self, snapshot_id, records, postings, outcomes, propositions, superseded):
        self.snapshot_id = snapshot_id
        self.records = records
        self.postings = postings
        self.outcome_counts = outcomes
        self.propositions = propositions
        self.superseded = superseded
        self.episode_count = len(records)

    def episode(self, identifier):
        return self.records[identifier]


class EndpointDependencyIndex:
    @staticmethod
    def build(edges):
        return SimpleNamespace(edges=edges)


class EventVrsInputs:
    def __init__(self, snapshot_id, direct, score, strength, unresolved, edges,
                 dependencies=None):
        self.snapshot_id = snapshot_id
        self.direct = direct
        self.score = score
        self.strength = strength
        self.unresolved = unresolved
        self.edges = edges
        self.dependencies = dependencies


@dataclass(frozen=True)
class ConnectivityRegions:
    source: object = None
    terms: tuple = ()
    sweeps: tuple = ()
    region_terms: object = None
    edge_source: object = None
    edge_target: object = None
    edge_sign: object = None
    strengths: object = None


class Graph:
    def __init__(self, inputs, nodes, components, regions, last_receipt):
        self.inputs = inputs
        self.nodes = nodes
        self.components = components
        self.regions = regions
        self.last_receipt = last_receipt


class FullCurrentMemoryVrsSnapshot:
    def __init__(self, _memory, snapshot_id):
        self.snapshot_id = snapshot_id


def frozen(value):
    result = np.asarray(value)
    result.flags.writeable = False
    return result


def freeze_view(value):
    return value


def retrieval_keys(text):
    return tuple(dict.fromkeys(str(text).casefold().split()))


def plain(value):
    if is_dataclass(value):
        return plain(asdict(value))
    if isinstance(value, Map):
        return {str(key): plain(item) for key, item in value.items()}
    if isinstance(value, dict):
        return {str(key): plain(item) for key, item in value.items()}
    if isinstance(value, (tuple, list)):
        return [plain(item) for item in value]
    if isinstance(value, np.ndarray):
        return value.tolist()
    if isinstance(value, np.generic):
        return value.item()
    return value


def canonical(value):
    return json.dumps(plain(value), ensure_ascii=False, sort_keys=True,
                      separators=(',', ':'), allow_nan=False)


class Main:
    """One in-memory generation sufficient to exercise real checkpoint encoding."""

    def __init__(self, _state=None, allow_ingest=False):
        self.identity = 'clean-checkpoint-fixture'
        self.sequence = 0
        self.operations = Map()
        self._records = {}
        self._outcomes = {}
        self._refresh()

    def _refresh(self):
        snapshot = 'fixture-pair-' + str(self.sequence)
        self.memory = HotIndex(
            snapshot, Map(self._records), Map(), Map(self._outcomes), Map(), Map())
        edges = np.asarray([], dtype=[
            ('source', '<u4'), ('target', '<u4'), ('sign', 'i1')])
        inputs = EventVrsInputs(
            snapshot,
            frozen(np.asarray([], dtype=np.float64)),
            frozen(np.asarray([], dtype=np.float64)),
            frozen(np.asarray([], dtype=np.float64)),
            frozen(np.asarray([], dtype=np.bool_)),
            frozen(edges),
            dependencies=EndpointDependencyIndex.build(edges),
        )
        self.graph = Graph(inputs, Map(), Map(), Map(), Map())
        self.pair = SimpleNamespace(snapshot_id=snapshot)

    def ingest(self, row):
        self.sequence += 1
        text = str(row['text'])
        source = str(row['source'])
        identifier = 'memory:' + hashlib.sha256(
            (source + '\0' + text + '\0' + str(self.sequence)).encode()).hexdigest()
        outcome = str(row.get('outcome', 'pending'))
        observation = {
            'text': text,
            'metadata': dict(row.get('metadata') or {}),
            'proposition_id': row.get('proposition'),
            'evidence_polarity': row.get('polarity'),
        }
        step = MemoryStep(
            'experience', observation, (), 'unverified', outcome, (source,))
        episode = MemoryEpisode(
            identifier,
            tuple(row.get('cues') or retrieval_keys(text)),
            (step,),
            (source,),
            str(row.get('revision', '1')),
            'unverified',
        )
        self._records[identifier] = episode
        self._outcomes[outcome] = self._outcomes.get(outcome, 0) + 1
        self._refresh()
        return {'episode_id': identifier}

    def checkpoint_view(self):
        return self

    def close(self):
        return None
