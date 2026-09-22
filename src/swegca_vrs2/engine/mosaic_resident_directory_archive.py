"""Resident address/header/posting tables; materialize only a requested header.

Explicit offline format, not a main commit. Retained sections and compressed
blocks own immutable RAM. No file, JSON, hash, or body decode occurs on a hot lookup.
"""
from collections.abc import Mapping
from dataclasses import dataclass, asdict
import hashlib
import io
import json
import operator
import struct
from types import MappingProxyType
from time import perf_counter_ns

from .mosaic_compressed_memory import CompressedMemoryActivationIndex, CompressionPolicy, _CompressedText
from .mosaic_lossless_blocks import Block, LosslessBlob
from .mosaic_lossless_float_tuple import PackedFloatTuple
from .mosaic_memory_activation import MemoryActivationIndex, MemoryEpisode, MemoryStep, OUTCOMES, _text, _cue
from .mosaic_resident_observation_tree import PackedResidentObservation, pack_observation, _validate, _frame

MAGIC = b'RZDIR001'
_Q = struct.Struct('<Q')
_EPISODE = struct.Struct('<9Q')
_STEP = struct.Struct('<8Q')
_POST = struct.Struct('<3Q')
_OBS = struct.Struct('<4Q')
_OUTCOMES = tuple(sorted(OUTCOMES))
_SECTIONS = ('refs', 'episodes', 'steps', 'posts', 'posting_ids', 'obs',
             'external_refs', 'observations', 'external_payload')


def _words(data, start, count):
    return struct.unpack_from(f'<{count}Q', data, start*8)


def _span(start, count, size):
    if type(start) is not int or type(count) is not int or not 0 <= start <= size or not 0 <= count <= size-start:
        raise ValueError('resident directory span exceeds extent')


def _freeze_record(cls, names, values):
    result = object.__new__(cls)
    for name, value in zip(names, values, strict=True):
        object.__setattr__(result, name, value)
    return result


@dataclass(frozen=True, slots=True)
class _Directory:
    strings: tuple
    refs: bytes
    episode_rows: bytes
    step_rows: bytes
    posting_ids: bytes
    observations: tuple
    episode_keys: tuple

    def text_refs(self, start, count):
        return tuple(self.strings[i] for i in _words(self.refs, start, count))

    def episode(self, ordinal):
        key, revision, state, cs, cn, ss, sn, ads, adn = _EPISODE.unpack_from(self.episode_rows, ordinal*_EPISODE.size)
        steps = []
        for i in range(ss, ss+sn):
            phase, judgment, outcome, rs, rn, es, en, obs = _STEP.unpack_from(self.step_rows, i*_STEP.size)
            steps.append(_freeze_record(MemoryStep,
                ('phase', 'observation', 'relations', 'judgment', 'outcome', 'evidence_refs'),
                (self.strings[phase], self.observations[obs].request_view(), self.text_refs(rs, rn),
                 self.strings[judgment], _OUTCOMES[outcome], self.text_refs(es, en))))
        return _freeze_record(MemoryEpisode,
            ('episode_id', 'cues', 'steps', 'source_addresses', 'revision', 'verification_state'),
            (self.strings[key], self.text_refs(cs, cn), tuple(steps), self.text_refs(ads, adn),
             self.strings[revision], self.strings[state]))


@dataclass(frozen=True, slots=True, eq=False)
class _Episodes(Mapping):
    directory: _Directory
    addresses: Mapping

    def __len__(self): return len(self.addresses)
    def __iter__(self): return iter(self.addresses)
    def __getitem__(self, key): return self.directory.episode(self.addresses[key])
    def __contains__(self, key): return key in self.addresses


@dataclass(frozen=True, slots=True, eq=False)
class _Postings(Mapping):
    directory: _Directory
    addresses: Mapping

    def __len__(self): return len(self.addresses)
    def __iter__(self): return iter(self.addresses)
    def __contains__(self, key): return key in self.addresses
    def __getitem__(self, key):
        start, count = self.addresses[key]
        return tuple(self.directory.episode_keys[i]
            for i in _words(self.directory.posting_ids, start, count))


@dataclass(frozen=True, slots=True, eq=False)
class _PostingIDs:
    """Borrow the immutable ID buffer; resolve one address only when consumed."""
    directory: _Directory

    def __len__(self):
        return len(self.directory.posting_ids) // _Q.size

    def __getitem__(self, position):
        position = operator.index(position)
        if position < 0:
            position += len(self)
        if not 0 <= position < len(self):
            raise IndexError(position)
        ordinal = _Q.unpack_from(self.directory.posting_ids, position * _Q.size)[0]
        return self.directory.episode_keys[ordinal]


