import json
import os
from pathlib import Path
import subprocess
import sys
import threading
import time

import pytest

from swegca_vrs2.layered import LayeredMCP
from swegca_vrs2.loopback import ensure_daemon
from swegca_vrs2.conversation_merge import run as merge_run
from swegca_vrs2.runtime_upgrade import arm as arm_upgrade
from swegca_vrs2.session_capture import SessionCapture, VRSClient, handle
from swegca_vrs2.conversation_finalize import finalize
from swegca_vrs2.resident import Resident
from swegca_vrs2.sharded import ShardedMain
from swegca_vrs2.store import Main
from swegca_vrs2.native_journal import NativeJournal, is_native_store
from swegca_vrs2.native_transport import InterfaceError
from swegca_vrs2.read_lease import engine_recall_active
from swegca_vrs2.codex_hooks import config as codex_hook_config
from swegca_vrs2.conversation_watch import watch


def stop(*states):
    for state in states:
        try:
            client = ensure_daemon(state, allow_ingest=True)
            client.request('shutdown')
            client.close()
        except Exception:
            pass


def write_transcript(path, session, rows):
    values = [{'type': 'session_meta', 'payload': {'id': session}}, *rows]
    path.write_text(''.join(json.dumps(row, ensure_ascii=False) + '\n'
                            for row in values), encoding='utf-8')


def append_message(path, value):
    with path.open('a', encoding='utf-8') as stream:
        stream.write(json.dumps({'type': 'response_item', 'payload': {
            'type': 'message', 'role': 'user',
            'content': [{'type': 'input_text', 'text': value}]}}
            , ensure_ascii=False) + '\n')


def finish(server, session, packet):
    while packet.get('status') != 'memory_context_ready':
        call = packet['next_call']
        assert call['name'] == 'memory_continue'
        packet = server.call_tool('memory_continue', dict(
            call['arguments'], session_id=session))
    return packet


def test_transcript_enters_only_session_vrs_without_sqlite_outbox(tmp_path):
    state, transcript = tmp_path / 'state', tmp_path / 'rollout.jsonl'
    session = 'session-one'
    write_transcript(transcript, session, [
        {'type': 'response_item', 'payload': {'type': 'message', 'role': 'user',
            'content': [{'type': 'input_text', 'text': 'purpose_unique_91'}]}},
        {'type': 'response_item', 'payload': {'type': 'function_call',
            'name': 'exec', 'call_id': 'call-1', 'arguments': '{"cmd":"pwd"}'}},
        {'type': 'response_item', 'payload': {'type': 'reasoning',
            'encrypted_content': 'private'}}])
    capture = SessionCapture(state)
    try:
        first = capture.scan_transcript('codex', session, transcript)
        second = capture.scan_transcript('codex', session, transcript)
        assert first['accounted'] and first['captured'] == 3 and first['excluded'] == 1
        assert first['admitted'] == 3 and second['admitted'] == 0
        assert not (state / 'memory.sqlite3').exists()
        assert not (state / 'conversation_hooks.sqlite3').exists()
        with VRSClient(capture.session_root('codex', session), writes=False) as local:
            status = local.call('memory_status', {})
        assert status['hot_episode_count'] == 3
    finally:
        stop(capture.session_root('codex', session))


def test_codex_pretool_hook_injects_exact_session_id(tmp_path):
    event = {'hook_event_name': 'PreToolUse', 'session_id': 'exact-session',
             'tool_name': 'mcp__swegca_vrs__memory_status', 'tool_input': {}}
    result = handle('codex', tmp_path / 'state', event)
    output = result['hookSpecificOutput']
    assert output['permissionDecision'] == 'allow'
    assert output['updatedInput'] == {'session_id': 'exact-session'}


def test_generated_codex_end_and_interrupt_hooks_fit_runtime_deadline(tmp_path):
    hooks = codex_hook_config(Path('/python'), tmp_path,
        module_root=Path('/source'))['hooks']
    assert 'matcher' not in hooks['SessionStart'][0]
    assert 'matcher' not in hooks['SessionEnd'][0]
    assert hooks['SessionEnd'][0]['hooks'][0]['timeout'] == 3
    assert hooks['Interrupt'][0]['hooks'][0]['timeout'] == 3


def test_generated_hook_preserves_virtualenv_python_symlink(tmp_path):
    base = tmp_path / 'base-python'
    base.write_text('', encoding='utf-8')
    virtualenv_python = tmp_path / 'venv' / 'bin' / 'python'
    virtualenv_python.parent.mkdir(parents=True)
    virtualenv_python.symlink_to(base)
    hooks = codex_hook_config(virtualenv_python, tmp_path / 'state')['hooks']
    run = hooks['SessionStart'][0]['hooks'][0]['command']
    assert run.split()[0] == str(virtualenv_python.absolute())
    assert str(base.resolve()) not in run


