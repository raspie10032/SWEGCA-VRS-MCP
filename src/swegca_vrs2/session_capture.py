"""VRS-first conversation capture with no transcript/outbox database.

Host logs are ingress streams only. Complete public records are sent directly
to the session VRS in transport-bounded ``ingest_many`` generations. A small
atomic cursor stores byte position, accounting counts and recent VRS addresses;
it never stores dialogue text and is never a recall source. Stable request IDs
make a crash before cursor publication an idempotent retry.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import stat
import sys
import time

from .native_lock import FileLock

from .loopback import ensure_daemon
from .native_transport import MAX_BYTES, decode
from .server import LoopbackMCP, default_state_dir
from .linked_shards import (attach_complete_shards, drop_derived_read_directory,
                            reload_running_main, shutdown_and_release)
from .native_journal import is_native_store
from .resident import WarmView
from .read_lease import (RECALL_LEASE_NS, begin_engine_recall, end_all_engine_recalls,
                         end_engine_recall, renew_engine_recall)


CHUNK = 60000
INPUT_LIMIT = 16 * 1024 * 1024
FRAME_BUDGET = MAX_BYTES - 128 * 1024


def digest(value):
    return hashlib.sha256(value.encode('utf-8')).hexdigest()


def serialized_content(value):
    if isinstance(value, str):
        return value
    if isinstance(value, list) and len(value) == 1 and isinstance(value[0], dict):
        item = value[0]
        if item.get('type') in ('text', 'input_text', 'output_text') and isinstance(item.get('text'), str):
            return item['text']
    return json.dumps(value, ensure_ascii=False, sort_keys=True, allow_nan=False)


def without_private_reasoning(value):
    if isinstance(value, dict):
        return {key: without_private_reasoning(item) for key, item in value.items()
                if key not in ('encrypted_content', 'reasoning_content')}
    if isinstance(value, list):
        return [without_private_reasoning(item) for item in value]
    return value


def transcript_record(host, row):
    """Extract host-visible session content; encrypted/private reasoning is excluded."""
    if not isinstance(row, dict):
        raise ValueError('transcript_record_invalid')
    kind = row.get('type')
    if host == 'codex':
        if kind == 'compacted':
            value = row.get('payload', {}).get('message')
            return ('summary', value, 'compacted') if isinstance(value, str) and value else None
        if kind == 'token_usage_record':
            usage = row.get('payload', {}).get('usage')
            return ('usage', serialized_content(usage), kind) if isinstance(usage, dict) else None
        if kind != 'response_item':
            return ('session', serialized_content(without_private_reasoning(row)), str(kind))
        item = row.get('payload')
        if not isinstance(item, dict):
            raise ValueError('transcript_record_invalid')
        item_kind = item.get('type')
        if item_kind == 'reasoning':
            return None
        if item_kind == 'message':
            role = item.get('role')
            if role not in ('user', 'assistant', 'system', 'developer'):
                return None
            content = item.get('content')
            if not isinstance(content, list):
                raise ValueError('transcript_message_invalid')
            value = serialized_content(content)
            return (role, value, 'message') if value else None
        safe = {key: item[key] for key in ('type', 'call_id', 'name', 'input', 'output',
                                          'arguments', 'status') if key in item}
        return ('tool', serialized_content(safe), str(item_kind)) if safe else (
            'session', serialized_content(without_private_reasoning(item)), str(item_kind))
    if kind in ('user', 'assistant'):
        message = row.get('message')
        if not isinstance(message, dict):
            raise ValueError('transcript_message_invalid')
        content = message.get('content')
        return (kind, serialized_content(content), kind) if content is not None else None
    if kind == 'summary':
        summary = row.get('summary')
        return ('summary', summary, kind) if isinstance(summary, str) and summary else None
    return ('session', serialized_content(without_private_reasoning(row)), str(kind))


def private_directory(path):
    directory = Path(path).expanduser().resolve()
    directory.mkdir(mode=0o700, parents=True, exist_ok=True)
    if os.name != 'nt':
        if directory.stat().st_uid != os.getuid():
            raise ValueError('capture_directory_not_owned')
        os.chmod(directory, 0o700)
    return directory


def atomic_json(path, value):
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
            directory = os.open(path.parent, os.O_RDONLY)
            try:
                os.fsync(directory)
            finally:
                os.close(directory)
    finally:
        temporary.unlink(missing_ok=True)


def read_json(path, default):
    try:
        value = json.loads(path.read_text(encoding='utf-8'))
        return value if isinstance(value, dict) else dict(default)
    except (OSError, ValueError, UnicodeError):
        return dict(default)


class VRSClient:
    def __init__(self, state_dir, *, writes=True):
        self.state_dir, self.writes = state_dir, writes

    def __enter__(self):
        self.client = ensure_daemon(self.state_dir, allow_ingest=self.writes)
        enabled = bool(self.client.request('ping').get('writes_enabled'))
        if self.writes and not enabled:
            self.client.close()
            raise ValueError('session_vrs_write_disabled')
        self.server = LoopbackMCP(self.client, writes_enabled=self.writes and enabled)
        return self

    def __exit__(self, *_):
        self.server.close()

    def call(self, name, arguments):
        return self.server.call_tool(name, arguments)

    def ingest_many(self, rows):
        return self.client.request('ingest_many', rows=rows)

    def export(self, after_sequence):
        return self.client.request('export_experiences', after_sequence=int(after_sequence),
                                   max_records=512, max_bytes=768 * 1024)


class SessionCapture:
    def __init__(self, state_dir):
        self.root = private_directory(state_dir)
        self.meta = private_directory(self.root / 'session-capture')

    @staticmethod
    def session_key(session):
        if not isinstance(session, str) or not session or len(session) > 512:
            raise ValueError('invalid_session')
        return digest(session)

    def session_root(self, host, session):
        key = session if len(session) == 64 and all(c in '0123456789abcdef' for c in session) \
            else self.session_key(session)
        return private_directory(self.root / 'session-vrs' / host / key)

    def cursor_path(self, host, session, path):
        return self.meta / 'cursors' / host / self.session_key(session) / (digest(str(path)) + '.json')

    def end_path(self, host, session):
        return self.meta / 'ended' / host / (self.session_key(session) + '.json')

    def ending_path(self, host, session):
        return self.meta / 'ending' / host / (self.session_key(session) + '.json')

    def watcher_lock(self, host, session):
        root = private_directory(self.meta / 'watchers' / host)
        return FileLock(str(root / (self.session_key(session) + '.lock')), thread_local=False)

    def recall_root(self, host, session):
        return self.meta / 'recall-leases' / host / self.session_key(session)

    def recall_barrier(self, host, session):
        root = self.recall_root(host, session)
        root.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
        return FileLock(str(root) + '.lock', thread_local=False)

    def _live_recall_leases(self, host, session):
        root = self.recall_root(host, session)
        if not root.exists():
            return []
        now = time.time_ns()
        live = []
        for path in root.glob('*.json'):
            lease = read_json(path, {})
            if int(lease.get('expires_ns', 0)) > now:
                live.append(path)
            else:
                path.unlink(missing_ok=True)
        return live

    def begin_recall(self, host, session):
        """Pause transcript admission after every scan already in flight finishes."""
        root = private_directory(self.recall_root(host, session))
        identifier = os.urandom(16).hex()
        capture_path = root / (identifier + '.json')
        atomic_json(capture_path, dict(
            schema='swegca-vrs2-session-recall-lease-v1',
            created_ns=time.time_ns(), expires_ns=time.time_ns() + RECALL_LEASE_NS))
        try:
            begin_engine_recall(self.session_root(host, session), identifier)
            # The lease is visible before this barrier wait. A scan that already
            # crossed the barrier finishes first; later scans observe the lease.
            with self.recall_barrier(host, session):
                pass
        except Exception:
            capture_path.unlink(missing_ok=True)
            end_engine_recall(self.session_root(host, session), identifier)
            raise
        return identifier

    def renew_recall(self, host, session, identifier):
        if not isinstance(identifier, str) or len(identifier) != 32:
            raise ValueError('invalid_recall_lease')
        path = self.recall_root(host, session) / (identifier + '.json')
        if not path.is_file():
            raise ValueError('recall_lease_expired')
        lease = read_json(path, {})
        lease['expires_ns'] = time.time_ns() + RECALL_LEASE_NS
        atomic_json(path, lease)
        renew_engine_recall(self.session_root(host, session), identifier)

    def end_recall(self, host, session, identifier):
        if isinstance(identifier, str) and len(identifier) == 32:
            (self.recall_root(host, session) / (identifier + '.json')).unlink(missing_ok=True)
            end_engine_recall(self.session_root(host, session), identifier)

    def end_all_recalls(self, host, session):
        root = self.recall_root(host, session)
        if root.exists():
            for path in root.glob('*.json'):
                path.unlink(missing_ok=True)
        end_all_engine_recalls(self.session_root(host, session))

    @staticmethod
    def payloads(host, session_key, source_key, role, value, kind, line_number):
        chunks = [value[index:index + CHUNK] for index in range(0, len(value), CHUNK)]
        result = []
        for index, chunk in enumerate(chunks):
            identity = digest(f'transcript:{host}:{session_key}:{source_key}:{index}')
            result.append(dict(request_id=identity, text=chunk,
                source=f'transcript:{host}:{session_key}:{source_key}:part:{index + 1}',
                revision='1', outcome='pending',
                cues=[f'hook-session:{session_key}', f'hook-role:{role}'],
                metadata=dict(origin='session_transcript', host=host, role=role,
                    record_type=kind, line=line_number, part=index + 1,
                    parts=len(chunks), epistemic_status='unverified_transcript')))
        return result

    def scan_transcript(self, host, session, raw_path, *, force=False):
        if host not in ('codex', 'claude'):
            raise ValueError('invalid_host')
        path = Path(raw_path).expanduser().resolve()
        info = path.stat()
        if path.suffix != '.jsonl' or not stat.S_ISREG(info.st_mode):
            raise ValueError('transcript_not_regular')
        if os.name != 'nt' and info.st_uid != os.getuid():
            raise ValueError('transcript_not_owned')
        session_key = self.session_key(session)
        cursor_path = self.cursor_path(host, session, path)
        cursor_path.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
        lock = FileLock(str(cursor_path) + '.lock')
        with self.recall_barrier(host, session):
            if not force and self._live_recall_leases(host, session):
                cursor = read_json(cursor_path, dict(offset=0, line=0, captured=0,
                    excluded=0, recent_user=[]))
                return dict(status='capture_deferred_for_memory_read', admitted=0,
                    offset=int(cursor.get('offset', 0)), line=int(cursor.get('line', 0)),
                    captured=int(cursor.get('captured', 0)),
                    excluded=int(cursor.get('excluded', 0)),
                    accounted=int(cursor.get('line', 0)) ==
                        int(cursor.get('captured', 0)) + int(cursor.get('excluded', 0)),
                    session=session_key,
                    session_state=str(self.session_root(host, session_key)))
            with lock:
                if os.name != 'nt':
                    os.chmod(str(cursor_path) + '.lock', 0o600)
                return self._scan_locked(host, session_key, path, cursor_path)

    def _scan_locked(self, host, session_key, path, cursor_path):
        info = path.stat()
        cursor = read_json(cursor_path, dict(offset=0, line=0, captured=0,
            excluded=0, recent_user=[]))
        offset, line_number = int(cursor.get('offset', 0)), int(cursor.get('line', 0))
        captured, excluded = int(cursor.get('captured', 0)), int(cursor.get('excluded', 0))
        recent = [item for item in cursor.get('recent_user', []) if isinstance(item, str)][-4:]
        if info.st_size < offset:
            offset = line_number = captured = excluded = 0
            recent = []
        pending, pending_bytes, pending_roles = [], 64, []
        batch_cursor = dict(offset=offset, line=line_number, captured=captured,
                            excluded=excluded)
        added = 0

        def publish(client, safe):
            nonlocal pending, pending_bytes, pending_roles, recent, added
            pair_id = cursor.get('session_pair_id')
            if pending:
                result = client.ingest_many(pending)
                receipts = result.get('results')
                if (result.get('status') != 'observations_recorded'
                        or not isinstance(receipts, list) or len(receipts) != len(pending)):
                    raise ValueError('session_vrs_batch_rejected')
                for role, receipt in zip(pending_roles, receipts):
                    episode = receipt.get('episode_id')
                    if not isinstance(episode, str):
                        raise ValueError('session_vrs_receipt_invalid')
                    if role == 'user':
                        recent = [*recent, episode][-4:]
                added += len(pending)
                pair_id = result.get('pair_snapshot_id')
            atomic_json(cursor_path, dict(**safe, recent_user=recent,
                session_pair_id=pair_id))
            pending, pending_roles, pending_bytes = [], [], 64

        with VRSClient(self.session_root(host, session_key), writes=True) as client:
            with path.open('rb') as stream:
                stream.seek(offset)
                while raw := stream.readline():
                    if not raw.endswith(b'\n'):
                        break
                    line_start = offset
                    offset += len(raw)
                    line_number += 1
                    try:
                        record = json.loads(raw)
                    except (UnicodeError, ValueError):
                        raise ValueError('transcript_record_invalid') from None
                    normalized = transcript_record(host, record)
                    if normalized is None:
                        excluded += 1
                        batch_cursor = dict(offset=offset, line=line_number,
                            captured=captured, excluded=excluded)
                        continue
                    role, value, kind = normalized
                    source_key = f'{digest(str(path))}:{line_start}:{digest(raw.decode("utf-8"))}'
                    line_payloads = self.payloads(host, session_key, source_key, role,
                                                  value, kind, line_number)
                    for payload in line_payloads:
                        estimated = len(json.dumps(payload, ensure_ascii=False,
                                                   separators=(',', ':')).encode('utf-8')) + 64
                        if pending and pending_bytes + estimated > FRAME_BUDGET:
                            publish(client, batch_cursor)
                        pending.append(payload)
                        pending_roles.append(role)
                        pending_bytes += estimated
                    captured += 1
                    batch_cursor = dict(offset=offset, line=line_number,
                        captured=captured, excluded=excluded)
                publish(client, batch_cursor)
        return dict(status='captured_to_session_vrs', admitted=added,
            offset=offset, line=line_number, captured=captured, excluded=excluded,
            accounted=line_number == captured + excluded,
            session=session_key, session_state=str(self.session_root(host, session_key)))

    def mark_ended(self, host, session):
        key = self.session_key(session)
        path = self.end_path(host, session)
        previous = read_json(path, {})
        if previous.get('host') == host and previous.get('session') == key \
                and isinstance(previous.get('ended_ns'), int):
            self.ending_path(host, session).unlink(missing_ok=True)
            return key
        atomic_json(path, dict(host=host, session=key,
            ended_ns=time.time_ns(), merged=False))
        self.ending_path(host, session).unlink(missing_ok=True)
        return key

    def mark_ending(self, host, session):
        key = self.session_key(session)
        path = self.ending_path(host, session)
        atomic_json(path, dict(host=host, session=key, ending_ns=time.time_ns()))
        return key

    def ended(self):
        root = self.meta / 'ended'
        if not root.exists():
            return []
        return [(path, read_json(path, {})) for path in sorted(root.rglob('*.json'))]

    def merge_ended(self):
        merged_count = 0
        for marker_path, marker in self.ended():
            if marker.get('merged') is True:
                continue
            host, session = marker.get('host'), marker.get('session')
            if host not in ('codex', 'claude') or not isinstance(session, str):
                raise ValueError('ended_marker_invalid')
            state = self.session_root(host, session)
            shutdown_and_release(state)
            directories = [state, *sorted(path for path in (state / 'shards').glob('shard-*')
                                          if is_native_store(path))]
            components = []
            for directory in directories:
                suffix = 'main' if directory == state else directory.name
                identifier = f'session-{host}-{session}-{suffix}'
                view = WarmView(identifier, directory)
                try:
                    owner = view.refresh()
                    components.append(dict(identifier=identifier, directory=directory,
                        pair_snapshot_id=owner.pair.snapshot_id,
                        records=owner.memory.episode_count,
                        cue_total=owner.memory.cue_total))
                finally:
                    view.close()
            receipt = attach_complete_shards(self.root, components)
            experiences = receipt['records']
            activation = reload_running_main(self.root)
            drop_derived_read_directory(state)
            marker.update(merged=True, merged_ns=time.time_ns(), experiences=experiences,
                          shards=receipt['shards'],
                          activation=activation)
            atomic_json(marker_path, marker)
            merged_count += experiences
        return merged_count


def codex_session(path):
    try:
        with path.open('rb') as stream:
            row = json.loads(stream.readline())
        value = row.get('payload', {}).get('id')
        if row.get('type') == 'session_meta' and isinstance(value, str) and value:
            return value
    except (OSError, ValueError, UnicodeError, AttributeError):
        pass
    return None


def scan(capture, codex_home=None, claude_home=None, *, since=0.0):
    paths = []
    if codex_home is not None:
        paths.extend(('codex', path) for path in (Path(codex_home) / 'sessions').rglob('*.jsonl'))
    if claude_home is not None:
        paths.extend(('claude', path) for path in (Path(claude_home) / 'projects').rglob('*.jsonl'))
    results, errors = [], []
    for host, path in sorted(paths, key=lambda pair: str(pair[1])):
        try:
            if path.stat().st_mtime < since:
                continue
            session = codex_session(path) if host == 'codex' else path.stem
            if session:
                results.append(capture.scan_transcript(host, session, path))
        except (OSError, ValueError) as error:
            errors.append(dict(host=host, path=str(path), error=type(error).__name__))
    return dict(status='scan_complete', sessions=results,
                admitted=sum(result['admitted'] for result in results), errors=errors)


def handle(host, state_dir, event, *, tool_prefix='mcp__swegca_vrs__memory_'):
    if host not in ('codex', 'claude') or not isinstance(event, dict):
        raise ValueError('invalid_hook_event')
    capture = SessionCapture(state_dir)
    session = event.get('session_id')
    path = event.get('transcript_path') or event.get('agent_transcript_path')
    name = event.get('hook_event_name')
    tool_name = event.get('tool_name')
    if not isinstance(tool_prefix, str) or not tool_prefix.startswith('mcp__') \
            or not tool_prefix.endswith('__memory_'):
        raise ValueError('invalid_vrs_tool_prefix')
    vrs_tool = host == 'codex' and isinstance(tool_name, str) \
        and tool_name.startswith(tool_prefix)
    # Keep status -> context -> read -> release on one pinned snapshot. The
    # status pre-hook captures the prompt first; release (or Stop) captures the
    # intermediate VRS tool records after the request is done.
    if name == 'SessionEnd':
        if not isinstance(session, str) or not isinstance(path, str):
            raise ValueError('session_end_transcript_missing')
        from .conversation_finalize import schedule
        schedule(state_dir, host, session, path)
        return None
    if name == 'Interrupt':
        if not isinstance(session, str) or not isinstance(path, str):
            raise ValueError('interrupt_transcript_missing')
        from .conversation_finalize import schedule_capture
        schedule_capture(state_dir, host, session, path)
        return None
    if name in ('SessionStart', 'UserPromptSubmit') and isinstance(session, str) \
            and isinstance(path, str):
        from .conversation_watch import schedule as schedule_watch
        schedule_watch(state_dir, host, session, path)
    scan_now = not vrs_tool or (name == 'PreToolUse' and tool_name.endswith('memory_status')) \
        or (name == 'PostToolUse' and tool_name.endswith('memory_release'))
    if scan_now and isinstance(session, str) and isinstance(path, str):
        capture.scan_transcript(host, session, path)
    if name == 'PreToolUse' and vrs_tool:
        arguments = event.get('tool_input')
        if not isinstance(arguments, dict) or not isinstance(session, str):
            raise ValueError('invalid_vrs_tool_hook')
        return {'hookSpecificOutput': {'hookEventName': 'PreToolUse',
            'permissionDecision': 'allow',
            'updatedInput': dict(arguments, session_id=session)}}
    return None


def hook_main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--host', choices=('codex', 'claude'), required=True)
    parser.add_argument('--state-dir', type=Path, default=default_state_dir())
    parser.add_argument('--tool-prefix', default='mcp__swegca_vrs__memory_')
    args = parser.parse_args()
    try:
        raw = sys.stdin.buffer.read(INPUT_LIMIT + 1)
        if len(raw) > INPUT_LIMIT:
            raise ValueError('hook_input_too_large')
        result = handle(args.host, args.state_dir, decode(raw),
                        tool_prefix=args.tool_prefix)
        if result is not None:
            print(json.dumps(result, ensure_ascii=False, separators=(',', ':')))
        return 0
    except (OSError, ValueError) as exc:
        print('VRS2 conversation hook failed: ' + type(exc).__name__, file=sys.stderr)
        return 1
