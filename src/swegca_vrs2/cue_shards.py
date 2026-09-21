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


MAGIC = b'VRS2CUES3\0'
HEADER_BYTES = 4096
SLOT_POWER = int(os.environ.get('VRS2_CUE_SLOT_POWER', '12'))
EMPTY = b'\0' * 32
LEVEL_STATE_OFFSET = 64
LEVEL_STATE = struct.Struct('<IB3x')  # published cues, sealed for new cues
LEVEL_LOAD_NUMERATOR = 7
LEVEL_LOAD_DENOMINATOR = 10
COPY_BODY = struct.Struct('<QII')            # head, count, generation
COPY = struct.Struct('<QIII')                # body + crc32
SLOT_BYTES = 80                              # key32, cue offset8, two copies20
CUE_HEADER = struct.Struct('<II')            # utf-8 bytes, crc32
NODE_BODY = struct.Struct('<QH126s32s')       # previous, shard bytes, shard, experience
NODE = struct.Struct('<QH126s32sI')           # body + crc32


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
        if not 1 <= self.slot_power <= 22:
            raise ValueError('cue_shard_slot_power_out_of_range')
        self.slot_count = 1 << self.slot_power
        self.mask = self.slot_count - 1
        self.slot_powers = tuple(range(self.slot_power, 23, 2))
        if self.slot_powers[-1] != 22:
            self.slot_powers = (*self.slot_powers, 22)
        self.table_path = self._table_path(self.slot_power)
        self.cues_path = self.directory / 'cues.vrs'
        self.postings_path = self.directory / 'postings.vrs'
        self.lock = threading.Lock()
        for path in (self.cues_path, self.postings_path):
            if not path.exists():
                fd = os.open(path, os.O_RDWR | os.O_CREAT | os.O_EXCL, 0o600)
                try:
                    _write_all(fd, MAGIC.ljust(HEADER_BYTES, b'\0'))
                    os.fsync(fd)
                finally:
                    os.close(fd)
            else:
                with path.open('rb') as stream:
                    if stream.read(len(MAGIC)) != MAGIC:
                        raise ValueError('cue_shard_data_header_invalid')
        table_fd, _ = self._open_table(self.slot_power, create=True)
        os.close(table_fd)

    def _table_path(self, slot_power):
        return self.directory / (f'table-p{slot_power}.vrs')

    @staticmethod
    def _shape(slot_power):
        segment_bytes = (1 << slot_power) * SLOT_BYTES
        stride = ((segment_bytes + mmap.ALLOCATIONGRANULARITY - 1)
                  // mmap.ALLOCATIONGRANULARITY * mmap.ALLOCATIONGRANULARITY)
        return segment_bytes, stride

    def _open_table(self, slot_power, *, create):
        path = self._table_path(slot_power)
        flags = os.O_RDWR | (os.O_CREAT if create else 0)
        try:
            fd = os.open(path, flags, 0o600)
        except FileNotFoundError:
            return None, None
        segment_bytes, stride = self._shape(slot_power)
        expected_size = HEADER_BYTES + 256 * stride
        size = os.fstat(fd).st_size
        if size == 0:
            if not create:
                os.close(fd); return None, None
            os.ftruncate(fd, expected_size)
            os.pwrite(fd, MAGIC + bytes((slot_power,)), 0)
            os.fsync(fd)
        elif (size != expected_size
              or os.pread(fd, len(MAGIC) + 1, 0) != MAGIC + bytes((slot_power,))):
            os.close(fd)
            raise ValueError('cue_shard_table_invalid')
        return fd, (segment_bytes, stride)

    def _map_segment(self, table_fd, key, *, write, slot_power=None):
        slot_power = self.slot_power if slot_power is None else int(slot_power)
        segment_bytes, stride = self._shape(slot_power)
        expected = HEADER_BYTES + 256 * stride
        if os.fstat(table_fd).st_size != expected:
            raise ValueError('cue_shard_table_invalid')
        offset = HEADER_BYTES + key[0] * stride
        access = mmap.ACCESS_WRITE if write else mmap.ACCESS_READ
        return mmap.mmap(table_fd, segment_bytes, access=access, offset=offset)

    def _probe(self, mapping, key, slot_power=None):
        slot_power = self.slot_power if slot_power is None else int(slot_power)
        slot_count, mask = 1 << slot_power, (1 << slot_power) - 1
        start = int.from_bytes(key[1:9], 'little') & mask
        step = (int.from_bytes(key[9:17], 'little') | 1) & mask or 1
        for count in range(slot_count):
            slot = (start + count * step) & mask
            offset = slot * SLOT_BYTES
            found = mapping[offset:offset + 32]
            if found == key:
                return offset, True
            if found == EMPTY:
                return offset, False
        raise ValueError('cue_shard_segment_full')

    def _probe_pending(self, mapping, key, pending, slot_power=None):
        """Probe while treating not-yet-published batch slots as occupied."""
        slot_power = self.slot_power if slot_power is None else int(slot_power)
        slot_count, mask = 1 << slot_power, (1 << slot_power) - 1
        start = int.from_bytes(key[1:9], 'little') & mask
        step = (int.from_bytes(key[9:17], 'little') | 1) & mask or 1
        for count in range(slot_count):
            slot = (start + count * step) & mask
            offset = slot * SLOT_BYTES
            found = pending.get(offset, mapping[offset:offset + 32])
            if found == key:
                return offset, True
            if found == EMPTY:
                return offset, False
        raise ValueError('cue_shard_segment_full')

    @staticmethod
    def _level_threshold(slot_power):
        return max(1, ((1 << slot_power) * LEVEL_LOAD_NUMERATOR
                       // LEVEL_LOAD_DENOMINATOR))

    @staticmethod
    def _level_state(table_fd, prefix, slot_power):
        offset = LEVEL_STATE_OFFSET + int(prefix) * LEVEL_STATE.size
        raw = os.pread(table_fd, LEVEL_STATE.size, offset)
        if len(raw) != LEVEL_STATE.size:
            raise ValueError('cue_shard_table_invalid')
        count, sealed = LEVEL_STATE.unpack(raw)
        if count > 1 << slot_power or sealed not in (0, 1):
            raise ValueError('cue_shard_table_invalid')
        return int(count), bool(sealed)

    @staticmethod
    def _write_level_state(table_fd, prefix, count, sealed):
        offset = LEVEL_STATE_OFFSET + int(prefix) * LEVEL_STATE.size
        raw = LEVEL_STATE.pack(int(count), bool(sealed))
        if os.pwrite(table_fd, raw, offset) != len(raw):
            raise OSError('cue_shard_table_state_write_failed')

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
    def _node_bytes(previous, shard, identifier):
        payload = str(shard).encode('utf-8')
        if not payload or len(payload) > 126:
            raise ValueError('invalid_vrs_shard_id')
        if (not isinstance(identifier, str) or not identifier.startswith('memory:')
                or len(identifier) != 71):
            raise ValueError('invalid_exact_experience_address')
        try:
            experience = bytes.fromhex(identifier[7:])
        except ValueError:
            raise ValueError('invalid_exact_experience_address') from None
        padded = payload.ljust(126, b'\0')
        body = NODE_BODY.pack(previous, len(payload), padded, experience)
        return NODE.pack(previous, len(payload), padded, experience, zlib.crc32(body))

    @staticmethod
    def _read_node(fd, offset):
        raw = os.pread(fd, NODE.size, offset)
        if len(raw) != NODE.size:
            raise ValueError('cue_shard_posting_truncated')
        previous, length, payload, experience, checksum = NODE.unpack(raw)
        if (not 1 <= length <= 126
                or zlib.crc32(NODE_BODY.pack(previous, length, payload, experience)) != checksum):
            raise ValueError('cue_shard_posting_corrupt')
        try:
            return previous, payload[:length].decode('utf-8'), 'memory:' + experience.hex()
        except UnicodeDecodeError:
            raise ValueError('cue_shard_posting_corrupt') from None

    def put_many(self, cues, shard, identifier):
        return self.put_records(((cues, shard, identifier),))

    def put_records(self, records):
        """Publish cue postings for many experiences with one durable barrier."""
        records = [(tuple(dict.fromkeys(str(cue) for cue in cues)), str(shard), identifier)
                   for cues, shard, identifier in records]
        added = 0
        with self.lock:
            cue_fd = os.open(self.cues_path, os.O_RDWR)
            posting_fd = os.open(self.postings_path, os.O_RDWR)
            table_fds, segments, pending_slots, level_states, virtual = {}, {}, {}, {}, {}
            cue_end = os.lseek(cue_fd, 0, os.SEEK_END)
            posting_end = os.lseek(posting_fd, 0, os.SEEK_END)
            cue_buffer, posting_buffer = bytearray(), bytearray()
            try:
                for cues, shard, identifier in records:
                    for cue in cues:
                        key = _key(cue); prefix = key[0]
                        selected = None
                        for slot_power in self.slot_powers:
                            if slot_power not in table_fds:
                                table_fds[slot_power] = self._open_table(
                                    slot_power, create=True)[0]
                            segment_key = (slot_power, prefix)
                            if segment_key not in segments:
                                segments[segment_key] = self._map_segment(
                                    table_fds[slot_power], key, write=True,
                                    slot_power=slot_power)
                                pending_slots[segment_key] = {}
                                level_states[segment_key] = list(self._level_state(
                                    table_fds[slot_power], prefix, slot_power))
                            mapping = segments[segment_key]
                            try:
                                slot, exists = self._probe_pending(
                                    mapping, key, pending_slots[segment_key],
                                    slot_power=slot_power)
                            except ValueError as error:
                                if str(error) != 'cue_shard_segment_full':
                                    raise
                                level_states[segment_key] = [1 << slot_power, True]
                                self._write_level_state(
                                    table_fds[slot_power], prefix, 1 << slot_power, True)
                                os.fsync(table_fds[slot_power])
                                continue
                            count, sealed = level_states[segment_key]
                            if not exists and sealed:
                                continue
                            if not exists:
                                count += 1
                                sealed = count >= self._level_threshold(slot_power)
                                level_states[segment_key] = [count, sealed]
                                if sealed:
                                    # Overflow routing is durable before a key
                                    # can be published in the following level.
                                    self._write_level_state(
                                        table_fds[slot_power], prefix, count, True)
                                    os.fsync(table_fds[slot_power])
                            selected = (slot_power, segment_key, mapping, slot, exists)
                            break
                        if selected is None:
                            raise ValueError('cue_shard_directory_capacity_exceeded')
                        slot_power, segment_key, mapping, slot, exists = selected
                        identity = (slot_power, prefix, slot)
                        state = virtual.get(identity)
                        if state is None:
                            stored = self._state(mapping, slot) if exists else None
                            if exists:
                                cue_offset = struct.unpack(
                                    '<Q', mapping[slot + 32:slot + 40])[0]
                                if self._read_cue(cue_fd, cue_offset) != cue:
                                    raise ValueError('cue_shard_digest_collision')
                            else:
                                payload = cue.encode('utf-8')
                                cue_offset = cue_end + len(cue_buffer)
                                cue_buffer.extend(CUE_HEADER.pack(
                                    len(payload), zlib.crc32(payload)))
                                cue_buffer.extend(payload)
                                pending_slots[segment_key][slot] = key
                            head, count, generation, active = (
                                (stored[0], stored[1], stored[2], stored[3])
                                if stored else (0, 0, 0, None))
                            state = dict(key=key, cue=cue, cue_offset=cue_offset,
                                         head=head, count=count, generation=generation,
                                         active=active, exists=exists, changed=False,
                                         last_identifier=(self._read_node(posting_fd, head)[2]
                                                          if head else None))
                            virtual[identity] = state
                        elif state['key'] != key or state['cue'] != cue:
                            raise ValueError('cue_shard_digest_collision')
                        if state['last_identifier'] == identifier:
                            continue
                        node_offset = posting_end + len(posting_buffer)
                        posting_buffer.extend(self._node_bytes(
                            state['head'], shard, identifier))
                        state.update(head=node_offset, count=state['count'] + 1,
                                     generation=state['generation'] + 1, changed=True,
                                     last_identifier=identifier)
                        added += 1
                _write_all(cue_fd, cue_buffer)
                _write_all(posting_fd, posting_buffer)
                os.fsync(cue_fd); os.fsync(posting_fd)
                for (slot_power, prefix, slot), state in virtual.items():
                    if not state['changed']:
                        continue
                    mapping = segments[(slot_power, prefix)]
                    copy = self._copy(state['head'], state['count'], state['generation'])
                    if not state['exists']:
                        mapping[slot + 32:slot + 40] = struct.pack(
                            '<Q', state['cue_offset'])
                        mapping[slot + 40:slot + 60] = copy
                        mapping[slot:slot + 32] = state['key']
                    else:
                        target = 1 if state['active'] == 0 else 0
                        start = slot + 40 + 20 * target
                        mapping[start:start + 20] = copy
                for mapping in segments.values():
                    mapping.flush()
                for (slot_power, prefix), (count, sealed) in level_states.items():
                    self._write_level_state(
                        table_fds[slot_power], prefix, count, sealed)
                for table_fd in table_fds.values():
                    os.fsync(table_fd)
                return added
            finally:
                for mapping in segments.values():
                    mapping.close()
                for table_fd in table_fds.values():
                    os.close(table_fd)
                os.close(posting_fd); os.close(cue_fd)

    def _matches_for_locked(self, cue):
        key = _key(cue)
        cue_fd = os.open(self.cues_path, os.O_RDONLY)
        posting_fd = os.open(self.postings_path, os.O_RDONLY)
        try:
            for slot_power in self.slot_powers:
                fd, _ = self._open_table(slot_power, create=False)
                if fd is None:
                    return ()
                mapping = self._map_segment(fd, key, write=False, slot_power=slot_power)
                try:
                    try:
                        slot, exists = self._probe(mapping, key, slot_power=slot_power)
                    except ValueError as error:
                        if str(error) != 'cue_shard_segment_full':
                            raise
                        continue
                    if not exists:
                        if self._level_state(fd, key[0], slot_power)[1]:
                            continue
                        return ()
                    cue_offset = struct.unpack('<Q', mapping[slot + 32:slot + 40])[0]
                    if self._read_cue(cue_fd, cue_offset) != cue:
                        raise ValueError('cue_shard_digest_collision')
                    state = self._state(mapping, slot)
                    if state is None:
                        raise ValueError('cue_shard_head_invalid')
                    offset, count = state[0], state[1]
                    matches = []
                    for _ in range(count):
                        offset, shard, identifier = self._read_node(posting_fd, offset)
                        matches.append((shard, identifier))
                        if not offset:
                            break
                    else:
                        if offset:
                            raise ValueError('cue_shard_posting_count_invalid')
                    if len(matches) != count or offset:
                        raise ValueError('cue_shard_posting_count_invalid')
                    return tuple(dict.fromkeys(matches))
                finally:
                    mapping.close(); os.close(fd)
            return ()
        finally:
            os.close(posting_fd); os.close(cue_fd)

    def matches_for(self, cue):
        with self.lock:
            return self._matches_for_locked(cue)

    def shards_for(self, cue):
        return tuple(dict.fromkeys(shard for shard, _ in self.matches_for(cue)))

    def logical_bytes(self):
        return sum(path.stat().st_size for path in self.directory.glob('*.vrs'))

    def allocated_bytes(self):
        return sum(path.stat().st_blocks * 512 for path in self.directory.glob('*.vrs'))