def test_generated_hook_routes_only_the_configured_mcp_server(tmp_path):
    event = {'hook_event_name': 'PreToolUse', 'session_id': 'exact-session',
             'tool_name': 'mcp__memory_prod__memory_status', 'tool_input': {}}
    assert handle('codex', tmp_path / 'state', event,
                  tool_prefix='mcp__memory_prod__memory_')['hookSpecificOutput'][
                      'updatedInput']['session_id'] == 'exact-session'
    assert handle('codex', tmp_path / 'state', dict(event,
        tool_name='mcp__another__memory_status'),
        tool_prefix='mcp__memory_prod__memory_') is None


def test_every_live_capture_event_enters_session_vrs_and_end_is_detached(tmp_path,
                                                                         monkeypatch):
    state, transcript = tmp_path / 'state', tmp_path / 'rollout.jsonl'
    session = 'lifecycle-session'
    write_transcript(transcript, session, [])
    capture = SessionCapture(state)
    base = dict(session_id=session, transcript_path=str(transcript))
    try:
        captured = 0
        monkeypatch.setattr('swegca_vrs2.conversation_watch.schedule', lambda *args: 1)
        for index, name in enumerate(('SessionStart', 'UserPromptSubmit', 'PostToolUse',
                                      'Stop', 'PreCompact', 'PostCompact')):
            append_message(transcript, f'lifecycle-{index}')
            handle('codex', state, dict(base, hook_event_name=name))
            cursor = next((capture.meta / 'cursors').rglob('*.json'))
            current = json.loads(cursor.read_text(encoding='utf-8'))['captured']
            assert current > captured
            captured = current

        append_message(transcript, 'status-boundary')
        status_event = dict(base, hook_event_name='PreToolUse',
                            tool_name='mcp__swegca_vrs__memory_status', tool_input={})
        routed = handle('codex', state, status_event)
        assert routed['hookSpecificOutput']['updatedInput']['session_id'] == session
        after_status = json.loads(cursor.read_text(encoding='utf-8'))['captured']
        assert after_status > captured

        append_message(transcript, 'pinned-context-call')
        context_event = dict(base, hook_event_name='PreToolUse',
            tool_name='mcp__swegca_vrs__memory_context', tool_input={})
        handle('codex', state, context_event)
        assert json.loads(cursor.read_text(encoding='utf-8'))['captured'] == after_status

        release_event = dict(base, hook_event_name='PostToolUse',
            tool_name='mcp__swegca_vrs__memory_release')
        handle('codex', state, release_event)
        assert json.loads(cursor.read_text(encoding='utf-8'))['captured'] > after_status

        calls = []
        monkeypatch.setattr('swegca_vrs2.conversation_finalize.schedule_capture',
                            lambda *args: calls.append(('interrupt', args)))
        monkeypatch.setattr('swegca_vrs2.conversation_finalize.schedule',
                            lambda *args: calls.append(('end', args)))
        handle('codex', state, dict(base, hook_event_name='Interrupt'))
        handle('codex', state, dict(base, hook_event_name='SessionEnd'))
        assert [row[0] for row in calls] == ['interrupt', 'end']
        assert not capture.end_path('codex', session).exists()
    finally:
        stop(capture.session_root('codex', session))


def test_session_watcher_captures_between_hooks_and_never_infers_end(tmp_path):
    state, transcript = tmp_path / 'state', tmp_path / 'rollout.jsonl'
    session = 'watcher-session'
    write_transcript(transcript, session, [])
    capture = SessionCapture(state)
    thread = threading.Thread(target=watch,
        args=(state, 'codex', session, transcript), kwargs={'poll_seconds': 0.05})
    thread.start()
    try:
        append_message(transcript, 'between_hooks_unique_9494')
        deadline = time.time() + 10
        cursor = None
        while time.time() < deadline:
            paths = list((capture.meta / 'cursors').rglob('*.json'))
            if paths:
                cursor = json.loads(paths[0].read_text(encoding='utf-8'))
                if cursor['offset'] == transcript.stat().st_size:
                    break
            time.sleep(0.05)
        assert cursor is not None and cursor['offset'] == transcript.stat().st_size
        assert not capture.end_path('codex', session).exists()
        capture.mark_ended('codex', session)
        thread.join(timeout=5)
        assert not thread.is_alive()
    finally:
        if thread.is_alive():
            capture.mark_ended('codex', session)
            thread.join(timeout=5)
        stop(capture.session_root('codex', session))


def test_detached_finalizer_captures_last_tail_before_attaching(tmp_path):
    state, transcript = tmp_path / 'state', tmp_path / 'rollout.jsonl'
    session = 'finalizer-session'
    write_transcript(transcript, session, [])
    capture = SessionCapture(state)
    session_state = capture.session_root('codex', session)
    try:
        capture.scan_transcript('codex', session, transcript)
        append_message(transcript, 'final_tail_unique_7272')
        result = finalize(state, 'codex', session, transcript)
        assert result['offset'] == transcript.stat().st_size
        marker = json.loads(capture.end_path('codex', session).read_text(encoding='utf-8'))
        assert marker['merged'] is True
        registry = json.loads((state / 'linked-shards.json').read_text(encoding='utf-8'))
        assert sum(row['records'] for row in registry['shards']) == 2
        with VRSClient(state, writes=False) as main:
            exported = main.export(0)
        assert 'final_tail_unique_7272' in {
            row['observation']['text'] for row in exported['rows']}
    finally:
        stop(session_state, state)


