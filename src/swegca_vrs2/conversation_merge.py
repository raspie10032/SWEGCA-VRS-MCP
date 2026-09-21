"""Adopt explicitly ended complete session VRS shards into main ownership."""
from __future__ import annotations

import argparse
from pathlib import Path

from .native_lock import FileLock, Timeout

from .session_capture import SessionCapture


def run(state_dir):
    root = Path(state_dir).expanduser().resolve()
    lock = FileLock(root / 'conversation_merge.lock')
    try:
        lock.acquire(timeout=0)
    except Timeout:
        return 0
    try:
        capture = SessionCapture(root)
        capture.merge_ended()
        from .runtime_upgrade import activate_if_ready
        activate_if_ready(capture)
    finally:
        lock.release()
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--state-dir', type=Path, required=True)
    args = parser.parse_args()
    return run(args.state_dir)


if __name__ == '__main__':
    raise SystemExit(main())
