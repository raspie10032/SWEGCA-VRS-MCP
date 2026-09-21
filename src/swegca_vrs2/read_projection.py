"""Memory-mapped read projection of one complete VRS shard generation.

The projection is derived from, and bound to, a full ``Main`` pair snapshot. It
does not replace consolidation or regions. It exposes the current per-experience
strength, region memberships, supersession and portal objects without loading
the shard's Python checkpoint. A mismatched pair is unusable.
"""
from __future__ import annotations

import json
import os
from pathlib import Path
import shutil
import threading
import uuid

import numpy as np

from .store import plain


SCHEMA = 'swegca-vrs2-read-projection-v1'


def _write_bytes(path, payload):
    descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    try:
        view = memoryview(payload)
        while view:
            written = os.write(descriptor, view)
            if written <= 0:
                raise OSError('read_projection_write_failed')
            view = view[written:]
        os.fsync(descriptor)
    finally:
        os.close(descriptor)


def _array(path, values, dtype):
    array = np.ascontiguousarray(values, dtype=dtype)
    _write_bytes(path, array.tobytes())
    return int(len(array))


def _json_bytes(value):
    return json.dumps(value, ensure_ascii=False, sort_keys=True,
                      separators=(',', ':'), allow_nan=False).encode('utf-8')


class ProjectionView:
    def __init__(self, root, manifest):
        self.root = Path(root)
        self.manifest = manifest
        self.shard = manifest['shard']
        self.pair_snapshot_id = manifest['pair_snapshot_id']
        self.graph_snapshot_id = manifest['graph_snapshot_id']
        self.stable_version_id = manifest.get('stable_version_id')
        self.record_count = int(manifest['record_count'])
        self.cue_total = int(manifest['cue_total'])
        self.ids = self._mapped('ids.bin', 'u1', (self.record_count, 32))
        self.strength = self._mapped('strength.f32', '<f4', (self.record_count,))
        self.region = self._mapped('region.i32', '<i4', (self.record_count,))
        self.superseded = self._mapped('superseded.bin', 'u1', (self.record_count, 32))
        self.member_ptr = self._mapped('member_ptr.u64', '<u8', (self.record_count + 1,))
        count = int(self.member_ptr[-1]) if self.record_count else 0
        self.member_region = self._mapped('member_region.i32', '<i4', (count,))
        self.member_weight = self._mapped('member_weight.f32', '<f4', (count,))
        self.portals = tuple(manifest.get('portals', ()))

    def _mapped(self, name, dtype, shape):
        if not int(np.prod(shape)):
            return np.empty(shape, dtype=dtype)
        return np.memmap(self.root / name, mode='r', dtype=dtype, shape=shape)

    def _row(self, identifier, row):
        row = int(row)
        if not 0 <= row < self.record_count:
            raise ValueError('read_projection_row_outside_generation')
        try:
            expected = bytes.fromhex(identifier[7:])
        except (ValueError, TypeError):
            raise ValueError('invalid_exact_experience_address') from None
        if self.ids[row].tobytes() != expected:
            raise ValueError('read_projection_experience_mismatch')
        return row

    def current(self, identifier, row):
        row = self._row(identifier, row)
        start, stop = int(self.member_ptr[row]), int(self.member_ptr[row + 1])
        superseded = self.superseded[row].tobytes()
        return dict(shard=self.shard, pair_snapshot_id=self.pair_snapshot_id,
            graph_snapshot_id=self.graph_snapshot_id,
            stable_version_id=self.stable_version_id,
            portals=self.portals,
            strength=float(self.strength[row]), region=int(self.region[row]),
            memberships=tuple((int(region), float(weight)) for region, weight in
                              zip(self.member_region[start:stop], self.member_weight[start:stop])),
            superseded_by=None if superseded == b'\0' * 32 else 'memory:' + superseded.hex())


