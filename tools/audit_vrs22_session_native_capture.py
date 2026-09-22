"""Compare one captured host transcript prefix with original native VRS journals.

The report contains only counts and status. It never prints dialogue, source
addresses, request IDs, or original hashes. The active session may keep
appending; the capture cursor defines the audited immutable prefix.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import sys

from swegca_vrs2.native_journal import NativeJournal, is_native_store
from swegca_vrs2.session_capture import SessionCapture, digest, transcript_record
from swegca_vrs2.store import canonical, journal_entry, observation


def audit(state: Path, transcript: Path, session: str) -> dict:
    capture = SessionCapture(state)
    key = capture.session_key(session)
    cursor = json.loads(capture.cursor_path('codex', session, transcript).read_text())
    cutoff = int(cursor['offset'])
    expected = {}
    lines = captured = excluded = offset = 0
    with transcript.open('rb') as stream:
        while offset < cutoff:
            raw = stream.readline()
            if not raw.endswith(b'\n') or offset + len(raw) > cutoff:
                raise ValueError('captured_transcript_prefix_changed')
            lines += 1
            start = offset
            offset += len(raw)
            record = json.loads(raw)
            normalized = transcript_record('codex', record)
            if normalized is None:
                excluded += 1
                continue
            role, value, kind = normalized
            source_key = f'{digest(str(transcript.resolve()))}:{start}:{digest(raw.decode("utf-8"))}'
            for payload in capture.payloads('codex', key, source_key, role, value, kind, lines):
                row = observation(payload)
                identifier = row['request_id']
                if identifier in expected:
                    raise ValueError('duplicate_transcript_experience_request')
                expected[identifier] = hashlib.sha256(canonical(row).encode('utf-8')).digest()
            captured += 1
    root = capture.session_root('codex', session)
    stores = [root, *sorted((root / 'shards').glob('shard-*'))]
    actual = {}
    native_stores = 0
    for directory in stores:
        if not is_native_store(directory):
            continue
        native_stores += 1
        journal = NativeJournal(directory)
        try:
            for _, request_id, body, fingerprint, _ in journal.rows():
                kind, row = journal_entry(request_id, body, fingerprint)
                if kind != 'observation':
                    continue
                if request_id in actual:
                    raise ValueError('duplicate_native_experience_request')
                actual[request_id] = hashlib.sha256(canonical(row).encode('utf-8')).digest()
        finally:
            journal.close()

    # The writer can append complete native observations after the cursor was
    # frozen above. Authenticate those extra rows against complete transcript
    # lines beyond the frozen cursor instead of calling them contamination.
    # Read the tail after the journal snapshot so every row seen in the journal
    # must already have its authoritative transcript line available here.
    post_cursor = {}
    post_cursor_lines = 0
    tail_offset = cutoff
    with transcript.open('rb') as stream:
        stream.seek(tail_offset)
        while raw := stream.readline():
            if not raw.endswith(b'\n'):
                break
            start = tail_offset
            tail_offset += len(raw)
            post_cursor_lines += 1
            normalized = transcript_record('codex', json.loads(raw))
            if normalized is None:
                continue
            role, value, kind = normalized
            source_key = f'{digest(str(transcript.resolve()))}:{start}:{digest(raw.decode("utf-8"))}'
            for payload in capture.payloads('codex', key, source_key, role, value, kind,
                                            lines + post_cursor_lines):
                row = observation(payload)
                identifier = row['request_id']
                if identifier in expected or identifier in post_cursor:
                    raise ValueError('duplicate_transcript_experience_request')
                post_cursor[identifier] = hashlib.sha256(canonical(row).encode('utf-8')).digest()
    missing = expected.keys() - actual.keys()
    unexpected = actual.keys() - expected.keys() - post_cursor.keys()
    changed = sum(expected[key] != actual[key] for key in expected.keys() & actual.keys())
    changed += sum(post_cursor[key] != actual[key]
                   for key in post_cursor.keys() & actual.keys())
    post_cursor_present = len(post_cursor.keys() & actual.keys())
    accounted = (lines == captured + excluded == int(cursor['line'])
                 and captured == int(cursor['captured'])
                 and excluded == int(cursor['excluded'])
                 and offset == int(cursor['offset']))
    return dict(status='PASS' if accounted and not (missing or unexpected or changed) else 'FAIL',
                scope='captured transcript prefix plus authenticated concurrent native tail',
                transcript_lines=lines, captured_lines=captured, excluded_private_or_control_lines=excluded,
                expected_original_observations=len(expected), native_original_observations=len(actual),
                post_cursor_transcript_lines=post_cursor_lines,
                post_cursor_original_observations=post_cursor_present,
                post_cursor_pending_observations=len(post_cursor) - post_cursor_present,
                native_stores=native_stores, missing_requests=len(missing),
                unexpected_requests=len(unexpected), changed_originals=changed,
                cursor_accounted=accounted, sqlite_module_loaded='sqlite3' in sys.modules)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--state', type=Path, required=True)
    parser.add_argument('--transcript', type=Path, required=True)
    parser.add_argument('--session', required=True)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    result = audit(args.state.resolve(), args.transcript.resolve(), args.session)
    body = json.dumps(result, ensure_ascii=False, indent=2) + '\n'
    if args.output:
        if args.output.exists():
            parser.error('output already exists')
        args.output.write_text(body, encoding='utf-8')
    print(body, end='')
    if result['status'] != 'PASS':
        raise SystemExit(1)


if __name__ == '__main__':
    main()
