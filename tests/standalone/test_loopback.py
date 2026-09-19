"""A real stdio bridge and two clients share one local main process."""
import asyncio
import json
import socket
import subprocess
import sys
import time

from mcp import Client, StdioServerParameters
import pytest

from swegca_vrs2.loopback import _connect, _endpoint, stop_resident
from swegca_vrs2.store import Main


@pytest.mark.asyncio
async def test_two_stdio_clients_share_one_resident_and_readonly_bridge(tmp_path):
    state = tmp_path / 'shared Windows style state'
    resident = subprocess.Popen([sys.executable, '-m', 'swegca_vrs2.loopback',
        '--daemon', '--state-dir', str(state), '--allow-ingest'],
        stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    try:
        deadline = time.monotonic() + 10
        while True:
            if resident.poll() is not None:
                raise AssertionError('resident exited: ' + resident.stderr.read().decode('utf-8'))
            try:
                connection = _connect(state, allow_ingest=False)
                connection.close()
                break
            except OSError:
                if time.monotonic() > deadline:
                    raise AssertionError('resident did not become ready')
                await asyncio.sleep(.05)
        endpoint = _endpoint(state)
        with socket.create_connection(('127.0.0.1', endpoint['port']), timeout=3) as unauthenticated:
            unauthenticated.sendall(json.dumps({'secret': '0' * 64,
                'allow_ingest': True}).encode('utf-8') + b'\n')
            assert unauthenticated.recv(1) == b''
        writable = StdioServerParameters(command=sys.executable,
            args=['-m', 'swegca_vrs2.server', '--loopback', '--state-dir', str(state), '--allow-ingest'])
        readonly = StdioServerParameters(command=sys.executable,
            args=['-m', 'swegca_vrs2.server', '--loopback', '--state-dir', str(state)])
        async with Client(writable, read_timeout_seconds=15) as writer:
            async with Client(readonly, read_timeout_seconds=15) as reader:
                writer_status = (await writer.call_tool('memory_status', {})).structured_content
                reader_status = (await reader.call_tool('memory_status', {})).structured_content
                assert writer_status['identity'] == reader_status['identity']
                assert writer_status['write_tools_exported'] is True
                assert reader_status['write_tools_exported'] is False
                assert 'memory_store' not in {tool.name for tool in (await reader.list_tools()).tools}
                stored = await writer.call_tool('memory_store', dict(request_id='shared-one',
                    text='원문 270인지', source='test:shared', revision='1'))
                assert not stored.is_error
                current = (await reader.call_tool('memory_status', {})).structured_content
                assert current['pair_snapshot_id'] != reader_status['pair_snapshot_id']
                context = await reader.call_tool('memory_context', dict(request_id='read-one',
                    query='270', expected_pair_snapshot_id=current['pair_snapshot_id']))
                assert not context.is_error
                assert context.structured_content['memories'][0]['episode_id'] == stored.structured_content['episode_id']
                handle = {key: context.structured_content[key] for key in ('request_id', 'view_id')}
                released = await reader.call_tool('memory_release', handle)
                assert not released.is_error
                for number in range(1, 8):
                    added = await writer.call_tool('memory_store', dict(request_id=f'shared-{number}',
                        text=f'별도 원문 {number}', source=f'test:shared:{number}', revision='1'))
                    assert not added.is_error
                deadline = time.monotonic() + 10
                while True:
                    status = (await reader.call_tool('memory_status', {})).structured_content
                    if status['checkpoint_sequence'] >= 8:
                        break
                    assert time.monotonic() < deadline, status
                    await asyncio.sleep(.1)
        assert stop_resident(state)['status']=='stopping'
        assert resident.wait(timeout=10)==0
    finally:
        if resident.poll() is None:
            resident.terminate()
            try:
                resident.communicate(timeout=5)
            except subprocess.TimeoutExpired:
                resident.kill()
                resident.communicate(timeout=5)
    reopened = Main(state)
    try:
        assert reopened.status()['hot_episode_count'] == 8
        assert reopened.status()['checkpoint_sequence'] == 8
        assert reopened.recall('270', reopened.pair.snapshot_id)['receipt']['activation'].recall.candidates
    finally:
        reopened.close()


def test_stdio_bridge_starts_resident_on_demand(tmp_path):
    state = tmp_path / 'on-demand'
    messages = [
        dict(jsonrpc='2.0', id=1, method='initialize', params=dict(
            protocolVersion='2025-06-18', clientInfo={'name': 'test', 'version': '1'},
            capabilities={})),
        dict(jsonrpc='2.0', method='notifications/initialized'),
        dict(jsonrpc='2.0', id=2, method='tools/call', params=dict(
            name='memory_status', arguments={})),
    ]
    raw = b''.join(json.dumps(item).encode('utf-8') + b'\n' for item in messages)
    result = subprocess.run([sys.executable, '-m', 'swegca_vrs2.server',
        '--loopback', '--state-dir', str(state)], input=raw,
        capture_output=True, timeout=20)
    try:
        assert result.returncode == 0, result.stderr
        replies = [json.loads(line) for line in result.stdout.splitlines()]
        assert [reply['id'] for reply in replies] == [1, 2]
        status = replies[1]['result']['structuredContent']
        assert status['identity'] and status['hot_episode_count'] == 0
        assert status['write_tools_exported'] is False
    finally:
        deadline = time.monotonic() + 5
        while True:
            try:
                stop_resident(state)
                break
            except ValueError as exc:
                if str(exc) != 'resident_clients_active' or time.monotonic() > deadline:
                    raise
                time.sleep(.05)
