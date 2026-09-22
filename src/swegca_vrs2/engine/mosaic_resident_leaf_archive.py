"""Sealed cold storage for ordinary compressed leaves, not a pickle loader.

Export verifies semantic identity once. Import requires the exact archive digest
AND leaf identity from an independently admitted generation manifest. It restores
closed typed headers, postings and resident compressed bytes, without decoding
observation bodies, rehashing semantic content or running a compression builder.
An archive's self-declared identity alone is never an admission credential.
Virtual VRS sources are not enumerated or stored by this format.
"""
from collections.abc import Mapping
from dataclasses import fields
import hashlib
import json
import struct
from types import MappingProxyType
from time import perf_counter_ns

from .mosaic_compressed_memory import (
    CompressedMemoryActivationIndex, CompressedObservation, CompressionPolicy,
    _CompressedText, _closed_node,
)
from .mosaic_lossless_blocks import Block, LosslessBlob
from .mosaic_lossless_float_tuple import PackedFloatTuple
from .mosaic_memory_activation import MemoryActivationIndex, MemoryEpisode, MemoryStep, OUTCOMES, _cue, _text
from .mosaic_memory_activation import _validated_memory_content
from .mosaic_resident_header_stream import header_events
from .mosaic_resident_framed_header import MAGIC as FRAMED_MAGIC, framed_header_events
from .mosaic_resident_observation_tree import PackedResidentObservation, pack_observation


MAGIC = b'RZLEAF01'
PACKED_MAGIC = b'RZLEAF03'
_TYPES = {cls.__name__: cls for cls in (
    Block, LosslessBlob, PackedFloatTuple, _CompressedText, CompressionPolicy,
    MemoryStep, MemoryEpisode,
)}


def _step_from_resident_fields(args):
    """Retain header checks without round-tripping a discarded empty body.

    Only the closed cold loader calls this with a detached, already decoded
    typed observation. It is not a public admission path for arbitrary bodies.
    """
    phase, observation, relations, judgment, outcome, evidence_refs = args
    if not isinstance(observation, Mapping):
        raise ValueError('resident observation must be a mapping')
    _text(phase, 'memory phase')
    _text(judgment, 'memory judgment')
    if outcome not in OUTCOMES:
        raise ValueError('unsupported historical outcome')
    if not evidence_refs or any(not str(ref).strip() for ref in evidence_refs):
        raise ValueError('memory step requires evidence provenance')
    result = object.__new__(MemoryStep)
    for name, value in zip(('phase', 'observation', 'relations', 'judgment',
            'outcome', 'evidence_refs'),
            (phase, observation, tuple(relations), judgment, outcome, tuple(evidence_refs)), strict=True):
        object.__setattr__(result, name, value)
    return result


def _episode_from_resident_fields(args, normalize):
    """Same MemoryEpisode header rules, using restore-local cue normalization.

    No persistent semantic validation is cached. The external archive seal has
    already been admitted; all original field checks and tuple detachment remain.
    """
    key, cues, steps, addresses, revision, verification = args
    _text(key, 'episode_id')
    normalized = tuple(dict.fromkeys(normalize(cue) for cue in cues))
    if not normalized or not steps or not addresses:
        raise ValueError('memory episode is incomplete')
    if len(addresses) != len(set(addresses)):
        raise ValueError('memory source addresses must be unique')
    _text(revision, 'memory revision')
    _text(verification, 'verification state')
    result = object.__new__(MemoryEpisode)
    for name, value in zip(('episode_id', 'cues', 'steps', 'source_addresses',
            'revision', 'verification_state'),
            (key, normalized, tuple(steps), tuple(addresses), revision, verification), strict=True):
        object.__setattr__(result, name, value)
    return result


def _digest(value):
    if type(value) is not str or len(value) != 64 or any(c not in '0123456789abcdef' for c in value):
        raise ValueError('external SHA256 seal required')
    return value


def _observation_from_resident_fields(nodes, closed_node=_closed_node):
    """Check values and attach the decoder's detached, key-validated mapping.

    Only the closed archive decoder calls this. Keys have been validated there;
    do not copy the same dict and check the same keys for a second time.
    No caller-owned mutable mapping is admitted through a public constructor.
    """
    for value in nodes.values():
        if value is None or type(value) in (str, bool, int, float):
            continue
        if not closed_node(value):
            raise TypeError('observation requires string keys and closed immutable typed nodes')
    result = object.__new__(CompressedObservation)
    object.__setattr__(result, '_nodes', MappingProxyType(nodes))
    return result


