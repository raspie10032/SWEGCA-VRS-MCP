"""Disk-resident exact-address directory and immutable Replay capsules.

An explicit ``memory:<sha256>`` already names one original experience.  Looking
it up must not scan lexical postings or every storage shard.  This store keeps a
main-owned fixed-slot hash directory on disk and an append-only capsule file.
Only the addressed segment and capsule pages enter RSS; total experience count
does not create a resident Python dictionary.

The capsule is an exact Replay envelope, not a log row or a truth summary.  It
preserves the original observation, provenance, revision, historical outcome
and uncertainty metadata.  Current VRS judgment remains the later Re-evidence
stage and is deliberately not frozen into the capsule.
"""
from __future__ import annotations

from collections import OrderedDict
from collections.abc import Sequence
import json
import hashlib
import mmap
import os
from pathlib import Path
import struct
import threading
import zlib

from .engine.mosaic_memory_activation import MemoryStep, ReplayedEpisode
from .store import plain
from .compact_index import asks_and_description

MAGIC = b'VRS2EXACT5\0'
HEADER_BYTES = 4096
SEGMENT_BITS = 8
SLOT_POWER = int(os.environ.get('VRS2_EXACT_SLOT_POWER', '16'))
SLOT_COUNT = 1 << SLOT_POWER
SLOT = struct.Struct('<32sQII')       # key, capsule offset, payload bytes, CRC32
CAPSULE = struct.Struct('<II')        # payload bytes, CRC32
PAYLOAD = struct.Struct('<II')        # Replay JSON bytes, derived cue JSON bytes
EMPTY = b'\0' * 32
LEVEL_STATE_OFFSET = 64
LEVEL_STATE = struct.Struct('<QB7x')  # published keys, sealed for new keys
LEVEL_LOAD_NUMERATOR = 7
LEVEL_LOAD_DENOMINATOR = 10
READ_SEGMENT_CACHE = int(os.environ.get('VRS2_EXACT_READ_SEGMENTS', '512'))


def _write_all(fd, data):
    """Write the complete buffer; a short regular-file write is still legal."""
    view = memoryview(data)
    while view:
        written = os.write(fd, view)
        if written <= 0:
            raise OSError('exact_replay_capsule_write_failed')
        view = view[written:]


def _key(identifier):
    if (not isinstance(identifier, str) or not identifier.startswith('memory:')
            or len(identifier) != 71):
        raise ValueError('invalid_exact_experience_address')
    try:
        value = bytes.fromhex(identifier[7:])
    except ValueError:
        raise ValueError('invalid_exact_experience_address') from None
    if value == EMPTY:
        raise ValueError('reserved_exact_experience_address')
    return value


def _source_key(source):
    if not isinstance(source, str) or not source:
        raise ValueError('invalid_experience_source_address')
    return hashlib.sha256(source.encode('utf-8')).digest()


class LazyCues(Sequence):
    """Exact derived cues, decoded only when Re-evidence consumes them."""
    __slots__ = ('payload', 'count', 'decoded')

    def __init__(self, payload, count):
        self.payload = payload
        self.count = int(count)
        self.decoded = None

    def __len__(self):
        return self.count

    def _value(self):
        if self.decoded is None:
            try:
                decoded = json.loads(self.payload)
            except (UnicodeDecodeError, json.JSONDecodeError):
                raise ValueError('exact_replay_cues_corrupt') from None
            if (not isinstance(decoded, list) or len(decoded) != self.count
                    or any(not isinstance(cue, str) for cue in decoded)):
                raise ValueError('exact_replay_cues_corrupt')
            self.decoded = tuple(decoded)
            self.payload = None
        return self.decoded

    def __getitem__(self, index):
        return self._value()[index]

    def __iter__(self):
        return iter(self._value())


