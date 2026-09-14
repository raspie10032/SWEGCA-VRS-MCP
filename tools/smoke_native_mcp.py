"""Installed-package native bridge check. Operator supplies the running backend.

Outputs timing/coverage only, never full source records. Not a Claude or growth test.
"""
import argparse
import asyncio
import json
from pathlib import Path
from time import perf_counter_ns
from uuid import uuid4

from mcp import Client, StdioServerParameters


async def run(options):
    began=perf_counter_ns()
    result={}
    parameters=StdioServerParameters(command=options.server,args=['--socket',options.socket,'--timeout','45'])
    async with Client(parameters,mode=options.mode,read_timeout_seconds=120) as client:
        names=[t.name for t in (await client.list_tools()).tools]
        assert len(names)==8 and 'memory_context' in names
        status=await client.call_tool('memory_status',{})
        assert not status.is_error
        pair=status.structured_content['pair_snapshot_id']
        started=perf_counter_ns()
        packet=await client.call_tool('memory_context',dict(request_id='native-smoke-'+uuid4().hex,
            query=options.query,expected_pair_snapshot_id=pair))
        assert not packet.is_error, 'native context call failed'
        data=packet.structured_content
        handle={k:data[k] for k in ('request_id','view_id')}
        try:
            turns=0
            while data['status']=='pending' and turns<16:
                packet=await client.call_tool('memory_context',handle)
                assert not packet.is_error
                data=packet.structured_content;turns+=1
            assert data['status']=='memory_context_ready', 'native activation still pending'
            assert data['pair_snapshot_id']==pair and data['grants_authority'] is False
            assert data['internal_llm_calls']==data['final_utterance_calls']==0
            if options.require_record:
                assert data['memories'] and any(r['replay']['complete'] for r in data['memories'])
            result=dict(status='PASS',tools=names,pair_snapshot_id=pair,
                candidate_count=data['candidate_count'],returned_records=len(data['memories']),
                complete_records=sum(r['complete'] for r in data['memories']),
                next_index=data['next_index'],coverage=data['coverage'],
                context_roundtrip_ns=perf_counter_ns()-started,server_timings_ns=data['timings_ns'],
                internal_llm_calls=0,final_utterance_calls=0,growth_claimed=False)
        finally:
            released=await client.call_tool('memory_release',handle)
            assert not released.is_error and released.structured_content['status']=='released'
        after=await client.call_tool('memory_status',{})
        assert not after.is_error and after.structured_content['pair_snapshot_id']==pair
    result['total_ns']=perf_counter_ns()-began
    if options.output:
        Path(options.output).write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')
    print(json.dumps(result,indent=2))


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--server',required=True)
    parser.add_argument('--socket',required=True)
    parser.add_argument('--query',required=True)
    parser.add_argument('--mode',choices=['auto','legacy'],default='auto')
    parser.add_argument('--require-record',action='store_true')
    parser.add_argument('--output')
    asyncio.run(run(parser.parse_args()))
