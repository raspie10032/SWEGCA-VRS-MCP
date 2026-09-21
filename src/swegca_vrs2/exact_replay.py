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

MAGIC = b'VRS2EXACT1\0'
HEADER_BYTES = 4096
SEGMENT_BITS = 8
SLOT_POWER = int(os.environ.get('VRS2_EXACT_SLOT_POWER', '23'))
SLOT_COUNT = 1 << SLOT_POWER
SLOT = struct.Struct('<32sQII')       # key, capsule offset, compressed bytes, CRC32
CAPSULE = struct.Struct('<II')        # compressed bytes, CRC32
EMPTY = b'\0' * 32


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


class ExactReplayStore:
    def __init__(self, directory, *, slot_power=None):
        self.directory = Path(directory)
        self.directory.mkdir(mode=0o700, parents=True, exist_ok=True)
        self.slot_power = SLOT_POWER if slot_power is None else int(slot_power)
        self.slot_count = 1 << self.slot_power
        self.mask = self.slot_count - 1
        self.data_path = self.directory / 'capsules.vrs'
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

    def _segment_path(self, key, namespace):
        return self.directory / f'{namespace}-{key[0]:02x}.vrs'

    def _open_segment(self, key, *, create, namespace='address'):
        path = self._segment_path(key, namespace)
        flags = os.O_RDWR | (os.O_CREAT if create else 0)
        try:
            fd = os.open(path, flags, 0o600)
        except FileNotFoundError:
            return None, None
        expected = HEADER_BYTES + self.slot_count * SLOT.size
        size = os.fstat(fd).st_size
        if size == 0:
            if not create:
                os.close(fd); return None, None
            os.ftruncate(fd, expected)
            os.pwrite(fd, MAGIC + bytes((key[0],)) + bytes((self.slot_power,)), 0)
            os.fsync(fd)
        elif size != expected or os.pread(fd, len(MAGIC), 0) != MAGIC:
            os.close(fd)
            raise ValueError('exact_replay_address_segment_invalid')
        return fd, mmap.mmap(fd, expected, access=mmap.ACCESS_WRITE if create else mmap.ACCESS_READ)

    def _probe(self, mapping, key):
        start = int.from_bytes(key[1:9], 'little') & self.mask
        step = (int.from_bytes(key[9:17], 'little') | 1) & self.mask
        step = step or 1
        for count in range(self.slot_count):
            slot = (start + count * step) & self.mask
            offset = HEADER_BYTES + slot * SLOT.size
            found = mapping[offset:offset + 32]
            if found == key:
                return offset, True
            if found == EMPTY:
                return offset, False
        raise ValueError('exact_replay_address_segment_full')

    @staticmethod
    def _encode(identifier, shard, episode):
        step = episode.steps[0]
        body = dict(schema='swegca-vrs2-replay-capsule-v1', episode_id=identifier,
                    shard=str(shard), matched_cues=[identifier],
                    steps=[dict(phase=step.phase, observation=plain(step.observation),
                                relations=list(step.relations), judgment=step.judgment,
                                outcome=step.outcome, evidence_refs=list(step.evidence_refs))],
                    source_addresses=list(episode.source_addresses), revision=episode.revision,
                    verification_state=episode.verification_state,
                    historical_truth_authorized=False)
        raw = json.dumps(body, ensure_ascii=False, sort_keys=True,
                         separators=(',', ':'), allow_nan=False).encode('utf-8')
        return zlib.compress(raw, 3)

    @staticmethod
    def _encode_source(source, shard):
        raw = json.dumps(dict(schema='swegca-vrs2-source-route-v1', source=source,
                              shard=str(shard)), ensure_ascii=False, sort_keys=True,
                         separators=(',', ':'), allow_nan=False).encode('utf-8')
        return zlib.compress(raw, 3)

    def _append_capsule(self, compressed):
        checksum = zlib.crc32(compressed)
        data_fd = os.open(self.data_path, os.O_RDWR)
        try:
            offset = os.lseek(data_fd, 0, os.SEEK_END)
            _write_all(data_fd, CAPSULE.pack(len(compressed), checksum))
            _write_all(data_fd, compressed)
            os.fsync(data_fd)
        finally:
            os.close(data_fd)
        return offset, len(compressed), checksum

    def put(self, identifier, shard, episode):
        key = _key(identifier)
        compressed = self._encode(identifier, shard, episode)
        checksum = zlib.crc32(compressed)
        with self.lock:
            fd, mapping = self._open_segment(key, create=True)
            try:
                slot, exists = self._probe(mapping, key)
                if exists:
                    _, offset, length, stored_crc = SLOT.unpack(mapping[slot:slot + SLOT.size])
                    body = self._read_capsule(offset, length, stored_crc)
                    if body['episode_id'] != identifier:
                        raise ValueError('exact_replay_address_collision')
                    if body.get('shard') != str(shard):
                        raise ValueError('exact_replay_address_reassigned')
                    return False
                offset, length, checksum = self._append_capsule(compressed)
                # Tail first, key last: a reader never treats a partially written
                # slot as committed.  CRC verifies both index and capsule.
                mapping[slot + 32:slot + SLOT.size] = struct.pack('<QII', offset, length, checksum)
                mapping[slot:slot + 32] = key
                mapping.flush()
                os.fsync(fd)
                return True
            finally:
                mapping.close(); os.close(fd)

    def put_source(self, source, shard):
        """Bind an experience source to its one lineage-owning storage shard."""
        key = _source_key(source)
        compressed = self._encode_source(source, shard)
        with self.lock:
            fd, mapping = self._open_segment(key, create=True, namespace='source')
            try:
                slot, exists = self._probe(mapping, key)
                if exists:
                    _, offset, length, stored_crc = SLOT.unpack(mapping[slot:slot + SLOT.size])
                    body = self._read_capsule(offset, length, stored_crc)
                    if body.get('source') != source:
                        raise ValueError('experience_source_hash_collision')
                    if body.get('shard') != str(shard):
                        raise ValueError('experience_source_lineage_split')
                    return False
                offset, length, checksum = self._append_capsule(compressed)
                mapping[slot + 32:slot + SLOT.size] = struct.pack(
                    '<QII', offset, length, checksum)
                mapping[slot:slot + 32] = key
                mapping.flush()
                os.fsync(fd)
                return True
            finally:
                mapping.close(); os.close(fd)

    def _read_capsule(self, offset, length, checksum):
        with self.data_path.open('rb', buffering=0) as stream:
            stream.seek(offset)
            header = stream.read(CAPSULE.size)
            if len(header) != CAPSULE.size:
                raise ValueError('exact_replay_capsule_truncated')
            stored_length, stored_crc = CAPSULE.unpack(header)
            if stored_length != length or stored_crc != checksum:
                raise ValueError('exact_replay_capsule_index_mismatch')
            compressed = stream.read(length)
        if len(compressed) != length or zlib.crc32(compressed) != checksum:
            raise ValueError('exact_replay_capsule_corrupt')
        return json.loads(zlib.decompress(compressed).decode('utf-8'))

    def get(self, identifier):
        key = _key(identifier)
        fd, mapping = self._open_segment(key, create=False)
        if mapping is None:
            return None
        try:
            slot, exists = self._probe(mapping, key)
            if not exists:
                return None
            _, offset, length, checksum = SLOT.unpack(mapping[slot:slot + SLOT.size])
        finally:
            mapping.close(); os.close(fd)
        body = self._read_capsule(offset, length, checksum)
        if body.get('episode_id') != identifier or body.get('schema') != 'swegca-vrs2-replay-capsule-v1':
            raise ValueError('exact_replay_capsule_identity_mismatch')
        row = body['steps'][0]
        step = MemoryStep(row['phase'], row['observation'], tuple(row['relations']), row['judgment'],
                          row['outcome'], tuple(row['evidence_refs']))
        replay = ReplayedEpisode(identifier, tuple(body['matched_cues']), (step,),
                                 tuple(body['source_addresses']), body['verification_state'])
        return dict(shard=body['shard'], revision=body['revision'], replay=replay)

    def source_shard(self, source):
        key = _source_key(source)
        fd, mapping = self._open_segment(key, create=False, namespace='source')
        if mapping is None:
            return None
        try:
            slot, exists = self._probe(mapping, key)
            if not exists:
                return None
            _, offset, length, checksum = SLOT.unpack(mapping[slot:slot + SLOT.size])
        finally:
            mapping.close(); os.close(fd)
        body = self._read_capsule(offset, length, checksum)
        if body.get('schema') != 'swegca-vrs2-source-route-v1' or body.get('source') != source:
            raise ValueError('experience_source_route_identity_mismatch')
        return body['shard']

    def logical_bytes(self):
        return sum(path.stat().st_size for path in self.directory.glob('*.vrs'))

    def allocated_bytes(self):
        return sum(path.stat().st_blocks * 512 for path in self.directory.glob('*.vrs'))
