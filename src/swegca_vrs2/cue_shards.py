"""Disk-resident cue -> VRS shard directory.

The main is split for storage, but lexical Déjà vu must not scan or load every
shard.  This directory preserves the existing cue set and maps each cue to the
complete VRS shards that own matching original experiences.  It is an index of
VRS experience addresses, never a log or a replacement judgment path.

Each hash-prefix segment is a sparse fixed-slot table.  A slot keeps two
checksummed head pointers, so an interrupted update leaves the previous chain
readable.  Posting nodes are append-only and name shards; repeated records in
the current shard do not add another node.  Full cue bytes are stored and
verified, so a digest collision fails closed.
"""
from __future__ import annotations

import hashlib
import mmap
import os
from pathlib import Path
import struct
import threading
import zlib


MAGIC = b'VRS2CUES1\0'
HEADER_BYTES = 4096
SLOT_POWER = int(os.environ.get('VRS2_CUE_SLOT_POWER', '16'))
EMPTY = b'\0' * 32
COPY_BODY = struct.Struct('<QII')            # head, count, generation
COPY = struct.Struct('<QIII')                # body + crc32
SLOT_BYTES = 80                              # key32, cue offset8, two copies20
CUE_HEADER = struct.Struct('<II')            # utf-8 bytes, crc32
NODE_BODY = struct.Struct('<QH126s')          # previous, shard bytes, shard
NODE = struct.Struct('<QH126sI')              # body + crc32


def _write_all(fd, data):
    view = memoryview(data)
    while view:
        written = os.write(fd, view)
        if written <= 0:
            raise OSError('cue_shard_directory_write_failed')
        view = view[written:]


def _key(cue):
    if not isinstance(cue, str) or not cue or len(cue.encode('utf-8')) > 4096:
        raise ValueError('invalid_vrs_cue')
    value = hashlib.sha256(cue.encode('utf-8')).digest()
    if value == EMPTY:
        raise ValueError('reserved_vrs_cue_digest')
    return value


