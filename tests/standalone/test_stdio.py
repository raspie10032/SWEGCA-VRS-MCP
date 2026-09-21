import json
import os
from pathlib import Path
import subprocess
import sys
from time import perf_counter_ns

from mcp import Client, StdioServerParameters
import pytest


@pytest.mark.parametrize('mode',['auto','legacy'])
async def test_actual_sdk_stdio_ingest_context_exit_restart(tmp_path, mode):
    directory = tmp_path / '윈도우 기억 with spaces'
    parameters = StdioServerParameters(command=sys.executable,
        args=['-m','swegca_vrs2.server','--state-dir',str(directory),'--allow-ingest'])
    rows=[]
    async with Client(parameters,mode=mode,read_timeout_seconds=20) as client:
        names={t.name for t in (await client.list_tools()).tools}
        assert len(names)==9 and 'memory_store' in names
        for i,outcome in enumerate(('success','failure','negative','uncertain','conflict','pending')):
            result=await client.call_tool('memory_store',dict(request_id=str(i),
                text='독립 실행 검증 원문 '+outcome,source='test:stdio:'+str(i),revision='r1',outcome=outcome))
            assert not result.is_error, result
            rows.append(result.structured_content['episode_id'])
        status=(await client.call_tool('memory_status',{})).structured_content
        assert status['native_engine_bundled'] and status['internal_llm_calls']==0
        context=await client.call_tool('memory_context',dict(request_id='q',query='독립 실행',
            expected_pair_snapshot_id=status['pair_snapshot_id'],page_size=8))
        assert not context.is_error, context
        packet=context.structured_content
        assert packet['candidate_count']==6 and len(packet['memories'])==6
        assert all(row['complete'] for row in packet['memories'])
        assert {row['episode_id'] for row in packet['memories']}==set(rows)
        # Deliberately rely on EOF cleanup for the open request.
    async with Client(parameters,mode=mode,read_timeout_seconds=20) as client:
        after=(await client.call_tool('memory_status',{})).structured_content
        assert after['identity']==status['identity']
        assert after['pair_snapshot_id']==status['pair_snapshot_id']
        result=await client.call_tool('memory_context',dict(request_id='restarted',query='독립 실행',expected_pair_snapshot_id=after['pair_snapshot_id'],page_size=8))
        assert not result.is_error and result.structured_content['candidate_count']==6
        for row in result.structured_content['memories']:
            data=row['replay']['data']
            assert data['steps'][0]['observation']['text'].startswith('독립 실행 검증 원문 ')
            assert data['source_addresses'][0].startswith('test:stdio:')
        handle={k:result.structured_content[k] for k in ('request_id','view_id')}
        release=await client.call_tool('memory_release',handle)
        assert not release.is_error and release.structured_content['experience_deleted'] is False


def test_readonly_cli_invalid_json_and_normal_exit(tmp_path):
    messages=[dict(jsonrpc='2.0',id=1,method='initialize',params=dict(protocolVersion='2025-06-18',clientInfo={'name':'test','version':'1'},capabilities={})),
              dict(jsonrpc='2.0',method='notifications/initialized'),
              dict(jsonrpc='2.0',id=2,method='tools/list',params={}),
              dict(jsonrpc='2.0',id=3,method='tools/call',params=dict(name='memory_store',arguments=dict(request_id='x',text='a',source='s',revision='r')))]
    raw=b'not json\n'+b''.join(json.dumps(m).encode()+b'\n' for m in messages)
    result=subprocess.run([sys.executable,'-m','swegca_vrs2.server','--state-dir',str(tmp_path)],input=raw,capture_output=True,timeout=20)
    assert result.returncode==0, result.stderr
    replies=[json.loads(line) for line in result.stdout.splitlines()]
    assert replies[0]['error']['code']==-32700
    catalog=next(r for r in replies if r.get('id')==2)['result']['tools']
    assert len(catalog)==8 and 'memory_store' not in {t['name'] for t in catalog}
    assert replies[-1]['result']['isError']


def test_console_entrypoint_is_standalone():
    from importlib.metadata import distribution
    entries={e.name:e.value for e in distribution('swegca-vrs-mcp').entry_points}
    assert entries=={'swegca-vrs-mcp':'swegca_vrs2.server:main','swegca-vrs2-mcp':'swegca_vrs2.server:main'}


def test_bridge_client_rebuilds_after_a_daemon_restart(monkeypatch):
    """2026-09-21: the daemon restarted under a live session and every MCP call failed for the rest of it; the
    bridge's client now re-resolves the daemon (port file, spawn) once and retries."""
    import swegca_vrs2.loopback as loopback
    from swegca_vrs2.native_transport import InterfaceError
    from swegca_vrs2.server import ReconnectingClient

    class Dead:
        closed = False

        def request(self, command, **arguments):
            raise InterfaceError('resident_request_failed')

        def close(self):
            self.closed = True

    class Alive:
        def request(self, command, **arguments):
            return dict(status='ok', command=command, **arguments)

        def close(self):
            pass

    made = []

    def ensure(state_dir, allow_ingest=True, **kw):
        made.append(state_dir)
        return Dead() if len(made) == 1 else Alive()

    monkeypatch.setattr(loopback, 'ensure_daemon', ensure)
    client = ReconnectingClient('C:/tmp/state', True)
    first = client.client
    assert client.request('status', x=1) == dict(status='ok', command='status', x=1)
    assert len(made) == 2 and first.closed and isinstance(client.client, Alive)
    assert client.request('ping') == dict(status='ok', command='ping') and len(made) == 2   # no rebuild when it works

    class Refusing:
        def request(self, command, **arguments):
            raise InterfaceError('memory_context_start_requires_query_and_snapshot')

        def close(self):
            pass
    client.client = Refusing()
    import pytest
    with pytest.raises(InterfaceError):                    # a refusal is not a dead daemon: no rebuild, no retry
        client.request('memory_context')
    assert len(made) == 2

