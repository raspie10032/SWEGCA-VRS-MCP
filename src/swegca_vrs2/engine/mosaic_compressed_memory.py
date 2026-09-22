"""Typed lossless observation compression behind the existing hot-memory protocol.

Only cold construction traverses/validates payloads. Hot episode/cue lookups do not
decode bodies; a body string decodes from resident blocks when its key is accessed.
No JSON/pickle parser, hash, I/O, global scan or decoded cache on that path.
"""

from __future__ import annotations

import hashlib
import sys
from collections.abc import Iterable, Mapping
from dataclasses import dataclass
from types import MappingProxyType
from typing import Any

from .mosaic_lossless_blocks import LosslessBlob
from .mosaic_lossless_float_tuple import PackedFloatTuple
from .mosaic_memory_activation import (
    CompositeMemoryActivationIndex, HotMemoryIndex, MemoryActivationIndex,
    MemoryEpisode, MemoryStep,
)


@dataclass(frozen=True, slots=True)
class CompressionPolicy:
    codec: str = "zlib"
    level: int = 3
    block_bytes: int = 262144
    minimum_utf8_bytes: int = 1024

    def __post_init__(self):
        # Validate even an empty source, without requiring a model/dependency load.
        LosslessBlob.build(b"", codec=self.codec, level=self.level,
                           block_bytes=self.block_bytes)
        if type(self.minimum_utf8_bytes) is not int or self.minimum_utf8_bytes < 1:
            raise ValueError("minimum_utf8_bytes must be positive")


@dataclass(frozen=True, slots=True)
class _CompressedText:
    blob: LosslessBlob

    def value(self) -> str:
        # Match the existing semantic snapshot's strict UTF-8 contract.
        return self.blob.read().data.decode("utf-8")


def _expose(node):
    if isinstance(node, (_CompressedText, PackedFloatTuple)):
        return node.value()
    if type(node) is tuple:
        return tuple(_expose(item) for item in node)
    return node


def _closed_node(node):
    if node is None or type(node) in (str, bool, int, float):
        return True
    if isinstance(node, (_CompressedText, PackedFloatTuple, CompressedObservation)):
        return True
    return type(node) is tuple and all(_closed_node(item) for item in node)


@dataclass(frozen=True, slots=True, eq=False)
class CompressedObservation(Mapping):
    """Immutable typed tree. Iteration lists keys without touching string bodies."""

    _nodes: Mapping

    def __post_init__(self):
        nodes = dict(self._nodes)
        if any(type(key) is not str or not _closed_node(value) for key, value in nodes.items()):
            raise TypeError("observation requires string keys and closed immutable typed nodes")
        object.__setattr__(self, "_nodes", MappingProxyType(nodes))

    def __getitem__(self, key):
        return _expose(self._nodes[key])

    def __len__(self):
        return len(self._nodes)

    def __iter__(self):
        return iter(self._nodes)


class _ColdEncoder:
    def __init__(self, policy):
        self.policy = policy
        self.shared: dict[bytes, _CompressedText] = {}
        self.string_references = 0
        self.logical_string_bytes = 0
        self.interned = {}

    def intern(self, value):
        # Builder-local exact string sharing, not process-global sys.intern and
        # not a semantic merge. The temporary pool is released after construction.
        return self.interned.setdefault(value, value) if type(value) is str else value

    def encode(self, value):
        if isinstance(value, Mapping):
            return CompressedObservation({self.intern(key): self.encode(item) for key, item in value.items()})
        if type(value) is tuple:
            if len(value) >= 64 and all(type(item) is float for item in value):
                packed = PackedFloatTuple.build(value, self.policy)
                original_bytes = sys.getsizeof(value) + sum(sys.getsizeof(item) for item in value)
                if packed.blob.resident_size_estimate() + sys.getsizeof(packed) < original_bytes:
                    self.numeric_references = getattr(self, 'numeric_references', 0) + 1
                    self.numeric_values = getattr(self, 'numeric_values', 0) + len(value)
                    self.numeric_payload_bytes = getattr(self, 'numeric_payload_bytes', 0) + packed.blob.stored_payload_bytes
                    return packed
            return tuple(self.encode(item) for item in value)
        if type(value) is str:
            raw = value.encode("utf-8")
            if len(raw) < self.policy.minimum_utf8_bytes:
                return self.intern(value)
            digest = hashlib.sha256(raw).digest()
            node = self.shared.get(digest)
            if node is None:
                blob = LosslessBlob.build(raw, codec=self.policy.codec,
                    level=self.policy.level, block_bytes=self.policy.block_bytes)
                # Small/uncompressible strings should retain their ordinary Python
                # representation if codec object cost exceeds that representation.
                if blob.resident_size_estimate() + sys.getsizeof(_CompressedText(blob)) >= sys.getsizeof(value):
                    return self.intern(value)
                node = _CompressedText(blob)
                self.shared[digest] = node
            elif node.blob.read().data != raw:
                raise ValueError("string content digest collision")
            self.string_references += 1
            self.logical_string_bytes += len(raw)
            return node
        if value is None or type(value) in (bool, int, float):
            # Preserve the original immutable scalar itself, including float bits.
            return value
        raise TypeError(f"unsupported validated observation type: {type(value).__name__}")