def dump_directory_leaf(source):
    """One-time verified conversion of a compressed leaf, not a new experience.

    This physical format requires the declared text header types. Unsupported
    headers fail explicitly; the existing general archive remains available.
    """
    return b''.join(_directory_leaf_parts(source))


class _PayloadParts:
    """Transient immutable source spans, not a second complete body buffer."""
    def __init__(self):
        self.parts = []
        self.size = 0

    def tell(self):
        return self.size

    def write(self, value):
        self.parts.append(value)
        self.size += len(value)


def _directory_leaf_parts(source):
    from .mosaic_memory_activation import _validated_memory_content
    if type(source) is not CompressedMemoryActivationIndex:
        raise TypeError('compressed ordinary leaf required')
    _validated_memory_content(source.snapshot_id, source.episodes_by_id, source.postings_by_cue)
    streams = {name: io.BytesIO() for name in _SECTIONS}
    streams['observations'] = _PayloadParts()
    streams['external_payload'] = _PayloadParts()
    strings, string_ids, reference_spans, external_ids, externals = [], {}, {}, {}, []
    def string(value):
        if type(value) is not str: raise TypeError('directory format requires text headers')
        result = string_ids.get(value)
        if result is None:
            result = len(strings); strings.append(value); string_ids[value] = result
        return result
    def refs(values):
        values = tuple(values)
        if any(type(v) is not str for v in values):
            raise TypeError('directory format requires text header references')
        if values not in reference_spans:
            start = streams['refs'].tell()//8
            for value in values: streams['refs'].write(_Q.pack(string(value)))
            reference_spans[values] = (start, len(values))
        return reference_spans[values]
    def external(value):
        if id(value) in external_ids: return external_ids[id(value)]
        if type(value) not in (_CompressedText, PackedFloatTuple):
            raise TypeError('unsupported directory external')
        index = len(externals); external_ids[id(value)] = index
        blocks = []
        for block in value.blob.blocks:
            start = streams['external_payload'].tell()
            streams['external_payload'].write(block.payload)
            blocks.append([block.codec, start, len(block.payload), block.raw_size, block.crc32])
        externals.append([type(value).__name__, getattr(value, 'count', None), getattr(value, 'format', None),
            value.blob.block_bytes, value.blob.raw_size, value.blob.content_sha256, blocks])
        return index
    keys = tuple(source.episodes_by_id)
    ordinals = {key: i for i, key in enumerate(keys)}
    for key, episode in source.episodes_by_id.items():
        cs, cn = refs(episode.cues); ads, adn = refs(episode.source_addresses)
        ss = streams['steps'].tell()//_STEP.size
        for step in episode.steps:
            packed = pack_observation(step.observation)
            start = streams['observations'].tell()
            streams['observations'].write(memoryview(packed._blob)[packed._start:packed._stop])
            es = streams['external_refs'].tell()//8
            for item in packed._externals: streams['external_refs'].write(_Q.pack(external(item)))
            obs_id = streams['obs'].tell()//_OBS.size
            streams['obs'].write(_OBS.pack(start, packed._stop-packed._start, es, len(packed._externals)))
            rs, rn = refs(step.relations); evs, evn = refs(step.evidence_refs)
            streams['steps'].write(_STEP.pack(string(step.phase), string(step.judgment),
                _OUTCOMES.index(step.outcome), rs, rn, evs, evn, obs_id))
        streams['episodes'].write(_EPISODE.pack(string(key), string(episode.revision),
            string(episode.verification_state), cs, cn, ss, len(episode.steps), ads, adn))
    for cue, ids in source.postings_by_cue.items():
        start = streams['posting_ids'].tell()//8
        for key in ids: streams['posting_ids'].write(_Q.pack(ordinals[key]))
        streams['posts'].write(_POST.pack(string(cue), start, len(ids)))
    doc = dict(schema=1, snapshot_id=source.snapshot_id, strings=strings, externals=externals,
        sections={name:streams[name].tell() for name in _SECTIONS},
        policy=asdict(source.policy), stats=dict(source.compression_stats), outcomes=dict(source.outcome_counts))
    header = json.dumps(doc, ensure_ascii=False, separators=(',', ':')).encode('utf-8')
    parts = [MAGIC, _Q.pack(len(header)), header]
    for name in _SECTIONS:
        stream = streams[name]
        if type(stream) is _PayloadParts:
            parts.extend(stream.parts)
        else:
            parts.append(stream.getvalue())
    return tuple(parts)


def _unique_pairs(pairs):
    result = {}
    for key, value in pairs:
        if key in result: raise ValueError('duplicate directory metadata key')
        result[key] = value
    return result


