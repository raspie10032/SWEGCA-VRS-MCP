import io
import json
import subprocess
import sys
from pathlib import Path

from mcp import Client, StdioServerParameters
import pytest

from swegca_vrs_mcp.native_memory import MemoryMCPServer
from swegca_vrs_mcp.native_transport import ResidentClient, InterfaceError, encode, decode, MAX_BYTES
from tests.native_fixture import native_peer, OUTCOMES

ROOT=Path(__file__).resolve().parents[1]


def start(server, **kwargs):
    return server.call_tool('memory_context',dict(request_id='test',query='synthetic',
        expected_pair_snapshot_id='a'*64,**kwargs))


def test_native_context_original_records_all_outcomes_and_cleanup(tmp_path):
    path=tmp_path/'native.sock'
    with native_peer(path) as peer:
        server=MemoryMCPServer(ResidentClient(path))
        p=start(server,page_size=2);rows=[]
        while True:
            assert p['status']=='memory_context_ready',p
            assert p['internal_llm_calls']==p['final_utterance_calls']==0
            assert not p['grants_authority'] and not p['coverage']['whole_memory_search_complete']
            for row in p['memories']:
                assert row['complete']
                assert row['replay']['data']==peer.replay[row['index']]
                assert row['candidate']['data']==peer.candidates[row['index']]
                assert row['re_evidence']['data']==peer.verdicts[row['index']]
            rows.extend(p['memories'])
            assert not peer.sessions['test']['cursors']
            if not p['next_call']:break
            p=server.call_tool(p['next_call']['tool'],p['next_call']['arguments'])
            assert p['transport_turns']==0
        assert len(rows)==6 and {r['replay']['data']['steps'][0]['outcome'] for r in rows}==set(OUTCOMES)
        assert peer.commands.count('cognitive_dialogue_start')==1
        assert peer.commands.count('cognitive_dialogue_continue')==2
        assert server.close()==[] and not peer.sessions
        assert len(peer.replay)==6


@pytest.mark.parametrize('mode',['auto','legacy'])
@pytest.mark.parametrize('entry',[['swegca_vrs_mcp.native_memory','--socket'],
                                ['swegca_vrs_mcp.server','--native-socket']])
async def test_real_sdk_stdio_native_modes_and_eof_cleanup(tmp_path,mode,entry):
    path=tmp_path/'native.sock'
    with native_peer(path) as peer:
        parameters=StdioServerParameters(command=sys.executable,args=['-m',*entry,str(path)],
            env={'PYTHONPATH':str(ROOT/'src')})
        async with Client(parameters,mode=mode,read_timeout_seconds=20) as client:
            tools=(await client.list_tools()).tools
            assert len(tools)==8 and 'memory_context' in {t.name for t in tools}
            assert not {'record_experiences','converge_vrs','commit_judgment'} & {t.name for t in tools}
            status=await client.call_tool('memory_status',{})
            assert not status.is_error and not status.structured_content['native_engine_bundled']
            p=await client.call_tool('memory_context',dict(request_id='test',query='synthetic',
                expected_pair_snapshot_id=status.structured_content['pair_snapshot_id'],page_size=1))
            assert not p.is_error,p
            data=p.structured_content
            assert data['memories'][0]['replay']['data']==peer.replay[0]
            rejected=await client.call_tool('memory_release',dict(request_id='test',view_id='wrong'))
            assert rejected.is_error
        assert not peer.sessions  # EOF owns transient cleanup; original source remains.
        assert len(peer.replay)==6


