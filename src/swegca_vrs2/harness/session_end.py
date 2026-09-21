# -*- coding: utf-8 -*-
"""SessionEnd hook body (2.2, 2026-09-21): the producer says it is finished. The daemon marks the session's proposal
journal ended and merges it into main off the request path (``merge.py``); a session that ends without this hook
(a crash, a kill) merges when the sweeper finds it idle. The receipt line goes to ``session_end.log``.

stdin: the hook payload (``session_id``, ``reason``). Never blocks: a daemon that is down is a receipt, not an error.
"""
import io
import json
import os
import sys
import time

from .paths import RECEIPTS, STATE, PYTHON, BUNDLE_LIMIT, BUNDLES, HOT_BUNDLES

LOG = os.path.join(RECEIPTS, "session_end.log")


def note(**fields):
    fields["ts"] = time.strftime("%Y-%m-%d %H:%M:%S")
    try:
        with io.open(LOG, "a", encoding="utf-8") as f:
            f.write(json.dumps(fields, ensure_ascii=False) + "\n")
    except OSError:
        pass


def run(session_id, reason=""):
    if not session_id:
        note(skip="no_session")
        return None
    from swegca_vrs2.loopback import ensure_daemon, port_of
    if not port_of(STATE):
        note(session=session_id[:12], skip="daemon_down", reason=reason)     # the sweeper merges it when idle
        return None
    client = ensure_daemon(STATE, allow_ingest=True, python=PYTHON, bundle_limit=BUNDLE_LIMIT, bundles=BUNDLES, hot_bundles=HOT_BUNDLES)
    try:
        out = client.request("session_end", session=session_id)
    finally:
        client.close()
    note(session=session_id[:12], reason=reason, journal=out.get("journal"), queued=out.get("queued"), enabled=out.get("enabled", True))
    return out


def main():
    try:
        data = json.loads(sys.stdin.read() or "{}")
    except ValueError:
        data = {}
    try:
        run(str(data.get("session_id") or ""), str(data.get("reason") or ""))
    except Exception as failure:
        note(error=repr(failure)[:300])


if __name__ == "__main__":
    main()