def load_directory_leaf(blob, *, expected_sha256, expected_snapshot_id, maximum_bytes, timings_ns=None):
    from .mosaic_resident_leaf_archive import _digest
    began = perf_counter_ns()
    if timings_ns is not None and type(timings_ns) is not dict:
        raise TypeError('cold timing collector must be a dict')
    _digest(expected_sha256); _digest(expected_snapshot_id)
    if type(blob) is not bytes or type(maximum_bytes) is not int or not 0 <= len(blob) <= maximum_bytes:
        raise ValueError('resident directory exceeds cold capacity')
    if hashlib.sha256(blob).hexdigest() != expected_sha256:
        raise ValueError('resident directory seal differs')
    sealed = perf_counter_ns()
    if len(blob) < 16 or blob[:8] != MAGIC: raise ValueError('invalid resident directory')
    size = _Q.unpack_from(blob, 8)[0]
    _span(16, size, len(blob))
    doc = json.loads(blob[16:16+size], object_pairs_hook=_unique_pairs)
    sizes = _directory_section_sizes(doc, expected_snapshot_id, 16+size, len(blob))
    sections, position = {}, 16+size
    for name, count in sizes:
        # Retained sections own bytes; the temporary external view cannot escape.
        sections[name] = (memoryview(blob)[position:position+count]
                          if name == 'external_payload' else blob[position:position+count])
        position += count
    return _load_directory_sections(doc, sections, expected_snapshot_id,
                                    began, sealed, timings_ns)


def _directory_section_sizes(doc, expected_snapshot_id, position, total):
    """Validate sealed framing before allocating any section."""
    if (type(doc) is not dict or set(doc) != {'schema','snapshot_id','strings','externals','sections','policy','stats','outcomes'}
            or type(doc['schema']) is not int or doc['schema'] != 1 or doc['snapshot_id'] != expected_snapshot_id):
        raise ValueError('resident directory schema or identity differs')
    if type(doc['sections']) is not dict or set(doc['sections']) != set(_SECTIONS):
        raise ValueError('invalid resident directory sections')
    sizes = []
    for name in _SECTIONS:
        count = doc['sections'][name]; _span(position, count, total)
        sizes.append((name, count))
        position += count
    if position != total: raise ValueError('unclaimed resident directory bytes')
    return tuple(sizes)


