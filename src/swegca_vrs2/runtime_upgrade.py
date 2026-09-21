"""One-time runtime handoff after an explicitly ended session is attached.

The active session keeps using the resident generation it started with.  An
upgrade is armed ahead of time, but it may run only after a newer SessionEnd
marker is durably merged.  Original native VRS stores stay untouched.  Only the
rebuildable disk read directory is removed after both residents release their
owner locks. The detached handoff then rebuilds that directory with the current
code before the next session can need durable main.
"""
from __future__ import annotations

import os
from pathlib import Path
import time

from .exact_replay import MAGIC as EXACT_MAGIC
from .linked_shards import shutdown_and_release, drop_derived_read_directory
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
    # Durable main may already have adopted and opened the ended session shards.
    # Release that parent owner first; otherwise waiting on a shard owner lock
    # deadlocks until the timeout while main is still serving the shard.
    shutdown_and_release(capture.root)
    for state in session_states:
        shutdown_and_release(state)
    for state in session_states:
        drop_derived_read_directory(state)
    drop_derived_read_directory(capture.root)

    # The ended task is no longer latency-sensitive. Rebuild the latest
    # main-owned read state here so the next task does not take the one-time
    # schema handoff cost on its first durable-memory fallback.
    from .store import Main
    from .resident import Resident
    from .loopback import _backfill_all_exact, _backfill_all_projections
    main = Main(capture.root, allow_ingest=True, defer_checkpoints=True)
    resident = Resident(main, {}, hot_limit=0)
    try:
        exact = _backfill_all_exact(resident)
        projections = _backfill_all_projections(resident)
        if not exact['complete'] or not projections['complete']:
            raise ValueError('runtime_upgrade_read_state_incomplete')
    finally:
        resident.close()
        main.close()

    receipt = dict(schema='swegca-vrs2-runtime-upgrade-receipt-v1',
                   armed_ns=armed_ns, activated_ns=time.time_ns(),
                   exact_schema=marker.get('exact_schema'),
                   sessions=[str(state) for state in session_states],
                   preserved='native-vrs-journal', rebuilt='exact-replay',
                   exact=exact, projections=projections)
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