def dump_resident_leaf(source, *, compact_observations=False, resident_directory=False):
    """One-time offline export of an already-built, compressed ordinary leaf.

    The returned bytes have no write/promotion authority. The caller publishes
    their SHA256 with the exact generation through its existing commit boundary.
    """
    return b''.join(_resident_leaf_parts(source,
        compact_observations=compact_observations, resident_directory=resident_directory))


def write_resident_leaf(source, stream, *, maximum_bytes, compact_observations=False,
                        resident_directory=False):
    """Write exact existing archive bytes without a joined whole-file buffer.

    Capacity is checked before any write. A failure never produces a receipt;
    callers keep partial output uncommitted and own flush/fsync/publication.
    """
    if type(maximum_bytes) is not int or maximum_bytes < 0:
        raise ValueError('explicit resident export capacity required')
    parts = _resident_leaf_parts(source, compact_observations=compact_observations,
                                 resident_directory=resident_directory)
    size = sum(map(len, parts))
    if size > maximum_bytes:
        raise ValueError('resident export exceeds declared capacity')
    digest = hashlib.sha256()
    for part in parts:
        _write_all(stream, part, digest=digest)
    return dict(bytes=size, sha256=digest.hexdigest())


def _write_all(stream, data, *, digest=None):
    """Bounded writes, including short writes; hash only bytes actually accepted."""
    view = memoryview(data)
    position = 0
    while position < len(view):
        chunk = view[position:position+4*1024*1024]
        count = stream.write(chunk)
        if type(count) is not int or not 0 < count <= len(chunk):
            raise OSError('resident export short write made invalid progress')
        if digest is not None:
            digest.update(chunk[:count])
        position += count


def _resident_leaf_parts(source, *, compact_observations=False, resident_directory=False):
    """Prepare one immutable snapshot's metadata and borrowed payload segments."""
    if type(compact_observations) is not bool or type(resident_directory) is not bool:
        raise TypeError('compact_observations must be an explicit boolean')
    if resident_directory:
        if compact_observations: raise ValueError('choose one resident archive format')
        from .mosaic_resident_directory_archive import _directory_leaf_parts
        return _directory_leaf_parts(source)
    if type(source) is not CompressedMemoryActivationIndex:
        raise TypeError('compressed ordinary leaf required; no virtual enumeration')
    if not compact_observations:
        from .mosaic_resident_directory_archive import _Episodes, _directory_leaf_parts
        if type(source.episodes_by_id) is _Episodes:
            return _directory_leaf_parts(source)
    # Export is the semantic verification boundary, not every restart.
    _validated_memory_content(source.snapshot_id, source.episodes_by_id, source.postings_by_cue)
    nodes, buffers, memo = [], [], {}
    packed_observations = {}  # Keep converted objects alive while identity memo is used.
    uses_packed = False
    payload_size = 0

    def pack(value):
        nonlocal payload_size, uses_packed
        identity = id(value)
        if identity in memo:
            return memo[identity]
        if compact_observations and type(value) is CompressedObservation:
            value = pack_observation(value)
            packed_observations[identity] = value
        if type(value) is PackedResidentObservation:
            if value._start != 0 or value._stop != len(value._blob):
                value = pack_observation(value)
                packed_observations[identity] = value
            uses_packed = True
            row = ['packed_observation', [pack(value._blob), pack(value._externals)]]
        elif value is None or type(value) in (str, bool, int):
            row = ['scalar', value]
        elif type(value) is float:
            row = ['float64', struct.pack('>d', value).hex()]
        elif type(value) is bytes:
            row = ['bytes', payload_size, len(value)]
            buffers.append(value)
            payload_size += len(value)
        elif type(value) is tuple:
            row = ['tuple', [pack(v) for v in value]]
        elif isinstance(value, Mapping):
            raw = value._nodes if type(value) is CompressedObservation else value
            row = ['observation' if type(value) is CompressedObservation else 'mapping',
                   [[key, pack(v)] for key, v in raw.items()]]
        elif type(value).__name__ in _TYPES and _TYPES[type(value).__name__] is type(value):
            row = [type(value).__name__, [pack(getattr(value, f.name)) for f in fields(value)]]
        else:
            raise TypeError('unsupported resident leaf value')
        index = len(nodes)
        nodes.append(row)
        memo[identity] = index
        return index

    # Keep temporary root values alive while the object-identity memo is used.
    root = (source.policy, source.compression_stats, source.episodes_by_id,
            source.postings_by_cue, source.outcome_counts)
    root_ref = pack(root)
    header = json.dumps(dict(schema=1, snapshot_id=source.snapshot_id,
        root=root_ref, nodes=nodes), ensure_ascii=False, separators=(',', ':')).encode('utf-8')
    return (PACKED_MAGIC if uses_packed else MAGIC, struct.pack('>Q', len(header)), header, *buffers)