def test_finalizer_stops_active_watcher_and_attaches_delayed_final_tail(tmp_path):
    state, transcript = tmp_path / 'state', tmp_path / 'rollout.jsonl'
    session = 'finalizer-watcher-race'
    write_transcript(transcript, session, [])
    capture = SessionCapture(state)
    session_state = capture.session_root('codex', session)
    watcher = threading.Thread(target=watch,
        args=(state, 'codex', session, transcript), kwargs={'poll_seconds': 0.01})
    watcher.start()
    deadline = time.time() + 5
    while time.time() < deadline and not list((capture.meta / 'cursors').rglob('*.json')):
        time.sleep(0.01)

    def delayed_tail():
        time.sleep(0.25)
        append_message(transcript, 'delayed_session_end_tail_7373')

    writer = threading.Thread(target=delayed_tail)
    writer.start()
    try:
        result = finalize(state, 'codex', session, transcript)
        writer.join(timeout=2)
        watcher.join(timeout=2)
        assert not writer.is_alive() and not watcher.is_alive()
        assert result['offset'] == transcript.stat().st_size
        assert not capture.ending_path('codex', session).exists()
        marker = json.loads(capture.end_path('codex', session).read_text(encoding='utf-8'))
        registry = json.loads((state / 'linked-shards.json').read_text(encoding='utf-8'))
        journal = NativeJournal(session_state, create=False, writable=False)
        try:
            session_count = sum(1 for _ in journal.rows())
        finally:
            journal.close()
        with VRSClient(state, writes=False) as main:
            main_rows = main.export(0)['rows']
        assert marker['merged'] is True
        assert marker['experiences'] == session_count
        assert sum(row['records'] for row in registry['shards']) == session_count
        assert len(main_rows) == session_count
        assert 'delayed_session_end_tail_7373' in {
            row['observation']['text'] for row in main_rows}
    finally:
        if watcher.is_alive():
            capture.mark_ending('codex', session)
            watcher.join(timeout=2)
        writer.join(timeout=2)
        stop(session_state, state)


def test_session_end_live_reloads_main_and_is_immediately_recallable(tmp_path):
    state, transcript = tmp_path / 'state', tmp_path / 'rollout.jsonl'
    session = 'live-reload-session'
    write_transcript(transcript, session, [
        {'type': 'response_item', 'payload': {'type': 'message', 'role': 'user',
            'content': [{'type': 'input_text', 'text': 'live_reload_unique_8383'}]}}])
    capture = SessionCapture(state)
    session_state = capture.session_root('codex', session)
    try:
        capture.scan_transcript('codex', session, transcript)
        with VRSClient(state, writes=True) as main:
            assert main.call('memory_status', {})['hot_episode_count'] == 0
        capture.mark_ended('codex', session)
        assert capture.merge_ended() == 2
        marker = json.loads(capture.end_path('codex', session).read_text(encoding='utf-8'))
        assert marker['activation'] == 'reloaded'

        with VRSClient(state, writes=False) as main:
            status = main.call('memory_status', {})
            assert status['logical_episode_count'] == 2
            packet = main.call('memory_context', dict(request_id='live-reload-query',
                query='live_reload_unique_8383',
                expected_pair_snapshot_id=status['pair_snapshot_id']))
            while packet.get('status') != 'memory_context_ready':
                packet = main.call('memory_continue', dict(
                    packet['next_call']['arguments'], request_id='live-reload-query'))
            assert packet['candidate_count'] >= 1
    finally:
        stop(session_state, state)


def test_session_hit_then_complete_miss_falls_back_to_main(tmp_path):
    state, session = tmp_path / 'state', 'session-layer'
    capture = SessionCapture(state)
    session_state = capture.session_root('codex', session)
    with VRSClient(session_state, writes=True) as local:
        local.ingest_many([dict(request_id='local-1', text='local_unique_4242',
            source='test:local', revision='1', outcome='pending')])
    with VRSClient(state, writes=True) as main:
        main.ingest_many([dict(request_id='main-1', text='main_unique_9898',
            source='test:main', revision='1', outcome='pending')])
    server = LayeredMCP(state)
    try:
        status = server.call_tool('memory_status', {'session_id': session})
        assert status['memory_layer'] == 'session' and server.main is None
        local = finish(server, session, server.call_tool('memory_context', {
            'session_id': session, 'request_id': 'local-query',
            'query': 'local_unique_4242',
            'expected_pair_snapshot_id': status['pair_snapshot_id']}))
        assert local['candidate_count'] == 1
        assert local['memory_layer'] == 'session' and local['fallback_used'] is False
        assert server.main is None
        server.call_tool('memory_release', {'session_id': session,
            'request_id': 'local-query', 'view_id': local['view_id']})

        status = server.call_tool('memory_status', {'session_id': session})
        durable = finish(server, session, server.call_tool('memory_context', {
            'session_id': session, 'request_id': 'main-query',
            'query': 'main_unique_9898',
            'expected_pair_snapshot_id': status['pair_snapshot_id']}))
        assert durable['candidate_count'] == 1
        assert durable['memory_layer'] == 'main' and durable['fallback_used'] is True
        assert durable['lookup_receipt']['session_candidate_count'] == 0
        server.call_tool('memory_release', {'session_id': session,
            'request_id': 'main-query', 'view_id': durable['view_id']})
    finally:
        server.close()
        stop(session_state, state)


