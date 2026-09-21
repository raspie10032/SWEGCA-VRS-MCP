"""Installed Codex stdio smoke for live session VRS routing and SessionEnd attach."""
from __future__ import annotations

import argparse
import asyncio
import json
from pathlib import Path
import tempfile

from mcp import Client, StdioServerParameters

from swegca_vrs2.linked_shards import shutdown_and_release
from swegca_vrs2.session_capture import SessionCapture, VRSClient


async def ready(client, session, request_id, query, snapshot):
    result = await client.call_tool('memory_context', {
        'session_id': session, 'request_id': request_id,
        'query': query, 'expected_pair_snapshot_id': snapshot})
    if result.is_error:
        raise RuntimeError(f'memory_context failed: {result}')
    packet = result.structured_content
    for _ in range(16):
        if packet.get('status') == 'memory_context_ready':
            return packet
        result = await client.call_tool('memory_context', {
            'session_id': session, 'request_id': request_id,
            'view_id': packet['view_id']})
        if result.is_error:
            raise RuntimeError(f'memory_context continuation failed: {result}')
        packet = result.structured_content
    raise RuntimeError('memory_context remained pending')


async def release(client, session, packet):
    result = await client.call_tool('memory_release', {
        'session_id': session, 'request_id': packet['request_id'],
        'view_id': packet['view_id']})
    if result.is_error:
        raise RuntimeError(f'memory_release failed: {result}')


async def run(args):
    temporary = tempfile.TemporaryDirectory(prefix='swegca-vrs2-codex-smoke-')
    root = Path(temporary.name)
    state, transcript = root / 'state', root / 'rollout.jsonl'
    session = 'codex-smoke-session'
    rows = [
        {'type': 'session_meta', 'payload': {'id': session}},
        {'type': 'response_item', 'payload': {'type': 'message', 'role': 'user',
            'content': [{'type': 'input_text', 'text': 'session_unique_7281'}]}},
        {'type': 'response_item', 'payload': {'type': 'function_call',
            'name': 'diagnostic', 'call_id': 'smoke-call', 'arguments': '{}'}},
    ]
    transcript.write_text(''.join(json.dumps(row, ensure_ascii=False) + '\n'
                                  for row in rows), encoding='utf-8')
    capture = SessionCapture(state)
    session_state = capture.session_root('codex', session)
    try:
        ingress = capture.scan_transcript('codex', session, transcript)
        with VRSClient(state, writes=True) as main:
            main.ingest_many([dict(request_id='durable-smoke',
                text='durable_unique_8362', source='diagnostic:durable',
                revision='1', outcome='pending')])

        parameters = StdioServerParameters(
            command=str(args.server), args=['--state-dir', str(state)])
        async with Client(parameters, mode=args.mode, read_timeout_seconds=60) as client:
            names = {tool.name for tool in (await client.list_tools()).tools}
            expected = {'memory_context', 'memory_status', 'memory_recall',
                'memory_continue', 'memory_resume', 'memory_read',
                'memory_read_path', 'memory_release'}
            if names != expected:
                raise RuntimeError(f'unexpected Codex tool catalog: {sorted(names)}')
            status = (await client.call_tool('memory_status', {
                'session_id': session})).structured_content
            local = await ready(client, session, 'local-smoke',
                                'session_unique_7281', status['pair_snapshot_id'])
            if local.get('memory_layer') != 'session' or local.get('fallback_used') is not False:
                raise RuntimeError('session experience did not win session-first lookup')
            await release(client, session, local)

            status = (await client.call_tool('memory_status', {
                'session_id': session})).structured_content
            durable = await ready(client, session, 'durable-smoke-query',
                                  'durable_unique_8362', status['pair_snapshot_id'])
            if durable.get('memory_layer') != 'main' or durable.get('fallback_used') is not True:
                raise RuntimeError('complete session miss did not fall back to main')
            if durable.get('lookup_receipt', {}).get('session_candidate_count') != 0:
                raise RuntimeError('main opened before a complete session miss')
            await release(client, session, durable)

        capture.mark_ended('codex', session)
        attached = capture.merge_ended()
        with VRSClient(state, writes=False) as main:
            exported = main.export(0)
        texts = {row['observation']['text'] for row in exported['rows']}
        if 'session_unique_7281' not in texts or 'durable_unique_8362' not in texts:
            raise RuntimeError('SessionEnd attachment lost original experience')
        database_files = [str(path.relative_to(state)) for path in state.rglob('*')
                          if path.is_file() and path.name.casefold().endswith(
                              ('.sqlite', '.sqlite3', '.db'))]
        if database_files:
            raise RuntimeError(f'database artifacts created: {database_files}')
        print(json.dumps({
            'status': 'PASS', 'transport': 'stdio', 'session_ingress': ingress,
            'session_layer': local['memory_layer'], 'fallback_layer': durable['memory_layer'],
            'session_miss_before_main': True, 'attached_experiences': attached,
            'database_artifacts': database_files, 'internal_llm_calls': 0,
        }, ensure_ascii=False, indent=2))
    finally:
        shutdown_and_release(state)
        shutdown_and_release(session_state)
        temporary.cleanup()


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--server', type=Path, required=True)
    parser.add_argument('--mode', choices=['auto', 'legacy'], default='auto')
    asyncio.run(run(parser.parse_args()))
