"""Small on-disk leases that keep a resident VRS generation stable during recall."""
from __future__ import annotations

import json
import os
from pathlib import Path
import time


RECALL_LEASE_NS = 120 * 1_000_000_000


def _root(state_dir):
    return Path(state_dir).expanduser().resolve() / '.recall-leases'


def _path(state_dir, identifier):
    if not isinstance(identifier, str) or len(identifier) != 32 \
            or any(character not in '0123456789abcdef' for character in identifier):
        raise ValueError('invalid_recall_lease')
    return _root(state_dir) / (identifier + '.json')


def _write(path, body):
    path.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
    temporary = path.with_name(path.name + '.' + os.urandom(8).hex() + '.tmp')
    try:
        with temporary.open('x', encoding='utf-8') as stream:
            json.dump(body, stream, sort_keys=True, separators=(',', ':'))
            stream.flush()
            os.fsync(stream.fileno())
        if os.name != 'nt':
            os.chmod(temporary, 0o600)
        os.replace(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)


def begin_engine_recall(state_dir, identifier):
    now = time.time_ns()
    _write(_path(state_dir, identifier), dict(
        schema='swegca-vrs2-engine-recall-lease-v1', created_ns=now,
        expires_ns=now + RECALL_LEASE_NS))


def renew_engine_recall(state_dir, identifier):
    path = _path(state_dir, identifier)
    if not path.is_file():
        raise ValueError('recall_lease_expired')
    try:
        body = json.loads(path.read_text(encoding='utf-8'))
    except (OSError, ValueError, TypeError, json.JSONDecodeError):
        raise ValueError('recall_lease_invalid') from None
    body['expires_ns'] = time.time_ns() + RECALL_LEASE_NS
    _write(path, body)


def end_engine_recall(state_dir, identifier):
    try:
        _path(state_dir, identifier).unlink(missing_ok=True)
    except ValueError:
        pass


def end_all_engine_recalls(state_dir):
    root = _root(state_dir)
    if root.exists():
        for path in root.glob('*.json'):
            path.unlink(missing_ok=True)


def engine_recall_active(state_dir):
    """Return whether consolidation must defer; discard only expired leases."""
    root = _root(state_dir)
    if not root.exists():
        return False
    now = time.time_ns()
    active = False
    for path in root.glob('*.json'):
        try:
            body = json.loads(path.read_text(encoding='utf-8'))
            if (body.get('schema') == 'swegca-vrs2-engine-recall-lease-v1'
                    and int(body.get('expires_ns', 0)) > now):
                active = True
            else:
                path.unlink(missing_ok=True)
        except (OSError, ValueError, TypeError, json.JSONDecodeError):
            path.unlink(missing_ok=True)
    return active