def test_cached_layered_remote_reattaches_only_for_status(tmp_path):
    state, session = tmp_path / 'state', 'stale-session-resident'
    capture = SessionCapture(state)
    session_state = capture.session_root('codex', session)
    server = LayeredMCP(state)
    try:
        first = server.call_tool('memory_status', {'session_id': session})
        assert first['status'] == 'ready'
        stale = server.sessions[session].server.resident
        stale.close()
        stale.port = 1  # An unreachable retired daemon endpoint.
        with pytest.raises(InterfaceError, match='resident_request_failed'):
            server.call_tool('memory_context', {'session_id': session,
                'request_id': 'stale-view', 'query': 'missing cue',
                'expected_pair_snapshot_id': first['pair_snapshot_id']})
        assert (session, 'stale-view') not in server.request_leases
        recovered = server.call_tool('memory_status', {'session_id': session})
        assert recovered['status'] == 'ready'
        assert server.sessions[session].server.resident.port != 1
        packet = finish(server, session, server.call_tool('memory_context', {
            'session_id': session, 'request_id': 'recovered-view',
            'query': 'missing cue',
            'expected_pair_snapshot_id': recovered['pair_snapshot_id']}))
        assert packet['candidate_count'] == 0
        server.call_tool('memory_release', {'session_id': session,
            'request_id': 'recovered-view', 'view_id': packet['view_id']})
    finally:
        server.close()
        stop(session_state, state)


def test_exact_session_address_is_fail_closed_and_exposes_four_stage_receipt(tmp_path):
    state, session = tmp_path / 'state', 'exact-session-layer'
    capture = SessionCapture(state)
    session_state = capture.session_root('codex', session)
    with VRSClient(session_state, writes=True) as local:
        receipt = local.ingest_many([dict(request_id='exact-1', text='exact address body',
            source='test:exact', revision='1', outcome='pending')])
        address = receipt['results'][0]['episode_id']
    server = LayeredMCP(state)
    try:
        status = server.call_tool('memory_status', {'session_id': session})
        with pytest.raises(InterfaceError, match='memory_context_exact_address_mismatch'):
            server.call_tool('memory_context', {'session_id': session,
                'request_id': 'exact-bad', 'query': address.removeprefix('memory:'),
                'exact_episode_id': address,
                'expected_pair_snapshot_id': status['pair_snapshot_id']})
        assert ('exact-bad' not in {request_id for _, request_id in server.queries})
        with pytest.raises(InterfaceError, match='memory_context_exact_address_mismatch'):
            server.call_tool('memory_context', {'session_id': session,
                'request_id': 'exact-malformed', 'query': 'memory:not-an-address',
                'exact_episode_id': 'memory:not-an-address',
                'expected_pair_snapshot_id': status['pair_snapshot_id']})
        packet = finish(server, session, server.call_tool('memory_context', {
            'session_id': session, 'request_id': 'exact-good', 'query': address,
            'exact_episode_id': address,
            'expected_pair_snapshot_id': status['pair_snapshot_id']}))
        activation = packet['activation_receipt']
        assert packet['memory_layer'] == 'session' and packet['fallback_used'] is False
        assert packet['lookup_receipt']['main_opened'] is False
        assert [row['episode_id'] for row in packet['memories']] == [address]
        assert activation['invariant'] == 'validated_deja_vu_recall_replay_re_evidence'
        assert activation['stage_order']['data'] == [
            'deja_vu', 'recall', 'replay', 're_evidence']
        assert activation['admission_query_verified'] is True
        assert all(row['data'] == address for row in activation['stage_queries'].values())
        assert all(row['data'] == activation['snapshot_id']['data']
                   for row in activation['stage_snapshots'].values())
        assert all(row['data'] is False for row in activation['authority'].values())
        server.call_tool('memory_release', {'session_id': session,
            'request_id': 'exact-good', 'view_id': packet['view_id']})
    finally:
        server.close()
        stop(session_state, state)


