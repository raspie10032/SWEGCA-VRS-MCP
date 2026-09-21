"""One-time runtime handoff after an explicitly ended session is assimilated.

The active session keeps using the resident generation it started with.  An
upgrade is armed ahead of time, but it may run only after a newer SessionEnd
marker is durably merged.  Original VRS databases stay untouched.  Only the
rebuildable disk read directory is removed after both residents release their
owner locks, so the next session starts the current code and backfills from
the preserved experiences.
"""
from __future__ import annotations

import os
from pathlib import Path
import shutil
import time

from filelock import FileLock, Timeout

from .exact_replay import MAGIC as EXACT_MAGIC
from .loopback import LoopbackClient, port_of
from .native_transport import InterfaceError
from .session_capture import atomic_json, read_json


MARKER = 'runtime-upgrade.json'


def marker_path(root):
    return Path(root).expanduser().resolve() / 'session-capture' / MARKER


def arm(root):
    """Durably request one handoff at the next newly ended merged session."""
    path = marker_path(root)
    if path.exists():
        return read_json(path, {})
    body = dict(schema='swegca-vrs2-runtime-upgrade-v1', armed_ns=time.time_ns(),
                exact_schema=EXACT_MAGIC.rstrip(b'\0').decode('ascii'))
    atomic_json(path, body)
    return body


def _shutdown_and_release(state, timeout=120):
    """Stop an existing daemon without starting one and wait for its owner lock."""
    state = Path(state).resolve()
    port = port_of(state)
    if port is not None:
        client = LoopbackClient(port, 10)
        try:
            try:
                reply = client.request('shutdown')
            except InterfaceError:
                # A dead daemon may leave a stale port file.  Ownership below
                # is authoritative: deletion is allowed only after its lock
                # can be acquired.
                reply = None
            if reply is not None and reply.get('status') != 'stopping':
                raise ValueError('runtime_upgrade_shutdown_rejected')
        finally:
            client.close()
    lock = FileLock(state / 'owner.lock', thread_local=False)
    try:
        lock.acquire(timeout=timeout)
    except Timeout:
        raise ValueError('runtime_upgrade_owner_not_released') from None
    finally:
        if lock.is_locked:
            lock.release()
    try:
        (state / 'loopback.port').unlink()
    except OSError:
        pass


def _drop_read_directory(state):
    state = Path(state).resolve()
    target = state / 'exact-replay'
    if target.exists():
        shutil.rmtree(target)


def activate_if_ready(capture):
    """Perform the armed handoff only after a post-arm SessionEnd was merged."""
    path = marker_path(capture.root)
    marker = read_json(path, {})
    if marker.get('schema') != 'swegca-vrs2-runtime-upgrade-v1':
        return False
    armed_ns = int(marker.get('armed_ns', 0))
    ended = [(marker_path_, row) for marker_path_, row in capture.ended()
             if row.get('merged') is True and int(row.get('ended_ns', 0)) >= armed_ns]
    if not ended:
        return False

    session_states = []
    for _, row in ended:
        session_states.append(capture.session_root(row['host'], row['session']))
    for state in session_states:
        _shutdown_and_release(state)
    _shutdown_and_release(capture.root)
    for state in session_states:
        _drop_read_directory(state)
    _drop_read_directory(capture.root)

    receipt = dict(schema='swegca-vrs2-runtime-upgrade-receipt-v1',
                   armed_ns=armed_ns, activated_ns=time.time_ns(),
                   exact_schema=marker.get('exact_schema'),
                   sessions=[str(state) for state in session_states],
                   preserved='memory.sqlite3', rebuilt='exact-replay')
    receipt_path = capture.meta / 'runtime-upgrades' / (
        str(receipt['activated_ns']) + '.json')
    atomic_json(receipt_path, receipt)
    path.unlink()
    if os.name != 'nt':
        directory = os.open(path.parent, os.O_RDONLY)
        try:
            os.fsync(directory)
        finally:
            os.close(directory)
    return True