@dataclass(frozen=True, slots=True)
class CompressedMemoryActivationIndex:
    """Protocol adapter; deliberately not a subclass of materialized source.

    Existing Composite treats protocol sources as independently indexed sources,
    so adding a wave does not enumerate old episode IDs or cue postings.
    """

    _index: MemoryActivationIndex
    policy: CompressionPolicy
    compression_stats: Mapping[str, int]
    lookup_requires_io: bool = False

    def __post_init__(self):
        if type(self._index) is not MemoryActivationIndex or self.lookup_requires_io:
            raise TypeError("compressed index must wrap a validated hot source")
        object.__setattr__(self, "compression_stats", MappingProxyType(dict(self.compression_stats)))

    @classmethod
    def from_index(cls, source: MemoryActivationIndex, policy: CompressionPolicy):
        if type(source) is not MemoryActivationIndex:
            raise TypeError("convert explicit materialized sources only, never enumerate virtual VRS")
        encoder = _ColdEncoder(policy)
        episodes = {}
        for episode_id, episode in source.episodes_by_id.items():
            steps = []
            for step in episode.steps:
                observation = encoder.encode(step.observation)
                # Construct/validate a DETACHED header at cold time, then attach a
                # closed typed tree produced above. Never modify the source step or
                # call a JSON-based constructor during a hot lookup. The complete
                # resulting content is verified by MemoryActivationIndex's existing
                # semantic digest check below before this adapter can be returned.
                detached = MemoryStep(encoder.intern(step.phase), {},
                    tuple(encoder.intern(value) for value in step.relations),
                    encoder.intern(step.judgment), encoder.intern(step.outcome),
                    tuple(encoder.intern(value) for value in step.evidence_refs))
                object.__setattr__(detached, "observation", observation)
                steps.append(detached)
            copied = MemoryEpisode(episode.episode_id, episode.cues, tuple(steps),
                tuple(encoder.intern(value) for value in episode.source_addresses),
                encoder.intern(episode.revision), encoder.intern(episode.verification_state))
            object.__setattr__(copied, "cues", tuple(encoder.intern(value) for value in copied.cues))
            episodes[episode_id] = copied
        validated = MemoryActivationIndex(source.snapshot_id, episodes,
                                           source.postings_by_cue)
        stats = {
            "compressed_string_references": encoder.string_references,
            "unique_compressed_strings": len(encoder.shared),
            "logical_compressed_string_utf8_bytes": encoder.logical_string_bytes,
            "unique_compressed_string_utf8_bytes": sum(n.blob.raw_size for n in encoder.shared.values()),
            "compressed_string_payload_bytes": sum(n.blob.stored_payload_bytes for n in encoder.shared.values()),
            "packed_numeric_tuple_references": getattr(encoder, 'numeric_references', 0),
            "packed_numeric_values": getattr(encoder, 'numeric_values', 0),
            "packed_numeric_payload_bytes": getattr(encoder, 'numeric_payload_bytes', 0),
        }
        return cls(validated, policy, stats)

    @property
    def snapshot_id(self):
        return self._index.snapshot_id

    @property
    def outcome_counts(self):
        return self._index.outcome_counts

    @property
    def episode_count(self):
        return self._index.episode_count

    @property
    def episodes_by_id(self):
        return self._index.episodes_by_id

    @property
    def postings_by_cue(self):
        return self._index.postings_by_cue

    def episode(self, episode_id):
        return self._index.episode(episode_id)

    def episode_ids_for_cue(self, cue):
        return self._index.episode_ids_for_cue(cue)

    def iter_episode_ids(self) -> Iterable[str]:
        return self._index.iter_episode_ids()