def test_exact_main_address_is_forwarded_after_complete_session_miss(tmp_path):
    state, session = tmp_path / 'state', 'exact-main-layer'
    capture = SessionCapture(state)
    session_state = capture.session_root('codex', session)
    with VRSClient(session_state, writes=True) as local:
        local.ingest_many([dict(request_id='local-only', text='unrelated session body',
            source='test:local-only', revision='1', outcome='pending')])
    with VRSClient(state, writes=True) as main:
        receipt = main.ingest_many([dict(request_id='main-exact', text='durable exact body',
            source='test:main-exact', revision='1', outcome='pending')])
        address = receipt['results'][0]['episode_id']
    server = LayeredMCP(state)
    try:
        status = server.call_tool('memory_status', {'session_id': session})
        packet = finish(server, session, server.call_tool('memory_context', {
            'session_id': session, 'request_id': 'main-exact-query', 'query': address,
            'exact_episode_id': address,
            'expected_pair_snapshot_id': status['pair_snapshot_id']}))
        activation = packet['activation_receipt']
        assert packet['memory_layer'] == 'main' and packet['fallback_used'] is True
        assert packet['lookup_receipt']['session_candidate_count'] == 0
        assert packet['lookup_receipt']['main_opened'] is True
        assert [row['episode_id'] for row in packet['memories']] == [address]
        assert engine_recall_active(state)
        assert activation['invariant'] == 'validated_deja_vu_recall_replay_re_evidence'
        assert activation['admission_query_verified'] is True
        assert all(row['data'] == address for row in activation['stage_queries'].values())
        server.call_tool('memory_release', {'session_id': session,
            'request_id': 'main-exact-query', 'view_id': packet['view_id']})
        assert not engine_recall_active(state)
    finally:
        server.close()
        stop(session_state, state)


def test_live_capture_waits_for_exact_recall_then_admits_every_deferred_record(tmp_path):
    state, transcript = tmp_path / 'state', tmp_path / 'rollout.jsonl'
    session = 'recall-capture-barrier'
    write_transcript(transcript, session, [
        {'type': 'response_item', 'payload': {'type': 'message', 'role': 'user',
            'content': [{'type': 'input_text', 'text': 'barrier_anchor_unique_3131'}]}}])
    capture = SessionCapture(state)
    session_state = capture.session_root('codex', session)
    capture.scan_transcript('codex', session, transcript)
    with VRSClient(session_state, writes=False) as local:
        before = local.export(0)['rows']
    address = next(row['episode_id'] for row in before
                   if row['observation']['text'] == 'barrier_anchor_unique_3131')
    server = LayeredMCP(state)
    try:
        status = server.call_tool('memory_status', {'session_id': session})
        append_message(transcript, 'deferred_tool_record_unique_4141')
        deferred = capture.scan_transcript('codex', session, transcript)
        assert deferred['status'] == 'capture_deferred_for_memory_read'
        assert deferred['admitted'] == 0
        assert deferred['offset'] < transcript.stat().st_size
        assert engine_recall_active(session_state)
        # The resident normally starts idle consolidation after five seconds.
        # Keep the exact read open across that boundary: consolidation must be
        # deferred instead of replacing the pair returned by memory_status.
        time.sleep(6.5)

        packet = finish(server, session, server.call_tool('memory_context', {
            'session_id': session, 'request_id': 'barrier-exact-query',
            'query': address, 'exact_episode_id': address,
            'expected_pair_snapshot_id': status['pair_snapshot_id']}))
        assert packet['memory_layer'] == 'session'
        assert packet['lookup_receipt']['main_opened'] is False
        assert [row['episode_id'] for row in packet['memories']] == [address]
        assert packet['activation_receipt']['stage_order']['data'] == [
            'deja_vu', 'recall', 'replay', 're_evidence']
        server.call_tool('memory_release', {'session_id': session,
            'request_id': 'barrier-exact-query', 'view_id': packet['view_id']})

        admitted = capture.scan_transcript('codex', session, transcript)
        assert admitted['status'] == 'captured_to_session_vrs'
        assert admitted['admitted'] == 1
        assert admitted['offset'] == transcript.stat().st_size
        with VRSClient(session_state, writes=False) as local:
            after = local.export(0)['rows']
        assert 'deferred_tool_record_unique_4141' in {
            row['observation']['text'] for row in after}
        assert not list(capture.recall_root('codex', session).glob('*.json'))
        assert not engine_recall_active(session_state)
        assert not (state / 'memory.sqlite3').exists()
    finally:
        server.close()
        stop(session_state, state)


def test_failed_exact_recall_releases_capture_barrier_without_losing_tail(tmp_path):
    state, transcript = tmp_path / 'state', tmp_path / 'rollout.jsonl'
    session = 'failed-recall-capture-barrier'
    write_transcript(transcript, session, [
        {'type': 'response_item', 'payload': {'type': 'message', 'role': 'user',
            'content': [{'type': 'input_text', 'text': 'failed_barrier_anchor_5151'}]}}])
    capture = SessionCapture(state)
    session_state = capture.session_root('codex', session)
    capture.scan_transcript('codex', session, transcript)
    server = LayeredMCP(state)
    try:
        status = server.call_tool('memory_status', {'session_id': session})
        append_message(transcript, 'tail_after_failed_recall_6161')
        assert capture.scan_transcript('codex', session, transcript)[
            'status'] == 'capture_deferred_for_memory_read'
        with pytest.raises(InterfaceError, match='memory_context_exact_address_mismatch'):
            server.call_tool('memory_context', {'session_id': session,
                'request_id': 'failed-exact-query', 'query': 'memory:not-an-address',
                'exact_episode_id': 'memory:not-an-address',
                'expected_pair_snapshot_id': status['pair_snapshot_id']})

        admitted = capture.scan_transcript('codex', session, transcript)
        assert admitted['admitted'] == 1
        with VRSClient(session_state, writes=False) as local:
            exported = local.export(0)['rows']
        assert 'tail_after_failed_recall_6161' in {
            row['observation']['text'] for row in exported}
        assert not list(capture.recall_root('codex', session).glob('*.json'))
    finally:
        server.close()
        stop(session_state, state)