class CueShardDirectory:
    def __init__(self, directory, *, slot_power=None):
        self.directory = Path(directory)
        self.directory.mkdir(mode=0o700, parents=True, exist_ok=True)
        self.slot_power = SLOT_POWER if slot_power is None else int(slot_power)
        self.slot_count = 1 << self.slot_power
        self.mask = self.slot_count - 1
        self.segment_bytes = self.slot_count * SLOT_BYTES
        self.segment_stride = ((self.segment_bytes + mmap.ALLOCATIONGRANULARITY - 1)
                               // mmap.ALLOCATIONGRANULARITY * mmap.ALLOCATIONGRANULARITY)
        self.table_path = self.directory / 'table.vrs'
        self.cues_path = self.directory / 'cues.vrs'
        self.postings_path = self.directory / 'postings.vrs'
        self.lock = threading.Lock()
        for path in (self.table_path, self.cues_path, self.postings_path):
            if not path.exists():
                fd = os.open(path, os.O_RDWR | os.O_CREAT | os.O_EXCL, 0o600)
                try:
                    _write_all(fd, MAGIC.ljust(HEADER_BYTES, b'\0'))
                    if path == self.table_path:
                        os.ftruncate(fd, HEADER_BYTES + 256 * self.segment_stride)
                        os.pwrite(fd, MAGIC + bytes((self.slot_power,)), 0)
                    os.fsync(fd)
                finally:
                    os.close(fd)
            else:
                with path.open('rb') as stream:
                    header = stream.read(len(MAGIC) + (1 if path == self.table_path else 0))
                    expected = MAGIC + (bytes((self.slot_power,)) if path == self.table_path else b'')
                    if header != expected:
                        raise ValueError('cue_shard_data_header_invalid')

    def _map_segment(self, table_fd, key, *, write):
        expected = HEADER_BYTES + 256 * self.segment_stride
        if os.fstat(table_fd).st_size != expected:
            raise ValueError('cue_shard_table_invalid')
        offset = HEADER_BYTES + key[0] * self.segment_stride
        access = mmap.ACCESS_WRITE if write else mmap.ACCESS_READ
        return mmap.mmap(table_fd, self.segment_bytes, access=access, offset=offset)

    def _probe(self, mapping, key):
        start = int.from_bytes(key[1:9], 'little') & self.mask
        step = (int.from_bytes(key[9:17], 'little') | 1) & self.mask or 1
        for count in range(self.slot_count):
            slot = (start + count * step) & self.mask
            offset = slot * SLOT_BYTES
            found = mapping[offset:offset + 32]
            if found == key:
                return offset, True
            if found == EMPTY:
                return offset, False
        raise ValueError('cue_shard_segment_full')

    def _probe_pending(self, mapping, key, pending):
        """Probe while treating not-yet-published batch slots as occupied."""
        start = int.from_bytes(key[1:9], 'little') & self.mask
        step = (int.from_bytes(key[9:17], 'little') | 1) & self.mask or 1
        for count in range(self.slot_count):
            slot = (start + count * step) & self.mask
            offset = slot * SLOT_BYTES
            found = pending.get(offset, mapping[offset:offset + 32])
            if found == key:
                return offset, True
            if found == EMPTY:
                return offset, False
        raise ValueError('cue_shard_segment_full')

    @staticmethod
    def _copy(head, count, generation):
        body = COPY_BODY.pack(head, count, generation)
        return COPY.pack(head, count, generation, zlib.crc32(body))

    @staticmethod
    def _valid_copy(raw, index):
        head, count, generation, checksum = COPY.unpack(raw)
        if not head and not count and not generation and not checksum:
            return None
        if zlib.crc32(COPY_BODY.pack(head, count, generation)) != checksum:
            return None
        if head < HEADER_BYTES or count < 1 or generation < 1:
            return None
        return head, count, generation, index

    def _state(self, mapping, slot):
        copies = [self._valid_copy(mapping[slot + 40:slot + 60], 0),
                  self._valid_copy(mapping[slot + 60:slot + 80], 1)]
        valid = [item for item in copies if item is not None]
        return max(valid, key=lambda item: item[2]) if valid else None

    @staticmethod
    def _append_cue(fd, cue):
        payload = cue.encode('utf-8')
        offset = os.lseek(fd, 0, os.SEEK_END)
        _write_all(fd, CUE_HEADER.pack(len(payload), zlib.crc32(payload)))
        _write_all(fd, payload)
        return offset

    @staticmethod
    def _read_cue(fd, offset):
        header = os.pread(fd, CUE_HEADER.size, offset)
        if len(header) != CUE_HEADER.size:
            raise ValueError('cue_shard_cue_truncated')
        length, checksum = CUE_HEADER.unpack(header)
        if length > 4096:
            raise ValueError('cue_shard_cue_invalid')
        payload = os.pread(fd, length, offset + CUE_HEADER.size)
        if len(payload) != length or zlib.crc32(payload) != checksum:
            raise ValueError('cue_shard_cue_corrupt')
        try:
            return payload.decode('utf-8')
        except UnicodeDecodeError:
            raise ValueError('cue_shard_cue_corrupt') from None

    @staticmethod
    def _node_bytes(previous, shard):
        payload = str(shard).encode('utf-8')
        if not payload or len(payload) > 126:
            raise ValueError('invalid_vrs_shard_id')
        padded = payload.ljust(126, b'\0')
        body = NODE_BODY.pack(previous, len(payload), padded)
        return NODE.pack(previous, len(payload), padded, zlib.crc32(body))

    @staticmethod
    def _read_node(fd, offset):
        raw = os.pread(fd, NODE.size, offset)
        if len(raw) != NODE.size:
            raise ValueError('cue_shard_posting_truncated')
        previous, length, payload, checksum = NODE.unpack(raw)
        if (not 1 <= length <= 126
                or zlib.crc32(NODE_BODY.pack(previous, length, payload)) != checksum):
            raise ValueError('cue_shard_posting_corrupt')
        try:
            return previous, payload[:length].decode('utf-8')
        except UnicodeDecodeError:
            raise ValueError('cue_shard_posting_corrupt') from None

    def put_many(self, cues, shard):
        cues = tuple(dict.fromkeys(str(cue) for cue in cues))
        if not cues:
            return 0
        added = 0
        with self.lock:
            cue_fd = os.open(self.cues_path, os.O_RDWR)
            posting_fd = os.open(self.postings_path, os.O_RDWR)
            table_fd = os.open(self.table_path, os.O_RDWR)
            segments = {}
            pending = {}
            updates = []
            try:
                for cue in cues:
                    key = _key(cue)
                    prefix = key[0]
                    if prefix not in segments:
                        segments[prefix] = self._map_segment(table_fd, key, write=True)
                        pending[prefix] = {}
                    mapping = segments[prefix]
                    slot, exists = self._probe_pending(mapping, key, pending[prefix])
                    state = self._state(mapping, slot) if exists else None
                    if exists:
                        cue_offset = struct.unpack('<Q', mapping[slot + 32:slot + 40])[0]
                        if self._read_cue(cue_fd, cue_offset) != cue:
                            raise ValueError('cue_shard_digest_collision')
                    else:
                        cue_offset = self._append_cue(cue_fd, cue)
                    previous, count, generation = (state[:3] if state else (0, 0, 0))
                    if state and self._read_node(posting_fd, previous)[1] == str(shard):
                        continue
                    node_offset = os.lseek(posting_fd, 0, os.SEEK_END)
                    _write_all(posting_fd, self._node_bytes(previous, shard))
                    updates.append((mapping, slot, exists, cue_offset,
                                    node_offset, count + 1, generation + 1,
                                    None if state is None else state[3]))
                    if not exists:
                        pending[prefix][slot] = key
                    added += 1
                os.fsync(cue_fd); os.fsync(posting_fd)
                touched = {}
                for mapping, slot, exists, cue_offset, head, count, generation, active in updates:
                    if not exists:
                        mapping[slot + 32:slot + 40] = struct.pack('<Q', cue_offset)
                        mapping[slot + 40:slot + 60] = self._copy(head, count, generation)
                        mapping[slot:slot + 32] = hashlib.sha256(
                            self._read_cue(cue_fd, cue_offset).encode('utf-8')).digest()
                    else:
                        target = 1 if active == 0 else 0
                        start = slot + 40 + 20 * target
                        mapping[start:start + 20] = self._copy(head, count, generation)
                    touched[id(mapping)] = mapping
                for mapping in touched.values():
                    mapping.flush()
                os.fsync(table_fd)
                return added
            finally:
                for mapping in segments.values():
                    mapping.close()
                os.close(table_fd); os.close(posting_fd); os.close(cue_fd)

    def shards_for(self, cue):
        key = _key(cue)
        fd = os.open(self.table_path, os.O_RDONLY)
        mapping = self._map_segment(fd, key, write=False)
        cue_fd = os.open(self.cues_path, os.O_RDONLY)
        posting_fd = os.open(self.postings_path, os.O_RDONLY)
        try:
            slot, exists = self._probe(mapping, key)
            if not exists:
                return ()
            cue_offset = struct.unpack('<Q', mapping[slot + 32:slot + 40])[0]
            if self._read_cue(cue_fd, cue_offset) != cue:
                raise ValueError('cue_shard_digest_collision')
            state = self._state(mapping, slot)
            if state is None:
                raise ValueError('cue_shard_head_invalid')
            offset, count = state[0], state[1]
            shards = []
            for _ in range(count):
                offset, shard = self._read_node(posting_fd, offset)
                shards.append(shard)
                if not offset:
                    break
            else:
                if offset:
                    raise ValueError('cue_shard_posting_count_invalid')
            if len(shards) != count or offset:
                raise ValueError('cue_shard_posting_count_invalid')
            return tuple(dict.fromkeys(shards))
        finally:
            os.close(posting_fd); os.close(cue_fd)
            mapping.close(); os.close(fd)

    def logical_bytes(self):
        return sum(path.stat().st_size for path in self.directory.glob('*.vrs'))

    def allocated_bytes(self):
        return sum(path.stat().st_blocks * 512 for path in self.directory.glob('*.vrs'))