def _load_directory_sections(doc, sections, expected_snapshot_id, began, sealed, timings_ns):
    """Shared admission for already sealed immutable bytes, never raw streams."""
    strings = doc['strings']
    if type(strings) is not list or any(type(v) is not str for v in strings) or len(set(strings)) != len(strings):
        raise ValueError('invalid resident string pool')
    strings = tuple(strings)
    for name, width in (('refs',8),('episodes',_EPISODE.size),('steps',_STEP.size),
                       ('posts',_POST.size),('posting_ids',8),('obs',_OBS.size),('external_refs',8)):
        if len(sections[name]) % width: raise ValueError('misaligned resident directory section')
    def word_bounds(data, bound):
        for start in range(0, len(data)//8, 65536):
            values = _words(data, start, min(65536, len(data)//8-start))
            if values and max(values) >= bound: raise ValueError('resident directory reference outside pool')
    word_bounds(sections['refs'], len(strings))
    external_values = []
    payload = sections['external_payload']
    end = 0
    for kind, count, fmt, block_bytes, raw_size, digest, rows in doc['externals']:
        blocks = []
        for codec, start, extent, raw, crc in rows:
            _span(start, extent, len(payload))
            if start != end: raise ValueError('noncontiguous external payload')
            blocks.append(Block(codec, bytes(payload[start:start+extent]), raw, crc)); end += extent
        value = LosslessBlob(tuple(blocks), block_bytes, raw_size, digest)
        if kind == '_CompressedText' and count is None and fmt is None: value = _CompressedText(value)
        elif kind == 'PackedFloatTuple': value = PackedFloatTuple(value, count, fmt)
        else: raise ValueError('unknown resident directory external')
        external_values.append(value)
    if end != len(payload): raise ValueError('unclaimed external payload')
    word_bounds(sections['external_refs'], len(external_values))
    parsed = perf_counter_ns()
    observations, end, external_end = [], 0, 0
    for start, extent, es, en in _OBS.iter_unpack(sections['obs']):
        _span(start, extent, len(sections['observations'])); _span(es,en,len(sections['external_refs'])//8)
        if start != end or es != external_end: raise ValueError('noncontiguous observation directory')
        external = tuple(external_values[i] for i in _words(sections['external_refs'], es, en))
        data = sections['observations']; stop = start+extent
        if _frame(data,start,stop)[0] != b'M' or _validate(data,start,stop,external) != stop:
            raise ValueError('invalid complete observation root')
        observations.append(PackedResidentObservation._view(data,external,start,stop,indexed=False))
        end, external_end = stop, es+en
    if end != len(sections['observations']) or external_end != len(sections['external_refs'])//8:
        raise ValueError('unclaimed observation data')
    observed = perf_counter_ns()
    refs_count, step_count = len(sections['refs'])//8, len(sections['steps'])//_STEP.size
    def scalar(index):
        if index >= len(strings): raise ValueError('header string outside pool')
        return strings[index]
    def text_refs(start, count):
        _span(start, count, refs_count)
        return _words(sections['refs'],start,count)
    actual_counts = {outcome:0 for outcome in OUTCOMES}
    if len(observations) != step_count: raise ValueError('step observation directory count differs')
    for ordinal, row in enumerate(_STEP.iter_unpack(sections['steps'])):
        phase, judgment, outcome, rs, rn, es, en, obs = row
        _text(scalar(phase),'memory phase'); _text(scalar(judgment),'memory judgment')
        if outcome >= len(_OUTCOMES) or obs != ordinal: raise ValueError('invalid step outcome or observation')
        text_refs(rs,rn)
        if not en or any(not strings[i].strip() for i in text_refs(es,en)):
            raise ValueError('memory step requires provenance')
        actual_counts[_OUTCOMES[outcome]] += 1
    if (type(doc['outcomes']) is not dict or doc['outcomes'] != actual_counts
            or any(type(v) is not int for v in doc['outcomes'].values())):
        raise ValueError('resident directory outcome counts differ')
    addresses, step_end, canonical = {}, 0, set()
    def cue(index):
        value = scalar(index)
        if index not in canonical:
            if _cue(value) != value: raise ValueError('noncanonical directory cue')
            canonical.add(index)
        return value
    for ordinal, row in enumerate(_EPISODE.iter_unpack(sections['episodes'])):
        key, revision, state, cs, cn, ss, sn, ads, adn = row
        key = scalar(key); _text(key,'episode_id'); _text(scalar(revision),'revision'); _text(scalar(state),'state')
        if key in addresses or not cn or not sn or not adn: raise ValueError('invalid episode directory header')
        cues = text_refs(cs,cn); source_addresses = text_refs(ads,adn)
        if len(set(cues)) != cn or len(set(source_addresses)) != adn: raise ValueError('duplicate episode metadata')
        for index in cues: cue(index)
        _span(ss,sn,step_count)
        if ss != step_end: raise ValueError('noncontiguous episode steps')
        step_end = ss+sn; addresses[key] = ordinal
    if step_end != step_count: raise ValueError('unclaimed episode steps')
    keys = tuple(addresses)
    word_bounds(sections['posting_ids'],len(keys))
    postings, post_end = {}, 0
    for index, start, count in _POST.iter_unpack(sections['posts']):
        key = cue(index); _span(start,count,len(sections['posting_ids'])//8)
        if key in postings or not count or start != post_end: raise ValueError('invalid posting directory')
        previous = None
        for ordinal in _words(sections['posting_ids'],start,count):
            value = keys[ordinal]
            if previous is not None and previous >= value: raise ValueError('noncanonical posting order')
            previous = value
        postings[key] = (start,count); post_end = start+count
    if post_end != len(sections['posting_ids'])//8: raise ValueError('unclaimed posting data')
    directory = _Directory(strings,sections['refs'],sections['episodes'],sections['steps'],
        sections['posting_ids'],tuple(observations),keys)
    header_validated = perf_counter_ns()
    from .mosaic_proposition_directory import PropositionDirectory
    # Cold metadata pass over admitted observation references, not episode body
    # reconstruction, JSON decoding or rehashing. No archive format mutation.
    propositions = PropositionDirectory.from_rows(
        (keys[ordinal], (observations[i] for i in range(row[5], row[5]+row[6])))
        for ordinal, row in enumerate(_EPISODE.iter_unpack(sections['episodes'])))
    propositions_built = perf_counter_ns()
    index = object.__new__(MemoryActivationIndex)
    values = dict(snapshot_id=expected_snapshot_id,episodes_by_id=_Episodes(directory,MappingProxyType(addresses)),
        postings_by_cue=_Postings(directory,MappingProxyType(postings)),
        outcome_counts=MappingProxyType(actual_counts),lookup_requires_io=False,
        proposition_directory=propositions)
    values.update({'includes_'+outcome:bool(actual_counts[outcome]) for outcome in OUTCOMES})
    for name,value in values.items(): object.__setattr__(index,name,value)
    result = CompressedMemoryActivationIndex(index,CompressionPolicy(**doc['policy']),doc['stats'])
    ended = perf_counter_ns()
    if timings_ns is not None:
        timings_ns.update(archive_seal=sealed-began,metadata_and_sections=parsed-sealed,
            observation_admission=observed-parsed,
            header_directory_validation=header_validated-observed,
            proposition_directory=propositions_built-header_validated,
            index_wrapper=ended-propositions_built,total=ended-began)
    return result