def test_failed_remote_release_and_server_close_both_resume_live_capture(tmp_path,
                                                                         monkeypatch):
    state, transcript = tmp_path / 'state', tmp_path / 'rollout.jsonl'
    session = 'release-failure-capture-barrier'
    write_transcript(transcript, session, [
        {'type': 'response_item', 'payload': {'type': 'message', 'role': 'user',
            'content': [{'type': 'input_text', 'text': 'release_anchor_unique_8181'}]}}])
    capture = SessionCapture(state)
    session_state = capture.session_root('codex', session)
    capture.scan_transcript('codex', session, transcript)
    server = LayeredMCP(state)
    try:
        status = server.call_tool('memory_status', {'session_id': session})
        packet = finish(server, session, server.call_tool('memory_context', {
            'session_id': session, 'request_id': 'release-failure-query',
            'query': 'release_anchor_unique_8181',
            'expected_pair_snapshot_id': status['pair_snapshot_id']}))
        append_message(transcript, 'tail_after_release_failure_9191')
        assert capture.scan_transcript('codex', session, transcript)[
            'status'] == 'capture_deferred_for_memory_read'

        remote = server.sessions[session]
        original_call = remote.call
        def fail_release(name, arguments):
            if name == 'memory_release':
                raise InterfaceError('synthetic_release_failure')
            return original_call(name, arguments)
        monkeypatch.setattr(remote, 'call', fail_release)
        with pytest.raises(InterfaceError, match='synthetic_release_failure'):
            server.call_tool('memory_release', {'session_id': session,
                'request_id': 'release-failure-query', 'view_id': packet['view_id']})
        assert capture.scan_transcript('codex', session, transcript)['admitted'] == 1
        assert not list(capture.recall_root('codex', session).glob('*.json'))

        server.call_tool('memory_status', {'session_id': session})
        append_message(transcript, 'tail_after_server_close_0202')
        assert capture.scan_transcript('codex', session, transcript)[
            'status'] == 'capture_deferred_for_memory_read'
        server.close()
        assert capture.scan_transcript('codex', session, transcript)['admitted'] == 1
        with VRSClient(session_state, writes=False) as local:
            texts = {row['observation']['text'] for row in local.export(0)['rows']}
        assert {'tail_after_release_failure_9191',
                'tail_after_server_close_0202'} <= texts
    finally:
        server.close()
        stop(session_state, state)


def test_session_end_clears_abandoned_recall_and_preserves_final_tail(tmp_path):
    state, transcript = tmp_path / 'state', tmp_path / 'rollout.jsonl'
    session = 'ended-recall-capture-barrier'
    write_transcript(transcript, session, [])
    capture = SessionCapture(state)
    session_state = capture.session_root('codex', session)
    capture.scan_transcript('codex', session, transcript)
    capture.begin_recall('codex', session)
    append_message(transcript, 'session_end_deferred_tail_7171')
    assert capture.scan_transcript('codex', session, transcript)[
        'status'] == 'capture_deferred_for_memory_read'
    try:
        result = finalize(state, 'codex', session, transcript)
        assert result['offset'] == transcript.stat().st_size
        assert not list(capture.recall_root('codex', session).glob('*.json'))
        with VRSClient(state, writes=False) as main:
            exported = main.export(0)['rows']
        assert 'session_end_deferred_tail_7171' in {
            row['observation']['text'] for row in exported}
        assert not (state / 'memory.sqlite3').exists()
    finally:
        stop(session_state, state)


def test_merge_requires_end_and_preserves_original_address(tmp_path):
    state, transcript = tmp_path / 'state', tmp_path / 'rollout.jsonl'
    session = 'ended-session'
    write_transcript(transcript, session, [
        {'type': 'response_item', 'payload': {'type': 'message', 'role': 'user',
            'content': [{'type': 'input_text', 'text': 'merge_unique_5151'}]}}])
    capture = SessionCapture(state)
    session_state = capture.session_root('codex', session)
    try:
        capture.scan_transcript('codex', session, transcript)
        assert not (state / 'memory.sqlite3').exists()
        with VRSClient(session_state, writes=False) as local:
            page = local.export(0)
        addresses = [row['episode_id'] for row in page['rows']]
        assert addresses
        assert capture.merge_ended() == 0
        assert not (state / 'memory.sqlite3').exists()
        capture.mark_ended('codex', session)
        assert capture.merge_ended() == len(addresses)
        assert not (state / 'memory.sqlite3').exists()
        registry = json.loads((state / 'linked-shards.json').read_text(encoding='utf-8'))
        assert registry['schema'] == 'swegca-vrs2-linked-shards-v1'
        assert registry['shards'][0]['records'] == len(addresses)
        assert registry['shards'][0]['path'].startswith('session-vrs/codex/')
        with VRSClient(state, writes=False) as main:
            exported = main.export(0)
        assert [row['episode_id'] for row in exported['rows']] == addresses
    finally:
        stop(session_state, state)


