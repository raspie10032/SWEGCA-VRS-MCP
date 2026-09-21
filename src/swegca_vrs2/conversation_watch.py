"""One session-local transcript tailer feeding every new record into VRS."""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import subprocess
import sys
import time

from .session_capture import SessionCapture
from .native_lock import Timeout


def watch(state_dir, host, session, transcript, *, poll_seconds=1.0):
    """Tail until SessionEnd publishes its durable marker; never infer an end."""
    capture = SessionCapture(state_dir)
    lock = capture.watcher_lock(host, session)
    try:
        lock.acquire(timeout=0)
    except Timeout:
        return False
    try:
        while (not capture.end_path(host, session).exists()
               and not capture.ending_path(host, session).exists()):
            try:
                capture.scan_transcript(host, session, transcript)
            except OSError:
                # The authoritative transcript may be between an append and a
                # complete JSONL line. The cursor advances only through the
                # last complete record, so retrying cannot duplicate experience.
                pass
            time.sleep(max(0.05, float(poll_seconds)))
        return True
    finally:
        lock.release()


def schedule(state_dir, host, session, transcript):
    command = [sys.executable, '-m', 'swegca_vrs2.conversation_watch',
               '--state-dir', str(Path(state_dir).expanduser().resolve()),
               '--host', str(host), '--session', str(session),
               '--transcript', str(Path(transcript).expanduser().resolve())]
    options = dict(stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                   stderr=subprocess.DEVNULL, close_fds=True)
    if os.name == 'nt':
        options['creationflags'] = (subprocess.DETACHED_PROCESS |
                                    subprocess.CREATE_NEW_PROCESS_GROUP)
    else:
        options['start_new_session'] = True
    return subprocess.Popen(command, **options).pid


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--state-dir', type=Path, required=True)
    parser.add_argument('--host', choices=('codex', 'claude'), required=True)
    parser.add_argument('--session', required=True)
    parser.add_argument('--transcript', type=Path, required=True)
    args = parser.parse_args()
    watch(args.state_dir, args.host, args.session, args.transcript)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
