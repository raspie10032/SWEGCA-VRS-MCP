"""Native durable journal for complete VRS generations.

The journal is deliberately small and storage only.  VRS semantics remain in
``store.Main``: this module persists the exact request envelope, fingerprint
and resulting pair id without interpreting any of them.

Each append is one checksummed compressed frame.  A batch therefore becomes
durable as one frame, and an interrupted final frame is ignored and truncated
the next time the owner opens the store.  Immutable segment files and an
atomically replaced checkpoint avoid a database, compatibility reader, or
second transcript store.
"""
from __future__ import annotations

from dataclasses import dataclass
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import uuid
import zlib


STORE_SCHEMA = 'swegca-vrs2-native-store-v1'
MANIFEST = 'vrs-store.json'
JOURNAL = 'journal'
HEAD = 'head.vrsj'
CHECKPOINT = 'checkpoint.vrsc'
FILE_MAGIC = b'VRS2JNL1'
FRAME = struct.Struct('<Q')
DIGEST_BYTES = 32
MAX_FRAME_BYTES = 64 * 1024 * 1024
ROTATE_BYTES = 32 * 1024 * 1024
CHECKPOINT_MAGIC = b'VRS2CP1\0'


def _fsync_directory(path):
    if os.name == 'nt':
        return
    descriptor = os.open(path, os.O_RDONLY)
    try:
        os.fsync(descriptor)
    finally:
        os.close(descriptor)


def atomic_bytes(path, body, mode=0o600):
    path = Path(path)
    path.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
    temporary = path.with_name('.' + path.name + '-' + os.urandom(8).hex())
    descriptor = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_EXCL, mode)
    try:
        with os.fdopen(descriptor, 'wb') as stream:
            stream.write(body)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
        _fsync_directory(path.parent)
    finally:
        temporary.unlink(missing_ok=True)


def _canonical(value):
    return json.dumps(value, ensure_ascii=False, sort_keys=True,
                      separators=(',', ':'), allow_nan=False).encode('utf-8')


def is_native_store(directory):
    path = Path(directory) / MANIFEST
    if not path.is_file():
        return False
    try:
        value = json.loads(path.read_text(encoding='utf-8'))
    except (OSError, UnicodeError, ValueError):
        return False
    return value.get('schema') == STORE_SCHEMA and isinstance(value.get('identity'), str)


@dataclass(frozen=True)
class Checkpoint:
    seq: int
    pair: str
    blob: bytes


