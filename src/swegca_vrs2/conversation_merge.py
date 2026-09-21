"""Asynchronously assimilate explicitly ended session VRS experience."""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import subprocess
import sys

from filelock import FileLock, Timeout

from .session_capture import SessionCapture


def run(state_dir):
    root = Path(state_dir).expanduser().resolve()
    lock = FileLock(root / 'conversation_merge.lock')
    try:
        lock.acquire(timeout=0)
    except Timeout:
        return 0
    try:
        SessionCapture(root).merge_ended()
    finally:
        lock.release()
    return 0


def schedule(state_dir):
    """Start one detached merger; the durable ended marker is the queue."""
    command = [sys.executable, '-m', 'swegca_vrs2.conversation_merge',
               '--state-dir', str(Path(state_dir).expanduser().resolve())]
    options = dict(stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                   stderr=subprocess.DEVNULL, close_fds=True)
    if os.name == 'nt':
        options['creationflags'] = (subprocess.DETACHED_PROCESS |
                                    subprocess.CREATE_NEW_PROCESS_GROUP)
    else:
        options['start_new_session'] = True
    subprocess.Popen(command, **options)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--state-dir', type=Path, required=True)
    args = parser.parse_args()
    return run(args.state_dir)


if __name__ == '__main__':
    raise SystemExit(main())
