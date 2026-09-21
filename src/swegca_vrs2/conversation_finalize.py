"""Detached final capture and linked-shard adoption after SessionEnd."""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import subprocess
import sys
import time

from .session_capture import SessionCapture


def finalize(state_dir, host, session, transcript):
    """Capture a stable final tail before publishing the durable end marker."""
    capture = SessionCapture(state_dir)
    path = Path(transcript).expanduser().resolve()
    stable = 0
    result = None
    for _ in range(20):
        result = capture.scan_transcript(host, session, path)
        size = path.stat().st_size
        if result['offset'] == size:
            time.sleep(0.1)
            if path.stat().st_size == size:
                stable += 1
                if stable >= 2:
                    break
            else:
                stable = 0
        else:
            stable = 0
            time.sleep(0.1)
    if result is None or result['offset'] != path.stat().st_size:
        raise ValueError('session_transcript_not_stable')
    capture.mark_ended(host, session)
    from .conversation_merge import run
    run(state_dir)
    return result


def schedule(state_dir, host, session, transcript):
    """Leave the synchronous three-second SessionEnd hook immediately."""
    command = [sys.executable, '-m', 'swegca_vrs2.conversation_finalize',
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
    subprocess.Popen(command, **options)


def schedule_capture(state_dir, host, session, transcript):
    """Capture an Interrupt tail outside its three-second hook budget."""
    command = [sys.executable, '-m', 'swegca_vrs2.conversation_finalize',
               '--state-dir', str(Path(state_dir).expanduser().resolve()),
               '--host', str(host), '--session', str(session),
               '--transcript', str(Path(transcript).expanduser().resolve()),
               '--capture-only']
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
    parser.add_argument('--host', choices=('codex', 'claude'), required=True)
    parser.add_argument('--session', required=True)
    parser.add_argument('--transcript', type=Path, required=True)
    parser.add_argument('--capture-only', action='store_true')
    args = parser.parse_args()
    if args.capture_only:
        SessionCapture(args.state_dir).scan_transcript(
            args.host, args.session, args.transcript)
    else:
        finalize(args.state_dir, args.host, args.session, args.transcript)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
