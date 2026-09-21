"""Durable ownership registry for complete ended-session VRS shards.

An ended session is already a complete VRS.  Main adopts that VRS in place;
it never exports and re-ingests the observations.  The registry contains only
location and generation facts needed to open the original experience store.
"""
from __future__ import annotations

import json
import os
from pathlib import Path
import shutil

from .native_lock import FileLock, Timeout

from .loopback import LoopbackClient, port_of
from .native_transport import InterfaceError
from .native_journal import is_native_store


SCHEMA = 'swegca-vrs2-linked-shards-v1'
REGISTRY = 'linked-shards.json'


def _atomic_json(path, value):
    path = Path(path)
    path.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
    body = json.dumps(value, ensure_ascii=False, sort_keys=True,
                      separators=(',', ':')).encode('utf-8')
    temporary = path.with_name('.' + path.name + '-' + os.urandom(8).hex())
    descriptor = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    try:
        with os.fdopen(descriptor, 'wb') as stream:
            stream.write(body)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
        if os.name != 'nt':
            descriptor = os.open(path.parent, os.O_RDONLY)
            try:
                os.fsync(descriptor)
            finally:
                os.close(descriptor)
    finally:
        temporary.unlink(missing_ok=True)


def _inside(child, parent):
    try:
        child.relative_to(parent)
        return True
    except ValueError:
        return False


def _validate(root, row):
    if not isinstance(row, dict):
        raise ValueError('linked_shard_invalid')
    identifier = row.get('id')
    if (not isinstance(identifier, str) or not identifier.startswith('session-')
            or len(identifier.encode('utf-8')) > 126):
        raise ValueError('linked_shard_id_invalid')
    relative = row.get('path')
    if not isinstance(relative, str) or not relative:
        raise ValueError('linked_shard_path_invalid')
    directory = (root / relative).resolve()
    session_root = (root / 'session-vrs').resolve()
    if not _inside(directory, session_root) or not is_native_store(directory):
        raise ValueError('linked_shard_path_invalid')
    if os.name != 'nt' and directory.stat().st_uid != os.getuid():
        raise ValueError('linked_shard_not_owned')
    pair = row.get('pair_snapshot_id')
    if not isinstance(pair, str) or not pair:
        raise ValueError('linked_shard_pair_invalid')
    records, cues = row.get('records'), row.get('cue_total')
    if not isinstance(records, int) or records < 0 or not isinstance(cues, int) or cues < 0:
        raise ValueError('linked_shard_counts_invalid')
    return dict(id=identifier, path=relative, directory=directory,
                pair_snapshot_id=pair, records=records, cue_total=cues)


def load_linked_shards(root):
    """Return registry order as id -> validated generation metadata."""
    root = Path(root).expanduser().resolve()
    path = root / REGISTRY
    if not path.exists():
        return {}
    try:
        body = json.loads(path.read_text(encoding='utf-8'))
    except (OSError, UnicodeError, ValueError):
        raise ValueError('linked_shard_registry_invalid') from None
    if not isinstance(body, dict) or body.get('schema') != SCHEMA \
            or not isinstance(body.get('shards'), list):
        raise ValueError('linked_shard_registry_invalid')
    result, paths = {}, set()
    for raw in body['shards']:
        row = _validate(root, raw)
        if row['id'] in result or row['directory'] in paths:
            raise ValueError('linked_shard_registry_duplicate')
        result[row['id']] = row
        paths.add(row['directory'])
    return result