def test_armed_runtime_upgrade_waits_for_end_then_preserves_native_vrs_stores(tmp_path):
    state, transcript = tmp_path / 'state', tmp_path / 'rollout.jsonl'
    session = 'upgrade-session'
    write_transcript(transcript, session, [
        {'type': 'response_item', 'payload': {'type': 'message', 'role': 'user',
            'content': [{'type': 'input_text', 'text': 'upgrade_unique_6262'}]}}])
    capture = SessionCapture(state)
    session_state = capture.session_root('codex', session)
    try:
        capture.scan_transcript('codex', session, transcript)
        assert is_native_store(session_state) and not is_native_store(state)
        arm_upgrade(state)
        assert merge_run(state) == 0
        assert not is_native_store(state)

        # Keep a live durable-main resident before SessionEnd. After adoption it
        # owns the linked session shard, so upgrade must release main first.
        with VRSClient(state, writes=True) as main:
            assert main.call('memory_status', {})['hot_episode_count'] == 0

        capture.mark_ended('codex', session)
        assert merge_run(state) == 0
        # The complete session VRS is adopted in place.  Merge creates no
        # duplicate primary experience rows. Runtime preparation opens the
        # existing empty primary control store and keeps all experience linked.
        assert is_native_store(state) and is_native_store(session_state)
        registry = json.loads((state / 'linked-shards.json').read_text(encoding='utf-8'))
        assert registry['shards'][0]['records'] == 2
        assert (state / 'exact-replay' / 'capsules.vrs').is_file()
        assert not (session_state / 'exact-replay').exists()
        assert not (state / 'session-capture' / 'runtime-upgrade.json').exists()
        receipts = list((state / 'session-capture' / 'runtime-upgrades').glob('*.json'))
        assert len(receipts) == 1

        receipt = json.loads(receipts[0].read_text(encoding='utf-8'))
        assert receipt['exact']['complete'] and receipt['projections']['complete']
        # Both old residents released ownership; the current runtime can open
        # the preserved experiences from the rebuilt current exact directory.
        with VRSClient(state, writes=False) as main:
            status = main.call('memory_status', {})
            exported = main.export(0)
        assert status['hot_episode_count'] == 0
        assert status['logical_episode_count'] == registry['shards'][0]['records']
        assert 'upgrade_unique_6262' in {
            row['observation']['text'] for row in exported['rows']}
        assert len(exported['rows']) == registry['shards'][0]['records']
        assert (state / 'exact-replay' / 'capsules.vrs').is_file()
    finally:
        stop(session_state, state)


def test_ended_partitioned_session_attaches_every_vrs_shard_and_recalls_cold(tmp_path):
    state, session = tmp_path / 'state', 'partitioned-session'
    capture = SessionCapture(state)
    session_state = capture.session_root('codex', session)
    primary = Main(session_state, allow_ingest=True, bundle_limit=2,
                   defer_checkpoints=True)
    session_vrs = Resident(primary, {}, hot_limit=1, bundle_limit=2)
    addresses = []
    try:
        receipt = session_vrs.ingest_many([dict(request_id=f'partition-{index}',
            text=f'linked_partition_anchor experience {index}',
            source=f'test:partition:{index}', revision='1', outcome='pending')
            for index in range(5)])
        addresses = [row['episode_id'] for row in receipt['results']]
    finally:
        session_vrs.close()
        primary.close()

    capture.mark_ended('codex', session)
    assert capture.merge_ended() == 5
    registry = json.loads((state / 'linked-shards.json').read_text(encoding='utf-8'))
    assert len(registry['shards']) == 3
    assert sum(row['records'] for row in registry['shards']) == 5
    assert sorted(Path(row['path']).name for row in registry['shards']) == [
        capture.session_key(session), 'shard-000001', 'shard-000002']

    main = Main(state, allow_ingest=False, defer_checkpoints=True)
    resident = Resident(main, {}, hot_limit=0)
    sharded = ShardedMain(main, resident)
    try:
        assert set(resident.ids()) == {row['id'] for row in registry['shards']}
        exact = resident.backfill_exact(100)
        while not exact['complete']:
            exact = resident.backfill_exact(100)
        projections = resident.backfill_projections(1)
        while not projections['complete']:
            projections = resident.backfill_projections(1)
        assert exact['complete'] and projections['complete']
        root = sharded.recall('linked_partition_anchor', resident.logical_snapshot())
        activation = root['receipt']['activation']
        assert activation.stage_order == ('deja_vu', 'recall', 'replay', 're_evidence')
        assert {row.episode_id for row in activation.replay.episodes} == set(addresses)
        direct = sharded.recall(addresses[-1], resident.logical_snapshot())
        assert direct['receipt']['activation'].replay.episodes[0].episode_id == addresses[-1]
    finally:
        resident.close()
        main.close()