class NativeJournal:
    """One owner for a checksummed VRS journal and checkpoint."""

    def __init__(self, directory, *, create=False, writable=False):
        self.directory = Path(directory).expanduser().resolve()
        self.manifest_path = self.directory / MANIFEST
        self.journal_root = self.directory / JOURNAL
        self.writable = bool(writable)
        if not self.manifest_path.exists():
            if not create:
                raise ValueError('native_vrs_store_missing')
            self.directory.mkdir(mode=0o700, parents=True, exist_ok=True)
            generation = 'g-' + uuid.uuid4().hex
            manifest = dict(schema=STORE_SCHEMA, identity=str(uuid.uuid4()),
                            generation=generation)
            target = self.journal_root / generation
            target.mkdir(mode=0o700, parents=True, exist_ok=False)
            atomic_bytes(target / HEAD, FILE_MAGIC)
            atomic_bytes(self.manifest_path, _canonical(manifest))
        try:
            manifest = json.loads(self.manifest_path.read_text(encoding='utf-8'))
        except (OSError, UnicodeError, ValueError):
            raise ValueError('native_vrs_manifest_invalid') from None
        if manifest.get('schema') != STORE_SCHEMA \
                or not isinstance(manifest.get('identity'), str) \
                or not isinstance(manifest.get('generation'), str):
            raise ValueError('native_vrs_manifest_invalid')
        self.identity = manifest['identity']
        self.generation = manifest['generation']
        self.path = self.journal_root / self.generation
        if not self.path.is_dir():
            raise ValueError('native_vrs_generation_missing')
        self.head_path = self.path / HEAD
        if not self.head_path.exists():
            if not self.writable:
                raise ValueError('native_vrs_head_missing')
            atomic_bytes(self.head_path, FILE_MAGIC)
        self._head = None
        self._rows = None
        self._scan(repair=self.writable)

    @staticmethod
    def _encode_frame(rows):
        payload = zlib.compress(_canonical(dict(schema='swegca-vrs2-frame-v1', rows=rows)), 3)
        if len(payload) > MAX_FRAME_BYTES:
            raise ValueError('native_vrs_frame_too_large')
        return FRAME.pack(len(payload)) + payload + hashlib.sha256(payload).digest()

    @staticmethod
    def _read_file(path, *, repair=False):
        mode = 'r+b' if repair else 'rb'
        with open(path, mode) as stream:
            if stream.read(len(FILE_MAGIC)) != FILE_MAGIC:
                raise ValueError('native_vrs_journal_magic_invalid')
            valid_end = len(FILE_MAGIC)
            while True:
                header = stream.read(FRAME.size)
                if not header:
                    break
                if len(header) != FRAME.size:
                    if repair:
                        stream.truncate(valid_end)
                        stream.flush(); os.fsync(stream.fileno())
                        break
                    raise ValueError('native_vrs_journal_truncated')
                size = FRAME.unpack(header)[0]
                if size > MAX_FRAME_BYTES:
                    raise ValueError('native_vrs_frame_too_large')
                payload = stream.read(size)
                checksum = stream.read(DIGEST_BYTES)
                if len(payload) != size or len(checksum) != DIGEST_BYTES:
                    if repair:
                        stream.truncate(valid_end)
                        stream.flush(); os.fsync(stream.fileno())
                        break
                    raise ValueError('native_vrs_journal_truncated')
                if hashlib.sha256(payload).digest() != checksum:
                    raise ValueError('native_vrs_frame_integrity_failed')
                try:
                    frame = json.loads(zlib.decompress(payload).decode('utf-8'))
                except (UnicodeError, ValueError, zlib.error):
                    raise ValueError('native_vrs_frame_invalid') from None
                rows = frame.get('rows') if isinstance(frame, dict) else None
                if frame.get('schema') != 'swegca-vrs2-frame-v1' or not isinstance(rows, list):
                    raise ValueError('native_vrs_frame_invalid')
                for row in rows:
                    if not isinstance(row, list) or len(row) != 5:
                        raise ValueError('native_vrs_row_invalid')
                    yield tuple(row)
                valid_end = stream.tell()

    def _files(self):
        segments = sorted(self.path.glob('segment-*.vrsj'))
        return [*segments, self.head_path]

    def _scan(self, *, repair=False):
        head, count, expected = None, 0, 1
        for path in self._files():
            for row in self._read_file(path, repair=repair and path == self.head_path):
                seq = row[0]
                if type(seq) is not int or seq != expected:
                    raise ValueError('native_vrs_sequence_invalid')
                expected += 1
                count += 1
                head = (seq, row[4])
        self._head, self._rows = head, count
        return head

    def refresh_head(self):
        return self._scan(repair=False)

    def head(self):
        return self._head

    @property
    def row_count(self):
        return self._rows

    def rows(self, after=0, upto=None):
        after = int(after)
        upto = None if upto is None else int(upto)
        for path in self._files():
            for row in self._read_file(path):
                seq = int(row[0])
                if seq <= after:
                    continue
                if upto is not None and seq > upto:
                    return
                yield row

    def pair(self, seq):
        for row in self.rows(int(seq) - 1, int(seq)):
            return row[4]
        return None

    def append(self, rows):
        """Append [(request_id, body, fingerprint, pair), ...] as one frame."""
        if not self.writable:
            raise ValueError('native_vrs_journal_read_only')
        rows = list(rows)
        if not rows:
            return []
        first = 1 if self._head is None else int(self._head[0]) + 1
        framed = [[first + index, *row] for index, row in enumerate(rows)]
        body = self._encode_frame(framed)
        start = self.head_path.stat().st_size
        try:
            with open(self.head_path, 'ab', buffering=0) as stream:
                stream.write(body)
                os.fsync(stream.fileno())
        except BaseException:
            with open(self.head_path, 'r+b', buffering=0) as stream:
                stream.truncate(start)
                os.fsync(stream.fileno())
            raise
        self._head = (framed[-1][0], framed[-1][4])
        self._rows += len(framed)
        if self.head_path.stat().st_size >= ROTATE_BYTES:
            self.rotate()
        return [row[0] for row in framed]

    def rotate(self):
        if not self.writable or self.head_path.stat().st_size <= len(FILE_MAGIC):
            return False
        head = self._head[0] if self._head else 0
        target = self.path / f'segment-{head:020d}.vrsj'
        os.replace(self.head_path, target)
        atomic_bytes(self.head_path, FILE_MAGIC)
        _fsync_directory(self.path)
        return True

    def read_checkpoint(self):
        path = self.directory / CHECKPOINT
        if not path.is_file():
            return None
        body = path.read_bytes()
        if not body.startswith(CHECKPOINT_MAGIC) or len(body) < len(CHECKPOINT_MAGIC) + 8 + 2 + DIGEST_BYTES:
            raise ValueError('native_vrs_checkpoint_invalid')
        at = len(CHECKPOINT_MAGIC)
        meta_size = FRAME.unpack(body[at:at + FRAME.size])[0]
        at += FRAME.size
        meta_body = body[at:at + meta_size]
        at += meta_size
        blob = body[at:-DIGEST_BYTES]
        if hashlib.sha256(body[:-DIGEST_BYTES]).digest() != body[-DIGEST_BYTES:]:
            raise ValueError('native_vrs_checkpoint_integrity_failed')
        try:
            meta = json.loads(meta_body.decode('utf-8'))
        except (UnicodeError, ValueError):
            raise ValueError('native_vrs_checkpoint_invalid') from None
        if meta.get('schema') != 'swegca-vrs2-checkpoint-v1' \
                or meta.get('identity') != self.identity:
            raise ValueError('native_vrs_checkpoint_identity_mismatch')
        return Checkpoint(int(meta['seq']), str(meta['pair']), blob)

    def write_checkpoint(self, seq, pair, blob):
        if not self.writable:
            raise ValueError('native_vrs_journal_read_only')
        meta = _canonical(dict(schema='swegca-vrs2-checkpoint-v1', identity=self.identity,
                               seq=int(seq), pair=str(pair)))
        body = CHECKPOINT_MAGIC + FRAME.pack(len(meta)) + meta + bytes(blob)
        atomic_bytes(self.directory / CHECKPOINT, body + hashlib.sha256(body).digest())

    def remove_checkpoint(self):
        (self.directory / CHECKPOINT).unlink(missing_ok=True)
        _fsync_directory(self.directory)

    def rewrite(self, rows):
        """Atomically replace the complete journal generation."""
        if not self.writable:
            raise ValueError('native_vrs_journal_read_only')
        generation = 'g-' + uuid.uuid4().hex
        target = self.journal_root / generation
        target.mkdir(mode=0o700, parents=True, exist_ok=False)
        try:
            head = target / HEAD
            with open(head, 'wb', buffering=0) as stream:
                stream.write(FILE_MAGIC)
                batch = []
                for row in rows:
                    batch.append(list(row))
                    if len(batch) >= 512:
                        stream.write(self._encode_frame(batch))
                        batch.clear()
                if batch:
                    stream.write(self._encode_frame(batch))
                os.fsync(stream.fileno())
            manifest = dict(schema=STORE_SCHEMA, identity=self.identity, generation=generation)
            old = self.path
            atomic_bytes(self.manifest_path, _canonical(manifest))
            self.generation, self.path = generation, target
            self.head_path = target / HEAD
            self._scan(repair=False)
            shutil.rmtree(old)
        except BaseException:
            if self.path != target:
                shutil.rmtree(target, ignore_errors=True)
            raise

    def stats(self):
        files = self._files()
        checkpoint = self.directory / CHECKPOINT
        return dict(rows=self._rows, segments=max(0, len(files) - 1),
                    disk_bytes=sum(path.stat().st_size for path in files)
                    + (checkpoint.stat().st_size if checkpoint.exists() else 0))

    def close(self):
        return None