class ReadProjectionStore:
    def __init__(self, shard_directory, shard):
        self.root = Path(shard_directory) / 'read-projection'
        self.root.mkdir(mode=0o700, parents=True, exist_ok=True)
        self.shard = str(shard)
        self.lock = threading.Lock()

    @property
    def current_path(self):
        return self.root / 'CURRENT'

    def write(self, owner):
        memory, graph, pair = owner.memory, owner.graph, owner.pair
        ids = tuple(memory._store['ids'][:memory.episode_count])
        strengths, regions, superseded = [], [], []
        member_ptr, member_region, member_weight = [0], [], []
        for identifier in ids:
            strengths.append(graph.strength(identifier))
            region = graph.region_of(identifier)
            regions.append(-1 if region is None else int(region))
            successor = memory.superseded.get(identifier)
            superseded.append(b'\0' * 32 if successor is None else bytes.fromhex(successor[7:]))
            memberships = graph.memberships_of(identifier)
            member_region.extend(int(item[0]) for item in memberships)
            member_weight.extend(float(item[1]) for item in memberships)
            member_ptr.append(len(member_region))
        stable = graph.stable
        portals = []
        for (left, right), portal in ((getattr(stable, 'portals', None) or {}).items()
                                      if stable is not None else ()):
            portals.append(dict(pair=[int(left), int(right)], value=plain(portal)))
        manifest = dict(schema=SCHEMA, shard=self.shard,
            pair_snapshot_id=pair.snapshot_id, graph_snapshot_id=graph.snapshot_id,
            stable_version_id=(stable.version_id if stable is not None else None),
            record_count=len(ids), cue_total=int(memory.cue_total), portals=portals)
        generation = self.root / ('.building-' + uuid.uuid4().hex)
        final = self.root / pair.snapshot_id
        generation.mkdir(mode=0o700)
        try:
            _write_bytes(generation / 'ids.bin', b''.join(bytes.fromhex(identifier[7:])
                                                          for identifier in ids))
            _array(generation / 'strength.f32', strengths, '<f4')
            _array(generation / 'region.i32', regions, '<i4')
            _write_bytes(generation / 'superseded.bin', b''.join(superseded))
            _array(generation / 'member_ptr.u64', member_ptr, '<u8')
            _array(generation / 'member_region.i32', member_region, '<i4')
            _array(generation / 'member_weight.f32', member_weight, '<f4')
            _write_bytes(generation / 'manifest.json', _json_bytes(manifest))
            directory_fd = os.open(generation, os.O_RDONLY)
            try:
                os.fsync(directory_fd)
            finally:
                os.close(directory_fd)
            with self.lock:
                if final.exists():
                    shutil.rmtree(final)
                os.replace(generation, final)
                temporary = self.root / ('.CURRENT-' + uuid.uuid4().hex)
                _write_bytes(temporary, pair.snapshot_id.encode('ascii'))
                os.replace(temporary, self.current_path)
                root_fd = os.open(self.root, os.O_RDONLY)
                try:
                    os.fsync(root_fd)
                finally:
                    os.close(root_fd)
                for path in self.root.iterdir():
                    if path.is_dir() and path != final and not path.name.startswith('.building-'):
                        shutil.rmtree(path)
            return dict(shard=self.shard, pair_snapshot_id=pair.snapshot_id,
                        records=len(ids), memberships=len(member_region), portals=len(portals))
        finally:
            if generation.exists():
                shutil.rmtree(generation)

    def open(self, expected_pair=None):
        try:
            generation = self.current_path.read_text(encoding='ascii').strip()
        except OSError:
            return None
        if expected_pair is not None and generation != expected_pair:
            raise ValueError('read_projection_pair_mismatch')
        root = self.root / generation
        try:
            manifest = json.loads((root / 'manifest.json').read_text(encoding='utf-8'))
        except (OSError, ValueError, UnicodeError):
            raise ValueError('read_projection_manifest_invalid') from None
        if (manifest.get('schema') != SCHEMA or manifest.get('shard') != self.shard
                or manifest.get('pair_snapshot_id') != generation):
            raise ValueError('read_projection_identity_mismatch')
        return ProjectionView(root, manifest)