class ExactReplayStore:
    def __init__(self, directory, *, slot_power=None):
        self.directory = Path(directory)
        self.directory.mkdir(mode=0o700, parents=True, exist_ok=True)
        self.slot_power = SLOT_POWER if slot_power is None else int(slot_power)
        if not 1 <= self.slot_power <= 23:
            raise ValueError('exact_replay_slot_power_out_of_range')
        self.slot_count = 1 << self.slot_power
        self.mask = self.slot_count - 1
        self.slot_powers = tuple(dict.fromkeys(
            (*range(self.slot_power, 23, 2), 23)))
        self.data_path = self.directory / 'capsules.vrs'
        self.proposition_directory = self.directory / 'propositions'
        self.proposition_directory.mkdir(mode=0o700, parents=True, exist_ok=True)
        self.lock = threading.Lock()
        if not self.data_path.exists():
            fd = os.open(self.data_path, os.O_RDWR | os.O_CREAT | os.O_EXCL, 0o600)
            try:
                _write_all(fd, MAGIC.ljust(HEADER_BYTES, b'\0'))
                os.fsync(fd)
            finally:
                os.close(fd)
        else:
            with self.data_path.open('rb') as stream:
                if stream.read(len(MAGIC)) != MAGIC:
                    raise ValueError('exact_replay_capsule_header_invalid')
        self.data_read_fd = os.open(self.data_path, os.O_RDONLY)
        self.read_segment_limit = max(1, READ_SEGMENT_CACHE)
        self.read_segments = OrderedDict()

    def _segment_path(self, key, namespace, slot_power=None):
        slot_power = self.slot_power if slot_power is None else int(slot_power)
        return self.directory / f'{namespace}-p{slot_power}-{key[0]:02x}.vrs'

    def _open_segment(self, key, *, create, namespace='address', slot_power=None):
        slot_power = self.slot_power if slot_power is None else int(slot_power)
        path = self._segment_path(key, namespace, slot_power)
        flags = os.O_RDWR | (os.O_CREAT if create else 0)
        try:
            fd = os.open(path, flags, 0o600)
        except FileNotFoundError:
            return None, None
        expected = HEADER_BYTES + (1 << slot_power) * SLOT.size
        size = os.fstat(fd).st_size
        if size == 0:
            if not create:
                os.close(fd); return None, None
            os.ftruncate(fd, expected)
        elif size != expected:
            os.close(fd)
            raise ValueError('exact_replay_address_segment_invalid')
        mapping = mmap.mmap(fd, expected,
                            access=mmap.ACCESS_WRITE if create else mmap.ACCESS_READ)
        header = mapping[:len(MAGIC) + 2]
        expected_header = MAGIC + bytes((key[0], slot_power))
        if header == b'\0' * len(header):
            if not create:
                mapping.close(); os.close(fd); return None, None
            mapping[:len(expected_header)] = expected_header
        elif header != expected_header:
            mapping.close(); os.close(fd)
            raise ValueError('exact_replay_address_segment_invalid')
        return fd, mapping

    def _read_segment(self, key, namespace, slot_power):
        """Return a bounded cached read mapping; caller holds ``self.lock``."""
        cache_key = (namespace, int(slot_power), key[0])
        cached = self.read_segments.pop(cache_key, None)
        if cached is not None:
            self.read_segments[cache_key] = cached
            return cached[1]
        fd, mapping = self._open_segment(
            key, create=False, namespace=namespace, slot_power=slot_power)
        if mapping is None:
            return None
        self.read_segments[cache_key] = (fd, mapping)
        while len(self.read_segments) > self.read_segment_limit:
            old_fd, old_mapping = self.read_segments.popitem(last=False)[1]
            old_mapping.close(); os.close(old_fd)
        return mapping

    def _probe(self, mapping, key, slot_power=None):
        slot_power = self.slot_power if slot_power is None else int(slot_power)
        slot_count, mask = 1 << slot_power, (1 << slot_power) - 1
        start = int.from_bytes(key[1:9], 'little') & mask
        step = (int.from_bytes(key[9:17], 'little') | 1) & mask
        step = step or 1
        for count in range(slot_count):
            slot = (start + count * step) & mask
            offset = HEADER_BYTES + slot * SLOT.size
            found = mapping[offset:offset + 32]
            if found == key:
                return offset, True
            if found == EMPTY:
                return offset, False
        raise ValueError('exact_replay_address_segment_full')

    def _probe_pending(self, mapping, key, pending, slot_power):
        slot_count, mask = 1 << slot_power, (1 << slot_power) - 1
        start = int.from_bytes(key[1:9], 'little') & mask
        step = (int.from_bytes(key[9:17], 'little') | 1) & mask or 1
        for count in range(slot_count):
            slot = (start + count * step) & mask
            offset = HEADER_BYTES + slot * SLOT.size
            found = pending.get(offset, mapping[offset:offset + 32])
            if found == key:
                return offset, True
            if found == EMPTY:
                return offset, False
        raise ValueError('exact_replay_address_segment_full')

    @staticmethod
    def _level_state(mapping, slot_power):
        count, sealed = LEVEL_STATE.unpack(
            mapping[LEVEL_STATE_OFFSET:LEVEL_STATE_OFFSET + LEVEL_STATE.size])
        if count > 1 << slot_power or sealed not in (0, 1):
            raise ValueError('exact_replay_address_segment_invalid')
        return int(count), bool(sealed)

    @staticmethod
    def _level_threshold(slot_power):
        return max(1, ((1 << slot_power) * LEVEL_LOAD_NUMERATOR
                       // LEVEL_LOAD_DENOMINATOR))

    @staticmethod
    def _write_level_state(mapping, count, sealed):
        mapping[LEVEL_STATE_OFFSET:LEVEL_STATE_OFFSET + LEVEL_STATE.size] = \
            LEVEL_STATE.pack(int(count), bool(sealed))

    def _seal_level(self, fd, mapping, count):
        """Publish overflow routing before a later level can receive keys."""
        self._write_level_state(mapping, count, True)
        mapping.flush(0, HEADER_BYTES)
        os.fsync(fd)

    @staticmethod
    def _encode(identifier, shard, row, episode):
        step = episode.steps[0]
        text = step.observation.get('text', '')
        asks, description = asks_and_description(text)
        body = dict(schema='swegca-vrs2-replay-capsule-v3', episode_id=identifier,
                    shard=str(shard), shard_row=int(row), matched_cues=[identifier],
                    cue_count=len(episode.cues),
                    kind=str((step.observation.get('metadata') or {}).get('kind') or ''),
                    asks=asks.casefold(), description=description.casefold(),
                    steps=[dict(phase=step.phase, observation=plain(step.observation),
                                relations=list(step.relations), judgment=step.judgment,
                                outcome=step.outcome, evidence_refs=list(step.evidence_refs))],
                    source_addresses=list(episode.source_addresses), revision=episode.revision,
                    verification_state=episode.verification_state,
                    historical_truth_authorized=False)
        raw = json.dumps(body, ensure_ascii=False, sort_keys=True,
                         separators=(',', ':'), allow_nan=False).encode('utf-8')
        cues = json.dumps(list(episode.cues), ensure_ascii=False,
                          separators=(',', ':'), allow_nan=False).encode('utf-8')
        return PAYLOAD.pack(len(raw), len(cues)) + raw + cues

    @staticmethod
    def _encode_source(source, shard):
        raw = json.dumps(dict(schema='swegca-vrs2-source-route-v1', source=source,
                              shard=str(shard)), ensure_ascii=False, sort_keys=True,
                         separators=(',', ':'), allow_nan=False).encode('utf-8')
        return PAYLOAD.pack(len(raw), 0) + raw

    def _append_capsule(self, payload):
        checksum = zlib.crc32(payload)
        data_fd = os.open(self.data_path, os.O_RDWR)
        try:
            offset = os.lseek(data_fd, 0, os.SEEK_END)
            _write_all(data_fd, CAPSULE.pack(len(payload), checksum))
            _write_all(data_fd, payload)
            os.fsync(data_fd)
        finally:
            os.close(data_fd)
        return offset, len(payload), checksum

    def put(self, identifier, shard, row, episode):
        return bool(self.put_many(((identifier, shard, row, episode),))['exact'])

    def put_many(self, records):
        """Durably publish many exact capsules and source routes in one I/O batch.

        Capsule bytes reach disk before any address slot is published.  A crash
        can therefore leave only harmless unindexed tail bytes, never an index
        pointing at a partial Replay capsule.
        """
        records = list(records)
        exact_rows = [(identifier, str(shard), int(row), episode,
                       self._encode(identifier, shard, row, episode))
                      for identifier, shard, row, episode in records]
        sources = {}
        for _, shard, _, episode, _ in exact_rows:
            for source in episode.source_addresses:
                previous = sources.get(source)
                if previous is not None and previous != shard:
                    raise ValueError('experience_source_lineage_split')
                sources[source] = shard
        source_rows = [(source, shard, self._encode_source(source, shard))
                       for source, shard in sources.items()]
        mappings, pending_slots, level_states, pending, staged = {}, {}, {}, {}, []
        added_exact = added_sources = 0
        with self.lock:
            data_fd = os.open(self.data_path, os.O_RDWR)
            try:
                for namespace, rows in (('address', exact_rows), ('source', source_rows)):
                    for item in rows:
                        identity = item[0]
                        key = _key(identity) if namespace == 'address' else _source_key(identity)
                        pending_key = (namespace, key)
                        assignment = ((item[1], item[2]) if namespace == 'address'
                                      else (item[1],))
                        if pending_key in pending:
                            if pending[pending_key] != assignment:
                                if namespace == 'address':
                                    raise ValueError('exact_replay_address_reassigned')
                                raise ValueError('experience_source_lineage_split')
                            continue
                        selected = None
                        for slot_power in self.slot_powers:
                            map_key = (namespace, slot_power, key[0])
                            if map_key not in mappings:
                                mappings[map_key] = self._open_segment(
                                    key, create=True, namespace=namespace,
                                    slot_power=slot_power)
                                pending_slots[map_key] = {}
                                level_states[map_key] = list(self._level_state(
                                    mappings[map_key][1], slot_power))
                            fd, mapping = mappings[map_key]
                            try:
                                slot, exists = self._probe_pending(
                                    mapping, key, pending_slots[map_key], slot_power)
                            except ValueError as error:
                                if str(error) != 'exact_replay_address_segment_full':
                                    raise
                                level_states[map_key] = [1 << slot_power, True]
                                self._seal_level(fd, mapping, 1 << slot_power)
                                continue
                            count, sealed = level_states[map_key]
                            if not exists and sealed:
                                continue
                            if not exists:
                                count += 1
                                sealed = count >= self._level_threshold(slot_power)
                                level_states[map_key] = [count, sealed]
                                if sealed:
                                    # A crash after this barrier can only leave
                                    # this batch unindexed.  It cannot hide a
                                    # key already published in a higher level.
                                    self._seal_level(fd, mapping, count)
                            selected = (map_key, fd, mapping, slot, exists)
                            break
                        if selected is None:
                            raise ValueError('exact_replay_directory_capacity_exceeded')
                        map_key, fd, mapping, slot, exists = selected
                        if exists:
                            _, offset, length, stored_crc = SLOT.unpack(
                                mapping[slot:slot + SLOT.size])
                            body, _ = self._read_capsule(offset, length, stored_crc)
                            if namespace == 'address':
                                if (body.get('episode_id') != identity
                                        or body.get('shard') != item[1]
                                        or int(body.get('shard_row', -1)) != item[2]):
                                    raise ValueError('exact_replay_address_reassigned')
                            elif body.get('source') != identity or body.get('shard') != item[1]:
                                raise ValueError('experience_source_lineage_split')
                            pending[pending_key] = assignment
                            continue
                        payload = item[-1]
                        checksum = zlib.crc32(payload)
                        offset = os.lseek(data_fd, 0, os.SEEK_END)
                        _write_all(data_fd, CAPSULE.pack(len(payload), checksum))
                        _write_all(data_fd, payload)
                        staged.append((mapping, slot, key, offset, len(payload), checksum))
                        pending_slots[map_key][slot] = key
                        pending[pending_key] = assignment
                        if namespace == 'address':
                            added_exact += 1
                        else:
                            added_sources += 1
                os.fsync(data_fd)
                touched = {}
                for mapping, slot, key, offset, length, checksum in staged:
                    mapping[slot + 32:slot + SLOT.size] = struct.pack(
                        '<QII', offset, length, checksum)
                    mapping[slot:slot + 32] = key
                    touched[id(mapping)] = mapping
                for map_key, (count, sealed) in level_states.items():
                    mapping = mappings[map_key][1]
                    self._write_level_state(mapping, count, sealed)
                    touched[id(mapping)] = mapping
                for mapping in touched.values():
                    mapping.flush()
                for fd, _ in mappings.values():
                    os.fsync(fd)
                return dict(exact=added_exact, sources=added_sources)
            finally:
                os.close(data_fd)
                for fd, mapping in mappings.values():
                    mapping.close(); os.close(fd)

    def put_source(self, source, shard):
        """Bind an experience source to its one lineage-owning storage shard."""
        key = _source_key(source)
        shard = str(shard)
        payload = self._encode_source(source, shard)
        with self.lock:
            selected = None
            for slot_power in self.slot_powers:
                fd, mapping = self._open_segment(
                    key, create=True, namespace='source', slot_power=slot_power)
                try:
                    slot, exists = self._probe(mapping, key, slot_power)
                except ValueError as error:
                    if str(error) == 'exact_replay_address_segment_full':
                        self._seal_level(fd, mapping, 1 << slot_power)
                        mapping.close(); os.close(fd)
                        continue
                    mapping.close(); os.close(fd)
                    raise
                count, sealed = self._level_state(mapping, slot_power)
                if not exists and sealed:
                    mapping.close(); os.close(fd)
                    continue
                selected = (fd, mapping, slot, exists)
                break
            if selected is None:
                raise ValueError('exact_replay_directory_capacity_exceeded')
            fd, mapping, slot, exists = selected
            try:
                if exists:
                    _, offset, length, stored_crc = SLOT.unpack(mapping[slot:slot + SLOT.size])
                    body, _ = self._read_capsule(offset, length, stored_crc)
                    if body.get('source') != source:
                        raise ValueError('experience_source_hash_collision')
                    if body.get('shard') != shard:
                        raise ValueError('experience_source_lineage_split')
                    return False
                offset, length, checksum = self._append_capsule(payload)
                mapping[slot + 32:slot + SLOT.size] = struct.pack(
                    '<QII', offset, length, checksum)
                mapping[slot:slot + 32] = key
                mapping.flush()
                os.fsync(fd)
                count += 1
                sealed = count >= self._level_threshold(slot_power)
                self._write_level_state(mapping, count, sealed)
                mapping.flush(0, HEADER_BYTES)
                os.fsync(fd)
                return True
            finally:
                mapping.close(); os.close(fd)

    def _read_capsule(self, offset, length, checksum):
        header = os.pread(self.data_read_fd, CAPSULE.size, offset)
        if len(header) != CAPSULE.size:
            raise ValueError('exact_replay_capsule_truncated')
        stored_length, stored_crc = CAPSULE.unpack(header)
        if stored_length != length or stored_crc != checksum:
            raise ValueError('exact_replay_capsule_index_mismatch')
        payload = os.pread(self.data_read_fd, length, offset + CAPSULE.size)
        if len(payload) != length or zlib.crc32(payload) != checksum:
            raise ValueError('exact_replay_capsule_corrupt')
        if len(payload) < PAYLOAD.size:
            raise ValueError('exact_replay_capsule_corrupt')
        replay_length, cue_length = PAYLOAD.unpack(payload[:PAYLOAD.size])
        if PAYLOAD.size + replay_length + cue_length != len(payload):
            raise ValueError('exact_replay_capsule_corrupt')
        replay_payload = payload[PAYLOAD.size:PAYLOAD.size + replay_length]
        cue_payload = payload[PAYLOAD.size + replay_length:]
        try:
            return json.loads(replay_payload), cue_payload
        except (UnicodeDecodeError, json.JSONDecodeError):
            raise ValueError('exact_replay_capsule_corrupt') from None

    def _get_locked(self, identifier):
        key = _key(identifier)
        located = None
        for slot_power in self.slot_powers:
            mapping = self._read_segment(key, 'address', slot_power)
            if mapping is None:
                return None
            try:
                slot, exists = self._probe(mapping, key, slot_power)
            except ValueError as error:
                if str(error) == 'exact_replay_address_segment_full':
                    continue
                raise
            if not exists:
                if self._level_state(mapping, slot_power)[1]:
                    continue
                return None
            _, offset, length, checksum = SLOT.unpack(mapping[slot:slot + SLOT.size])
            located = (offset, length, checksum)
            break
        if located is None:
            return None
        offset, length, checksum = located
        body, cue_payload = self._read_capsule(offset, length, checksum)
        if body.get('episode_id') != identifier or body.get('schema') != 'swegca-vrs2-replay-capsule-v3':
            raise ValueError('exact_replay_capsule_identity_mismatch')
        row = body['steps'][0]
        step = MemoryStep(row['phase'], row['observation'], tuple(row['relations']), row['judgment'],
                          row['outcome'], tuple(row['evidence_refs']))
        replay = ReplayedEpisode(identifier, tuple(body['matched_cues']), (step,),
                                 tuple(body['source_addresses']), body['verification_state'])
        return dict(shard=body['shard'], revision=body['revision'], replay=replay,
                    shard_row=int(body['shard_row']),
                    cue_count=int(body['cue_count']),
                    cues=LazyCues(cue_payload, body['cue_count']), kind=body['kind'],
                    asks=body['asks'], description=body['description'])

    def get(self, identifier):
        with self.lock:
            return self._get_locked(identifier)

    def _source_shard_locked(self, source):
        key = _source_key(source)
        located = None
        for slot_power in self.slot_powers:
            mapping = self._read_segment(key, 'source', slot_power)
            if mapping is None:
                return None
            try:
                slot, exists = self._probe(mapping, key, slot_power)
            except ValueError as error:
                if str(error) == 'exact_replay_address_segment_full':
                    continue
                raise
            if not exists:
                if self._level_state(mapping, slot_power)[1]:
                    continue
                return None
            _, offset, length, checksum = SLOT.unpack(mapping[slot:slot + SLOT.size])
            located = (offset, length, checksum)
            break
        if located is None:
            return None
        offset, length, checksum = located
        body, cue_payload = self._read_capsule(offset, length, checksum)
        if cue_payload:
            raise ValueError('experience_source_route_identity_mismatch')
        if body.get('schema') != 'swegca-vrs2-source-route-v1' or body.get('source') != source:
            raise ValueError('experience_source_route_identity_mismatch')
        return body['shard']

    def source_shard(self, source):
        with self.lock:
            return self._source_shard_locked(source)

    def close(self):
        with self.lock:
            for fd, mapping in self.read_segments.values():
                mapping.close(); os.close(fd)
            self.read_segments.clear()
            if self.data_read_fd is not None:
                os.close(self.data_read_fd)
                self.data_read_fd = None

    def __del__(self):
        try:
            if getattr(self, 'data_read_fd', None) is not None:
                self.close()
        except (OSError, BufferError):
            pass

    def _proposition_path(self, proposition, *, create):
        if not isinstance(proposition, str) or not proposition:
            raise ValueError('invalid_experience_proposition')
        key = hashlib.sha256(proposition.encode('utf-8')).hexdigest()
        directory = self.proposition_directory / key[:2]
        if create:
            directory.mkdir(mode=0o700, parents=True, exist_ok=True)
        return directory / (key + '.json')

    def put_proposition(self, proposition, shard, identifier):
        """Bind one explicit claim to every shard carrying its VRS evidence."""
        path = self._proposition_path(proposition, create=True)
        with self.lock:
            if path.exists():
                try:
                    body = json.loads(path.read_text(encoding='utf-8'))
                except (OSError, ValueError, UnicodeError):
                    raise ValueError('experience_proposition_route_corrupt') from None
                if (body.get('schema') != 'swegca-vrs2-proposition-route-v1'
                        or body.get('proposition') != proposition):
                    raise ValueError('experience_proposition_hash_collision')
                experiences = {(str(item['shard']), str(item['episode_id']))
                               for item in body.get('experiences', ())}
                if (str(shard), identifier) in experiences:
                    return False
                experiences.add((str(shard), identifier))
            else:
                experiences = {(str(shard), identifier)}
            shards = sorted({item[0] for item in experiences})
            body = dict(schema='swegca-vrs2-proposition-route-v1', proposition=proposition,
                        shards=shards, experiences=[dict(shard=item[0], episode_id=item[1])
                                                   for item in sorted(experiences)])
            temporary = path.with_name('.' + path.name + '-' + os.urandom(8).hex())
            descriptor = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
            try:
                with os.fdopen(descriptor, 'w', encoding='utf-8') as stream:
                    json.dump(body, stream, ensure_ascii=False, sort_keys=True,
                              separators=(',', ':'))
                    stream.flush(); os.fsync(stream.fileno())
                os.replace(temporary, path)
                directory_fd = os.open(path.parent, os.O_RDONLY)
                try:
                    os.fsync(directory_fd)
                finally:
                    os.close(directory_fd)
            finally:
                temporary.unlink(missing_ok=True)
            return True

    def proposition_shards(self, proposition):
        path = self._proposition_path(proposition, create=False)
        try:
            body = json.loads(path.read_text(encoding='utf-8'))
        except FileNotFoundError:
            return ()
        except (OSError, ValueError, UnicodeError):
            raise ValueError('experience_proposition_route_corrupt') from None
        if (body.get('schema') != 'swegca-vrs2-proposition-route-v1'
                or body.get('proposition') != proposition):
            raise ValueError('experience_proposition_route_identity_mismatch')
        shards = body.get('shards')
        if not isinstance(shards, list) or any(not isinstance(item, str) for item in shards):
            raise ValueError('experience_proposition_route_corrupt')
        return tuple(shards)

    def proposition_experiences(self, proposition):
        path = self._proposition_path(proposition, create=False)
        try:
            body = json.loads(path.read_text(encoding='utf-8'))
        except FileNotFoundError:
            return ()
        except (OSError, ValueError, UnicodeError):
            raise ValueError('experience_proposition_route_corrupt') from None
        if (body.get('schema') != 'swegca-vrs2-proposition-route-v1'
                or body.get('proposition') != proposition):
            raise ValueError('experience_proposition_route_identity_mismatch')
        rows = body.get('experiences')
        if not isinstance(rows, list):
            raise ValueError('experience_proposition_route_corrupt')
        result = []
        for row in rows:
            if (not isinstance(row, dict) or not isinstance(row.get('shard'), str)
                    or not isinstance(row.get('episode_id'), str)):
                raise ValueError('experience_proposition_route_corrupt')
            _key(row['episode_id'])
            result.append((row['shard'], row['episode_id']))
        return tuple(result)

    def logical_bytes(self):
        return sum(path.stat().st_size for path in self.directory.rglob('*') if path.is_file())

    def allocated_bytes(self):
        return sum(path.stat().st_blocks * 512 for path in self.directory.rglob('*') if path.is_file())