def test_main_extends_attached_source_lineage_and_registry_survives_restart(tmp_path):
    state, transcript = tmp_path / 'state', tmp_path / 'rollout.jsonl'
    session = 'linked-revision-session'
    write_transcript(transcript, session, [
        {'type': 'response_item', 'payload': {'type': 'message', 'role': 'user',
            'content': [{'type': 'input_text', 'text': 'linked revision one'}]}}])
    capture = SessionCapture(state)
    session_state = capture.session_root('codex', session)
    try:
        capture.scan_transcript('codex', session, transcript)
        with VRSClient(session_state, writes=False) as local:
            rows = local.export(0)['rows']
        original = next(row for row in rows
                        if row['observation']['text'] == 'linked revision one')
        capture.mark_ended('codex', session)
        assert capture.merge_ended() == 2

        main = Main(state, allow_ingest=True, defer_checkpoints=True)
        resident = Resident(main, {}, hot_limit=1)
        try:
            while not resident.backfill_exact(100)['complete']:
                pass
            second = dict(original['observation'], request_id='linked-revision-two',
                          text='linked revision two', revision='2',
                          supersedes=original['episode_id'])
            receipt = resident.ingest(second)
            assert receipt['episode_id'] != original['episode_id']
            assert resident.exact_backfill[next(iter(resident.linked))]['count'] == 3
            registry = json.loads((state / 'linked-shards.json').read_text(encoding='utf-8'))
            linked = registry['shards'][0]
            assert linked['records'] == 3
            assert linked['pair_snapshot_id'] == resident.pair_ids[linked['id']]
        finally:
            resident.close()
            main.close()

        restored_main = Main(state, allow_ingest=False, defer_checkpoints=True)
        restored = Resident(restored_main, {}, hot_limit=0)
        try:
            while not restored.backfill_exact(100)['complete']:
                pass
            while not restored.backfill_projections(1)['complete']:
                pass
            direct = ShardedMain(restored_main, restored).recall(
                receipt['episode_id'], restored.logical_snapshot())
            assert direct['receipt']['activation'].replay.episodes[0].steps[0].observation[
                'text'] == 'linked revision two'
        finally:
            restored.close()
            restored_main.close()
    finally:
        stop(session_state, state)


def test_linked_shard_recovers_committed_journal_after_unclean_main_exit(tmp_path):
    state, transcript = tmp_path / 'state', tmp_path / 'rollout.jsonl'
    session = 'linked-crash-session'
    write_transcript(transcript, session, [
        {'type': 'response_item', 'payload': {'type': 'message', 'role': 'user',
            'content': [{'type': 'input_text', 'text': 'linked crash revision one'}]}}])
    capture = SessionCapture(state)
    session_state = capture.session_root('codex', session)
    try:
        capture.scan_transcript('codex', session, transcript)
        with VRSClient(session_state, writes=False) as local:
            original = next(row for row in local.export(0)['rows']
                            if row['observation']['text'] == 'linked crash revision one')
        capture.mark_ended('codex', session)
        capture.merge_ended()
        payload = dict(original['observation'], request_id='linked-crash-two',
                       text='linked crash revision two', revision='2',
                       supersedes=original['episode_id'])
        payload_path, receipt_path = tmp_path / 'payload.json', tmp_path / 'receipt.json'
        payload_path.write_text(json.dumps(payload), encoding='utf-8')
        script = tmp_path / 'crash_writer.py'
        script.write_text('''
import json, os, sys
from pathlib import Path
from swegca_vrs2.store import Main
from swegca_vrs2.resident import Resident
root, payload, receipt = map(Path, sys.argv[1:])
main = Main(root, allow_ingest=True, defer_checkpoints=True)
resident = Resident(main, {}, hot_limit=1)
result = resident.ingest(json.loads(payload.read_text(encoding="utf-8")))
receipt.write_text(json.dumps(result), encoding="utf-8")
os._exit(0)
''', encoding='utf-8')
        environment = dict(os.environ, PYTHONPATH=str(Path(__file__).parents[2] / 'src'))
        subprocess.run([sys.executable, str(script), str(state), str(payload_path),
                        str(receipt_path)], check=True, env=environment, timeout=30)
        receipt = json.loads(receipt_path.read_text(encoding='utf-8'))
        registry = json.loads((state / 'linked-shards.json').read_text(encoding='utf-8'))
        linked = registry['shards'][0]
        linked_store = NativeJournal(state / linked['path'])
        checkpoint_pair = linked_store.read_checkpoint().pair
        journal_pair = linked_store.head()[1]
        assert checkpoint_pair != journal_pair == linked['pair_snapshot_id']

        main = Main(state, allow_ingest=False, defer_checkpoints=True)
        resident = Resident(main, {}, hot_limit=0)
        try:
            repaired = json.loads((state / 'linked-shards.json').read_text(encoding='utf-8'))[
                'shards'][0]
            repaired_store = NativeJournal(state / repaired['path'])
            assert repaired_store.read_checkpoint().pair == repaired['pair_snapshot_id']
            while not resident.backfill_exact(100)['complete']:
                pass
            while not resident.backfill_projections(1)['complete']:
                pass
            recalled = ShardedMain(main, resident).recall(
                receipt['episode_id'], resident.logical_snapshot())
            assert recalled['receipt']['activation'].replay.episodes[0].steps[0].observation[
                'text'] == 'linked crash revision two'
        finally:
            resident.close()
            main.close()
    finally:
        stop(session_state, state)