def inherited_compression_policy(index: HotMemoryIndex) -> CompressionPolicy | None:
    """Only source metadata; do not iterate any parent episode/body/posting."""
    if isinstance(index, CompressedMemoryActivationIndex):
        return index.policy
    from .mosaic_paper_vrs_generation_rebind import VrsGenerationBoundMemoryIndex
    if isinstance(index, (CompositeMemoryActivationIndex, VrsGenerationBoundMemoryIndex)):
        sources = index.ordinary_sources if isinstance(index, VrsGenerationBoundMemoryIndex) else index.sources
        for source in reversed(sources):
            policy = inherited_compression_policy(source)
            if policy is not None:
                return policy
    return None


def compress_hot_memory_index(index: HotMemoryIndex, policy: CompressionPolicy) -> HotMemoryIndex:
    """Cold representation conversion; preserve unsupported virtual sources by identity."""
    if isinstance(index, CompressedMemoryActivationIndex):
        if index.policy == policy:
            return index
        return CompressedMemoryActivationIndex.from_index(index._index, policy)
    if type(index) is MemoryActivationIndex:
        return CompressedMemoryActivationIndex.from_index(index, policy)
    if isinstance(index, CompositeMemoryActivationIndex):
        sources = tuple(compress_hot_memory_index(source, policy) for source in index.sources)
        converted = CompositeMemoryActivationIndex(sources)
        if converted.snapshot_id != index.snapshot_id:
            raise ValueError("physical compression changed composite semantic identity")
        return converted
    from .mosaic_paper_vrs_generation_rebind import VrsGenerationBoundMemoryIndex
    if isinstance(index, VrsGenerationBoundMemoryIndex):
        converted = VrsGenerationBoundMemoryIndex(
            ordinary_sources=tuple(compress_hot_memory_index(source, policy)
                                   for source in index.ordinary_sources),
            vrs_source=index.vrs_source,
            effective_vrs_snapshot_id=index.effective_vrs_snapshot_id,
            replaced_vrs_source_snapshot_ids=index.replaced_vrs_source_snapshot_ids,
        )
        if converted.snapshot_id != index.snapshot_id:
            raise ValueError("physical compression changed generation-bound identity")
        return converted
    return index


def resident_python_bytes(value: Any) -> int:
    """Cold diagnostic reachable object size, deduplicated by identity; not RSS.

    Traverse typed internal nodes without exposing/decompressing observations.
    CPython mappingproxy table cost is estimated from an equivalent temporary dict.
    Native workspaces, allocator slack, interpreter and shared class objects excluded.
    """
    from .mosaic_resident_observation_tree import PackedResidentObservation
    from .mosaic_resident_directory_archive import _Episodes, _Postings
    seen = set()

    def visit(item):
        if id(item) in seen:
            return 0
        seen.add(id(item))
        size = sys.getsizeof(item)
        if type(item) is PackedResidentObservation:
            return size + visit(item._blob) + visit(item._externals) + visit(item._fields_by_key)
        if type(item) in (_Episodes, _Postings):
            return size + visit(item.directory) + visit(item.addresses)
        if isinstance(item, Mapping):
            if isinstance(item, MappingProxyType):
                size += sys.getsizeof(dict(item))
            if isinstance(item, CompressedObservation):
                return size + visit(item._nodes)
            return size + sum(visit(k) + visit(v) for k, v in item.items())
        if isinstance(item, (tuple, list)):
            return size + sum(visit(v) for v in item)
        if hasattr(item, "__dataclass_fields__"):
            if hasattr(item, "__dict__"):
                size += sys.getsizeof(item.__dict__)
            return size + sum(visit(getattr(item, name)) for name in item.__dataclass_fields__)
        return size

    return visit(value)