def test_partial_data_keeps_original_path_and_fits_mcp_envelope(tmp_path):
    path=tmp_path/'native.sock'
    with native_peer(path) as peer:
        original='복잡한 조건😺'*5000
        peer.replay[0]['steps'][0]['observation']['text']=original
        server=MemoryMCPServer(ResidentClient(path));server.initialized=True
        reply=server.dispatch(dict(jsonrpc='2.0',id=1,method='tools/call',params=dict(name='memory_context',
            arguments=dict(request_id='test',query='synthetic',expected_pair_snapshot_id='a'*64))))
        assert not reply['result']['isError'] and len(encode(reply))<MAX_BYTES
        packet=reply['result']['structuredContent'];row=packet['memories'][0]
        assert not row['replay']['complete'] and row['replay']['deferred']
        h={k:packet[k] for k in ('request_id','view_id')}
        page=server.call_tool('memory_read_path',dict(h,
            path=['receipt','activation','replay','episodes',0,'steps',0,'observation','text']))
        assert page['content']==original[:128] and page['next_offset']==128
        assert server.close()==[]


def test_conflict_on_other_page_not_hidden(tmp_path):
    path=tmp_path/'native.sock'
    with native_peer(path) as peer:
        control=peer.root['receipt']['activation']['re_evidence']
        control.update(unresolved_conflict=True,conflicting_propositions=['synthetic-proposition'])
        peer.verdicts[0].update(verdict='conflict',contradiction_refs=['fixture:0','fixture:5'])
        server=MemoryMCPServer(ResidentClient(path));p=start(server,page_size=1)
        assert p['main_controls']['unresolved_conflict']['data'] is True
        assert p['memories'][0]['re_evidence']['data']['contradiction_refs']==['fixture:0','fixture:5']
        assert p['next_index']==1 and not p['grants_authority']
        assert server.close()==[]


def test_resume_and_stale_generation_refusal(tmp_path):
    path=tmp_path/'native.sock'
    with native_peer(path) as peer:
        first=MemoryMCPServer(ResidentClient(path));p=start(first,wait_turns=0)
        assert p['status']=='pending'
        h={k:p[k] for k in ('request_id','view_id')}
        second=MemoryMCPServer(ResidentClient(path))
        resumed=second.call_tool('memory_resume',dict(h,expected_pair_snapshot_id='a'*64))
        assert resumed['recall_resubmitted'] is False
        assert second.call_tool('memory_context',h)['status']=='memory_context_ready'
        peer.snapshot='b'*64
        with pytest.raises(InterfaceError):second.call_tool('memory_context',h)
        assert second.close()==[] and not peer.sessions


@pytest.mark.parametrize('name',['cognitive_dialogue_result','cognitive_dialogue_advance','assimilate_sealed_wave','cue'])
def test_no_hidden_model_or_write_rpc_even_from_python(name,tmp_path):
    with pytest.raises(InterfaceError,match='resident_operation_not_exported'):
        ResidentClient(tmp_path/'nonexistent.sock').request(name)


@pytest.mark.parametrize('args',[{'page_size':9},{'wait_turns':9},{'page_size':True},{'query':'x'},
                                 {'view_id':'v','query':'x'},{'start_index':-1}])
def test_invalid_tool_inputs_before_network(args):
    class NoCalls:
        def request(self,*a,**k):raise AssertionError('no IPC')
    with pytest.raises(InterfaceError):
        MemoryMCPServer(NoCalls()).call_tool('memory_context',dict(request_id='r',**args))


@pytest.mark.parametrize('raw',[b'{"a":1,"a":2}',b'{"a":NaN}',b'[]',b'\xff'])
def test_invalid_json(raw):
    with pytest.raises(InterfaceError):decode(raw)


def test_initialize_reports_version_and_usage_instructions():
    server=MemoryMCPServer(None)
    reply=server.dispatch(dict(jsonrpc='2.0',id=1,method='initialize',params=dict(
        protocolVersion='2025-06-18',capabilities={},clientInfo={'name':'test','version':'1'})))
    assert reply['result']['serverInfo']['version']=='2.0.0'
    assert 'memory_context' in reply['result']['instructions']


def test_native_rejects_write_mode_cli(tmp_path):
    result=subprocess.run([sys.executable,'-m','swegca_vrs_mcp.server','--native-socket',str(tmp_path/'x'),
        '--enable-writes'],capture_output=True,text=True,timeout=10)
    assert result.returncode==2 and not result.stdout and 'no write/trust tools' in result.stderr