def load_resident_leaf(blob, *, expected_sha256, expected_snapshot_id, maximum_bytes,
                       timings_ns=None):
    """Cold admission of sealed bytes; result has no retained file/mmap handles.

    Do not derive expected_sha256 from the candidate being loaded. It must come
    from the separately checked, current generation manifest. The physical seal
    replaces repeat semantic hashing only for this versioned exporter format.
    """
    from .mosaic_resident_directory_archive import MAGIC as DIRECTORY_MAGIC, load_directory_leaf
    if type(blob) is bytes and blob[:8] == DIRECTORY_MAGIC:
        return load_directory_leaf(blob,expected_sha256=expected_sha256,
            expected_snapshot_id=expected_snapshot_id,maximum_bytes=maximum_bytes,timings_ns=timings_ns)
    began = perf_counter_ns()
    if timings_ns is not None and type(timings_ns) is not dict:
        raise TypeError('cold timing collector must be a dict')
    _digest(expected_sha256); _digest(expected_snapshot_id)
    if (type(blob) is not bytes or type(maximum_bytes) is not int
            or maximum_bytes < 0 or len(blob) > maximum_bytes):
        raise ValueError('resident leaf exceeds declared cold byte budget')
    if hashlib.sha256(blob).hexdigest() != expected_sha256:
        raise ValueError('resident leaf archive seal differs')
    sealed = perf_counter_ns()
    if len(blob) < 16 or blob[:8] not in (MAGIC, FRAMED_MAGIC, PACKED_MAGIC):
        raise ValueError('unsupported resident leaf archive')
    size = struct.unpack('>Q', blob[8:16])[0]
    if size > len(blob)-16:
        raise ValueError('truncated resident leaf header')
    header = {}
    parsed = perf_counter_ns()
    values, offset = [], 16+size
    # Repeated literal metadata is normalized once per cold leaf. This pool has
    # no process-global lifetime and is never retained by the returned index.
    normalized_cues = {}
    # JSON array-element keys are not shared by the decoder. Retain one exact
    # immutable spelling per observation field, local to this cold restore.
    # Do not pool ordinary mappings' often-unique episode/address keys.
    observation_keys = {}
    layouts = {cls: fields(cls) for cls in _TYPES.values()}

    def normalize(value):
        if type(value) is not str:
            return _cue(value)
        try:
            return normalized_cues[value]
        except KeyError:
            normalized = _cue(value)
            normalized_cues[value] = normalized
            return normalized

    def ref(index):
        if type(index) is not int or not 0 <= index < len(values):
            raise ValueError('resident leaf forward/cyclic/invalid reference')
        return values[index]

    events = framed_header_events if blob[:8] == FRAMED_MAGIC else header_events
    for field, row in events(blob, size):
        if field != 'nodes':
            header[field] = row
            continue
        if type(row) is not list:
            raise ValueError('invalid typed resident node')
        arity = len(row)
        if arity not in (2, 3):
            raise ValueError('invalid typed resident node')
        tag = row[0]
        if tag == 'bytes' and arity == 3:
            start, count = row[1:]
            if (type(start) is not int or type(count) is not int or count < 0
                    or start != offset-(16+size) or count > len(blob)-offset):
                raise ValueError('invalid resident payload extent')
            value = blob[offset:offset+count]
            offset += count
        elif arity != 2:
            raise ValueError('invalid resident node arity')
        elif tag == 'scalar' and (row[1] is None or type(row[1]) in (str, bool, int)):
            value = row[1]
        elif tag == 'float64':
            value = struct.unpack('>d', bytes.fromhex(row[1]))[0]
        elif tag == 'tuple':
            # No value is appended until the complete node has been resolved.
            # Its backward-reference limit is therefore invariant throughout
            # this loop. Keep every exact-int/range check, without a generator
            # and a Python function/len call for every tuple member.
            reference_limit = len(values)
            members = []
            for index in row[1]:
                if type(index) is not int or not 0 <= index < reference_limit:
                    raise ValueError('resident leaf forward/cyclic/invalid reference')
                members.append(values[index])
            value = tuple(members)
            del members
        elif tag in ('mapping', 'observation'):
            # One pass over pairs: the detached output also tracks admitted keys.
            # Keep duplicate/type/reference rejection; do not build and discard
            # a second key set or traverse the same pairs three times.
            value = {}
            reference_limit = len(values)  # unchanged until this node is complete
            is_observation = tag == 'observation'
            for key, index in row[1]:
                if type(key) is not str or key in value:
                    raise ValueError('invalid resident mapping keys')
                if type(index) is not int or not 0 <= index < reference_limit:
                    raise ValueError('resident leaf forward/cyclic/invalid reference')
                if is_observation:
                    key = observation_keys.setdefault(key, key)
                value[key] = values[index]
            if is_observation:
                value = _observation_from_resident_fields(value)
            else:
                value = MappingProxyType(value)
        elif tag == 'packed_observation' and blob[:8] == PACKED_MAGIC:
            if type(row[1]) is not list or len(row[1]) != 2:
                raise ValueError('invalid packed observation layout')
            value = PackedResidentObservation(*(ref(i) for i in row[1]))
        elif tag in _TYPES:
            cls = _TYPES[tag]
            args = [ref(i) for i in row[1]]
            if len(args) != len(layouts[cls]):
                raise ValueError('resident type layout differs')
            if cls is MemoryStep:
                value = _step_from_resident_fields(args)
            elif cls is MemoryEpisode:
                value = _episode_from_resident_fields(args, normalize)
            else:
                value = cls(*args)
        else:
            raise ValueError('unsupported resident node type')
        values.append(value)
    if header['schema'] != 1 or header['snapshot_id'] != expected_snapshot_id:
        raise ValueError('resident leaf schema or generation differs')
    if offset != len(blob):
        raise ValueError('unclaimed resident payload bytes')
    typed = perf_counter_ns()
    policy, stats, episodes, postings, counts = ref(header['root'])
    if (type(policy) is not CompressionPolicy or not all(isinstance(v, Mapping)
            for v in (stats, episodes, postings, counts))):
        raise ValueError('invalid resident leaf root')
    actual_counts = {outcome: 0 for outcome in OUTCOMES}
    for key, episode in episodes.items():
        if type(episode) is not MemoryEpisode or key != episode.episode_id:
            raise ValueError('resident episode address differs')
        for step in episode.steps:
            if type(step) is not MemoryStep:
                raise ValueError('invalid resident step')
            actual_counts[step.outcome] += 1
    if dict(counts) != actual_counts:
        raise ValueError('resident outcome counts differ')
    for cue, identifiers in postings.items():
        if (normalize(cue) != cue or type(identifiers) is not tuple or not identifiers
                or any(type(k) is not str or k not in episodes for k in identifiers)
                or any(a >= b for a, b in zip(identifiers, identifiers[1:]))):
            raise ValueError('resident posting differs')
    # Exact bytes already admitted against the external seal above. Re-running
    # MemoryActivationIndex.__post_init__ here would decode/hash every body.
    index = object.__new__(MemoryActivationIndex)
    from .mosaic_proposition_directory import PropositionDirectory
    propositions = PropositionDirectory.from_rows(
        (episode.episode_id, (step.observation for step in episode.steps))
        for episode in episodes.values())
    for key, value in dict(snapshot_id=expected_snapshot_id, episodes_by_id=episodes,
            postings_by_cue=postings, outcome_counts=counts, lookup_requires_io=False,
            proposition_directory=propositions).items():
        object.__setattr__(index, key, value)
    for outcome in OUTCOMES:
        object.__setattr__(index, 'includes_'+outcome, bool(counts[outcome]))
    result = CompressedMemoryActivationIndex(index, policy, stats)
    ended = perf_counter_ns()
    if timings_ns is not None:
        timings_ns.update(archive_seal=sealed-began, metadata_stream_setup=parsed-sealed,
            streamed_metadata_and_typed_resident_blocks=typed-parsed,
            address_validation_and_index_attach=ended-typed, total=ended-began)
    return result