def attach_complete_shards(root, shards):
    """Atomically append complete VRS generations; retries are idempotent."""
    root = Path(root).expanduser().resolve()
    root.mkdir(mode=0o700, parents=True, exist_ok=True)
    candidates = []
    for shard in shards:
        directory = Path(shard['directory']).expanduser().resolve()
        relative = str(directory.relative_to(root)) if _inside(directory, root) else ''
        candidate = _validate(root, dict(id=str(shard['identifier']), path=relative,
            pair_snapshot_id=str(shard['pair_snapshot_id']),
            records=int(shard['records']), cue_total=int(shard['cue_total'])))
        candidates.append({key: candidate[key] for key in
                           ('id', 'path', 'pair_snapshot_id', 'records', 'cue_total')})
    if not candidates or len({row['id'] for row in candidates}) != len(candidates) \
            or len({row['path'] for row in candidates}) != len(candidates):
        raise ValueError('linked_shard_batch_invalid')
    lock = FileLock(str(root / (REGISTRY + '.lock')), thread_local=False)
    with lock:
        current = load_linked_shards(root)
        current_paths = {row['path']: row['id'] for row in current.values()}
        added = []
        for serial in candidates:
            previous = current.get(serial['id'])
            if previous is not None:
                comparable = {key: previous[key] for key in serial}
                if comparable != serial:
                    raise ValueError('linked_shard_reassignment_rejected')
                continue
            if serial['path'] in current_paths:
                raise ValueError('linked_shard_path_already_attached')
            added.append(serial)
            current_paths[serial['path']] = serial['id']
        rows = [{key: row[key] for key in candidates[0]}
                for row in current.values()]
        rows.extend(added)
        _atomic_json(root / REGISTRY, dict(schema=SCHEMA, shards=rows))
    return dict(status='attached' if added else 'already_attached',
                shards=[row['id'] for row in candidates], added=len(added),
                records=sum(row['records'] for row in candidates),
                cue_total=sum(row['cue_total'] for row in candidates))


def update_linked_shard(root, identifier, *, pair_snapshot_id, records, cue_total):
    """Publish the new generation after main extends an adopted source lineage."""
    root = Path(root).expanduser().resolve()
    lock = FileLock(str(root / (REGISTRY + '.lock')), thread_local=False)
    with lock:
        current = load_linked_shards(root)
        if identifier not in current:
            raise ValueError('linked_shard_not_registered')
        rows = []
        for shard, row in current.items():
            serial = {key: row[key] for key in
                      ('id', 'path', 'pair_snapshot_id', 'records', 'cue_total')}
            if shard == identifier:
                serial.update(pair_snapshot_id=str(pair_snapshot_id),
                              records=int(records), cue_total=int(cue_total))
                _validate(root, serial)
            rows.append(serial)
        _atomic_json(root / REGISTRY, dict(schema=SCHEMA, shards=rows))


def shutdown_and_release(state, timeout=120):
    """Stop an existing daemon without starting one and acquire its owner lock."""
    state = Path(state).resolve()
    port = port_of(state)
    if port is not None:
        client = LoopbackClient(port, 10)
        try:
            try:
                reply = client.request('shutdown')
            except InterfaceError:
                reply = None
            if reply is not None and reply.get('status') != 'stopping':
                raise ValueError('linked_shard_shutdown_rejected')
        finally:
            client.close()
    lock = FileLock(str(state / 'owner.lock'), thread_local=False)
    try:
        lock.acquire(timeout=timeout)
    except Timeout:
        raise ValueError('linked_shard_owner_not_released') from None
    finally:
        if lock.is_locked:
            lock.release()
    (state / 'loopback.port').unlink(missing_ok=True)


def reload_running_main(root):
    """Install new registry rows in a current main, or stop an older runtime."""
    root = Path(root).resolve()
    port = port_of(root)
    if port is None:
        return 'next_start'
    client = LoopbackClient(port, 120)
    try:
        try:
            reply = client.request('reload_linked_shards')
        except InterfaceError:
            reply = None
        if reply is not None and reply.get('status') == 'linked_shards_reloaded':
            return 'reloaded'
    finally:
        client.close()
    shutdown_and_release(root)
    return 'runtime_replaced'


def drop_derived_read_directory(state):
    target = Path(state).resolve() / 'exact-replay'
    if target.exists():
        shutil.rmtree(target)
