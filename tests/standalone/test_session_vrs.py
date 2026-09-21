import json
from pathlib import Path

from swegca_vrs2.layered import LayeredMCP
from swegca_vrs2.loopback import ensure_daemon
from swegca_vrs2.conversation_merge import run as merge_run
from swegca_vrs2.runtime_upgrade import arm as arm_upgrade
from swegca_vrs2.session_capture import SessionCapture, VRSClient, handle


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
        with VRSClient(state, writes=False) as main:
            exported = main.export(0)
        assert [row['episode_id'] for row in exported['rows']] == addresses
    finally:
        stop(session_state, state)


def test_armed_runtime_upgrade_waits_for_end_then_preserves_vrs_databases(tmp_path):
    state, transcript = tmp_path / 'state', tmp_path / 'rollout.jsonl'
    session = 'upgrade-session'
    write_transcript(transcript, session, [
        {'type': 'response_item', 'payload': {'type': 'message', 'role': 'user',
            'content': [{'type': 'input_text', 'text': 'upgrade_unique_6262'}]}}])
    capture = SessionCapture(state)
    session_state = capture.session_root('codex', session)
    try:
        capture.scan_transcript('codex', session, transcript)
        main_db = state / 'memory.sqlite3'
        session_db = session_state / 'memory.sqlite3'
        assert session_db.is_file() and not main_db.exists()
        arm_upgrade(state)
        assert merge_run(state) == 0
        assert not main_db.exists()

        capture.mark_ended('codex', session)
        assert merge_run(state) == 0
        assert main_db.is_file() and session_db.is_file()
        assert not (state / 'exact-replay').exists()
        assert not (session_state / 'exact-replay').exists()
        assert not (state / 'session-capture' / 'runtime-upgrade.json').exists()
        receipts = list((state / 'session-capture' / 'runtime-upgrades').glob('*.json'))
        assert len(receipts) == 1

        # Both old residents released ownership; the current runtime can open
        # the preserved experiences and recreate its derived V5 directory.
        with VRSClient(state, writes=False) as main:
            exported = main.export(0)
        assert 'upgrade_unique_6262' in {
            row['observation']['text'] for row in exported['rows']}
        assert (state / 'exact-replay' / 'capsules.vrs').is_file()
    finally:
        stop(session_state, state)
