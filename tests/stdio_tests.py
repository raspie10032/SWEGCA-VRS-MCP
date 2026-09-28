#!/usr/bin/env python3
"""Real subprocess transport/lifecycle checks; no client app or service is changed."""
import json, os, pathlib, select, shutil, socket, subprocess, sys, tempfile, time
exe=pathlib.Path(sys.argv[1]).resolve()
checks=0
mode_prefix="limited-" if "--limited" in sys.argv[2:] else ""

def check(value):
    global checks
    checks+=1
    assert value

def identity(n):
    return bytes([n]+[0]*31).hex()

class Client:
    def __init__(self,mode,root,config,query_socket=None):
        env=os.environ.copy()
        env.pop('SWEGCA_QUERY_SOCKET',None)
        if query_socket:env['SWEGCA_QUERY_SOCKET']=str(query_socket)
        self.p=subprocess.Popen([str(exe),mode_prefix+mode,str(root),str(config)],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,env=env)
        self.serial=0
    def raw(self,data):
        self.p.stdin.write(data);self.p.stdin.flush()
        check(bool(select.select([self.p.stdout],[],[],10)[0]))
        line=self.p.stdout.readline()
        check(bool(line))
        return json.loads(line)
    def call(self,method,params=None):
        self.serial+=1
        reply=self.raw(json.dumps({'jsonrpc':'2.0','id':self.serial,'method':method,'params':params or {}},ensure_ascii=True).encode()+b'\n')
        check(reply['id']==self.serial)
        result=reply.get('result')
        if method=='tools/call' and isinstance(result,dict) and 'structuredContent' in result:
            check(json.loads(result['content'][0]['text'])==result['structuredContent'])
        return reply
    def notice(self,method,params=None):
        self.p.stdin.write(json.dumps({'jsonrpc':'2.0','method':method,'params':params or {}}).encode()+b'\n');self.p.stdin.flush()
    def initialize(self):
        check('error' in self.call('tools/list'))
        initialized=self.call('initialize',{'protocolVersion':'2025-06-18','capabilities':{},'clientInfo':{'name':'test','version':'1'}})['result']
        check(initialized['protocolVersion']=='2025-06-18')
        check(initialized['capabilities']['experimental']['swegcaHostInput']['version']=='15')
        self.notice('notifications/initialized')
        tools=self.call('tools/list')['result']['tools']
        check([t['name'] for t in tools]==['vrs_replay','vrs_re_evidence'])
        check('error' in self.call('initialize',{'protocolVersion':'2025-06-18','capabilities':{},'clientInfo':{}}))
    def close(self):
        self.p.stdin.close();check(self.p.wait(timeout=10)==0)
        check(self.p.stdout.read()==b'');check(self.p.stderr.read()==b'')

with tempfile.TemporaryDirectory(prefix='swegca-stdio-') as directory:
    root=pathlib.Path(directory)
    config={
        'cpuAffinity':'6 7','ioBytesPerSecond':'625000000','storageBytes':'500000000000','mergeWorkers':'2','memoryBytes':str(64<<20),'frameBytes':'4096','mainIdentity':identity(99),'initialStrength':1.0,
        'sessionBlockBytes':'65536','mainBlockBytes':'4096','readLimit':'16384',
        'policy':{'chance_rate':0.2,'accept_margin':0.25,'confidence_level':0.9,'prior_alpha':1.0,'prior_beta':1.0,
            'regime_change_threshold':0.3,'minimum_effective_samples_per_axis':'4','minimum_source_diversity':'2',
            'minimum_axis_source_diversity':'1','minimum_context_diversity':'4','recent_window':'6','minimum_recent_samples':'4','axis_count':'1'}}
    path=root/'config.json';path.write_text(json.dumps(config))
    c=Client('create',root,path);c.initialize()
    check(c.call('unknown')['error']['code']==-32601)
    check(c.call('tools/call',{'name':'missing','arguments':{}})['error']['code']==-32602)
    malformed=[b'{',b'[]',b'{"jsonrpc":"2.0","jsonrpc":"2.0","id":1,"method":"ping"}',b'{"jsonrpc":"2.0","id":"\\ud800","method":"ping"}',b'{"jsonrpc":"2.0","id":"\xc0\x80","method":"ping"}',b'{"jsonrpc":"2.0","id":01,"method":"ping"}',b'['*66+b'0'+b']'*66,b' '*4097]
    for value in malformed:check('error' in c.raw(value+b'\n'))
    # Notifications must never trigger host mutations or acquire a session.
    c.notice('swegca/start',{'identity':identity(1),'name':'not-started'})
    check('error' in c.call('swegca/end'))
    check(c.call('swegca/start',{'identity':identity(1),'name':'one'})['result']=={})
    text='사용자\n입력\x00🙂'
    def event(name,source='user',**extra):
        fields={'sequence':'0','observedAt':'0','seed':'7','step':'0','session':name,'source':source,'media':'text/plain','content':text};fields.update(extra);return fields
    first=c.call('swegca/receive',event('one'))['result'];check(first['candidates']==[])
    check(c.call('tools/call',{'name':'vrs_replay','arguments':{'receipt':first['receipt']}})['result']['isError'])
    second=c.call('swegca/receive',event('one','assistant',sequence='1'))['result'];check(len(second['candidates'])==1)
    context_packet=c.call('tools/call',{'name':'vrs_replay','arguments':{'receipt':second['receipt']}})['result']['structuredContent']
    check(context_packet['original']==first['original'] and context_packet['source']=='user')
    check(context_packet['session']=='one' and context_packet['observedAt']=='0')
    check(context_packet['grantsAuthority'] is False)
    check(context_packet['assessment']['agreement']==1 and context_packet['assessment']['status']==0)
    check(context_packet['assessment']['inputOriginal']==second['original'])
    check(context_packet['assessment']['currentOriginalCount']=='1')
    check(context_packet['assessment']['reEvidencePerformed'] is False)
    check(bytes.fromhex(context_packet['contentHex'])==text.encode())
    check(c.call('tools/call',{'name':'vrs_replay','arguments':{'receipt':second['receipt'],
        'inputOriginal':second['original']}})['result']['structuredContent']==context_packet)
    check(c.call('tools/call',{'name':'vrs_replay','arguments':{'receipt':second['receipt'],
        'inputOriginal':first['original']}})['result']['isError'])
    check(c.call('tools/call',{'name':'vrs_replay','arguments':{'receipt':second['receipt']}})['result']['structuredContent']==context_packet)

    check(c.call('tools/call',{'name':'vrs_replay','arguments':{'receipt':first['receipt'],'candidate':'0'}})['result']['isError'])
    replay=c.call('tools/call',{'name':'vrs_replay','arguments':{'receipt':second['receipt'],'candidate':'0'}})['result']
    check(bytes.fromhex(replay['structuredContent']['contentHex'])==text.encode())
    check(replay['structuredContent']['original']==first['original'])
    automatic=c.call('tools/call',{'name':'vrs_replay','arguments':{'receipt':second['receipt']}})['result']
    check(automatic['structuredContent']==context_packet)
    compared=c.call('tools/call',{'name':'vrs_re_evidence','arguments':{'receipt':second['receipt'],'seed':'7','step':'0'}})['result']['structuredContent']
    check(compared['status']==0)
    check(compared['reEvidencePerformed'] is False)
    # Partial Replay retains only the requested raw bytes and cannot reuse an
    # earlier full Replay receipt for Re-evidence, even on the same candidate.
    def partial(receipt, offset, count):
        return c.call('tools/call',{'name':'vrs_replay','arguments':{
            'receipt':receipt,'candidate':'0','offset':str(offset),'count':str(count)}})['result']
    part=partial(second['receipt'],2,5)['structuredContent']
    check(part['partial'] is True and part['offset']=='2' and part['totalBytes']==str(len(text.encode())))
    check(bytes.fromhex(part['contentHex'])==text.encode()[2:7] and part['original']==first['original'])
    check(c.call('tools/call',{'name':'vrs_re_evidence','arguments':{'receipt':second['receipt'],'seed':'7','step':'0'}})['result']['isError'])
    restored=c.call('tools/call',{'name':'vrs_replay','arguments':{'receipt':second['receipt']}})['result']['structuredContent']
    check(restored==context_packet)
    check(partial(second['receipt'],len(text.encode()),0)['structuredContent']['contentHex']=='')
    check(partial(second['receipt'],len(text.encode()),1)['isError'])
    for field in ('offset','count'):
        check(c.call('tools/call',{'name':'vrs_replay','arguments':{
            'receipt':second['receipt'],'candidate':'0',field:'0'}})['result']['isError'])
    check(partial(first['receipt'],0,1)['isError'])
    check('structuredContent' in c.call('tools/call',{'name':'vrs_replay','arguments':{
        'receipt':second['receipt'],'candidate':'0'}})['result'])
    check('structuredContent' in c.call('tools/call',{'name':'vrs_re_evidence','arguments':{
        'receipt':second['receipt'],'seed':'7','step':'0'}})['result'])
    check(c.call('swegca/work',{'seed':'7','step':'0'})['result']['merged']=='0')
    # UTF-8 JSON string and escape forms must preserve identical request IDs.
    for rid in ['한글🙂','\x00\n"\\',123456789012345678901234567890,-12]:
        for ascii_only in (True,False):
            reply=c.raw(json.dumps({'jsonrpc':'2.0','method':'ping','id':rid},ensure_ascii=ascii_only).encode()+b'\n');check(reply['id']==rid)
    c.close() # EOF must not end the live session.
    c=Client('open',root,path);c.initialize()
    check(c.call('swegca/work',{'seed':'7','step':'0'})['result']['merged']=='0')
    check(c.call('swegca/resume',{'identity':identity(1)})['result']=={})
    third=c.call('swegca/receive',event('one',sequence='2'))['result'];check(len(third['candidates'])==2)
    check(c.call('swegca/end')['result']=={});c.close()
    c=Client('open',root,path);c.initialize()
    check(c.call('swegca/work',{'seed':'7','step':'0'})['result']['merged']=='1')
    check(c.call('swegca/start',{'identity':identity(2),'name':'two'})['result']=={})
    fourth=c.call('swegca/receive',event('two'))['result'];check(not fourth['temporary'] and len(fourth['candidates'])==3)
    replay=c.call('tools/call',{'name':'vrs_replay','arguments':{'receipt':fourth['receipt'],'candidate':'0'}})['result']['structuredContent']
    check(replay['original']==first['original'])
    check(partial(fourth['receipt'],1,4)['structuredContent']['contentHex']==text.encode()[1:5].hex())
    payload=event('two','tool',sequence='1',media='application/octet-stream');del payload['content'];payload['contentHex']='00ff80fe0a'
    c.call('swegca/receive',payload)
    payload['sequence']='2';binary=c.call('swegca/receive',payload)['result']
    replay=c.call('tools/call',{'name':'vrs_replay','arguments':{'receipt':binary['receipt'],'candidate':'0'}})['result']['structuredContent']
    check(replay['contentHex']=='00ff80fe0a')
    check(c.call('swegca/end')['result']=={});c.close()
    retained_root=root/'retained-assessment';retained_root.mkdir()
    c=Client('create',retained_root,path);c.initialize()
    check(c.call('swegca/start',{'identity':identity(30),'name':'retained-assessment'})['result']=={})
    retained_first=c.call('swegca/receive',event('retained-assessment'))['result']
    retained_input=c.call('swegca/receive',event('retained-assessment',sequence='1'))['result']
    def retained_packet():
        return c.call('tools/call',{'name':'vrs_replay','arguments':{'receipt':retained_input['receipt']}})['result']['structuredContent']
    prior_packet=retained_packet();check(prior_packet['assessment']['currentOriginalCount']=='1')
    check('result' in c.call('swegca/retain',event('retained-assessment',sequence='2',step='1')))
    updated_packet=retained_packet()
    check(updated_packet['original']==retained_first['original'])
    check(updated_packet['assessment']['currentOriginalCount']=='2' and updated_packet['assessment']['step']=='1')
    check(updated_packet['assessment']['agreement']==1 and updated_packet['assessment']['status']==0)
    check(not updated_packet['assessment']['reEvidencePerformed'] and not updated_packet['grantsAuthority'])
    check(partial(retained_input['receipt'],0,1)['structuredContent']['partial'])
    check('result' in c.call('swegca/retain',event('retained-assessment',sequence='3',step='2',content='unrelated content')))
    check(retained_packet()==updated_packet)
    check('error' in c.call('swegca/retain',event('retained-assessment',source='',sequence='4')))
    check(c.call('tools/call',{'name':'vrs_replay','arguments':{'receipt':retained_input['receipt']}})['result']['isError'])
    c.close()
    # Recorded observations go through the same server/runtime/core route.
    # Evidence travels in the receive call, before a separate observe or merge.
    live_root=root/'live-input-evidence';live_root.mkdir()
    live=Client('create',live_root,path);live.initialize()
    live.call('swegca/start',{'identity':identity(19),'name':'live'})
    for outcome,expected in [('support',1),('refute',2),('insufficient',0)]:
        for n in range(12):
            obs={'source':identity(100+n),'context':identity(150+n),'producer':identity(200+n),
                 'expiresAt':'0','hasExpiry':False,'confidence':1.0,'axis':'0','outcome':outcome}
            result=live.call('swegca/receive',event('live','sensor',sequence=str(n),
                content='predicate '+outcome,observation=obs))['result']
            if n or outcome=='support': check(len(result['candidates'])==n)
        check(result['refinement']['status']==expected)
    bad=dict(obs,verdict='accept')
    check('error' in live.call('swegca/receive',event('live',observation=bad)))
    bad=dict(obs,hypothesis=identity(1))
    check('error' in live.call('swegca/receive',event('live',observation=bad)))
    live.close()

    observed_root=root/'observed';observed_root.mkdir()
    c=Client('create',observed_root,path);c.initialize()
    check(c.call('swegca/start',{'identity':identity(3),'name':'observed'})['result']=={})
    check(c.call('swegca/define',{'identity':identity(20)})['result']=={})
    check(c.call('swegca/define',{'identity':identity(21)})['result']=={})
    def observation(n,connection,outcome):
        p=event('observed','experiment',sequence=str(n))
        p['observation']={'hypothesis':identity(connection),'source':identity(100+n),
            'context':identity(150+n),'producer':identity(200+n),'expiresAt':'0',
            'hasExpiry':False,'confidence':1.0,'axis':'0','outcome':outcome}
        return p
    last_strength=1.0;support_original=None;states=set()
    for n in range(8):
        result=c.call('swegca/observe',observation(n,20,'support'))['result']
        if n==0:support_original=result['original']
        result=result['refinement'];status=result['status'];states.add(status)
        expected=last_strength*1.01 if status==1 else last_strength*0.995 if status==2 else last_strength
        check(result['strength']==expected);last_strength=result['strength']
    check(last_strength>1.0 and 0 in states and 1 in states)
    invalid=observation(8,20,'approve')
    check('error' in c.call('swegca/observe',invalid))
    recalled=c.call('swegca/receive',event('observed',sequence='8'))['result']
    check(len(recalled['candidates'])==8)
    receipt=recalled['receipt']
    automatic_before=c.call('tools/call',{'name':'vrs_replay','arguments':{'receipt':receipt}})['result']['structuredContent']
    check(automatic_before['assessment']['agreement']==1)
    check('error' in c.call('swegca/observe',invalid))
    check(c.call('tools/call',{'name':'vrs_replay','arguments':{'receipt':receipt}})['result']['structuredContent']==automatic_before)
    current=[]
    check(partial(receipt,0,1)['structuredContent']['partial'])
    for n in range(8,16):
        incoming=observation(n,20,'refute')
        if n==15:incoming['step']='1'
        result=c.call('swegca/observe',incoming)['result'];current.append(result['original'])
    automatic_after=c.call('tools/call',{'name':'vrs_replay','arguments':{'receipt':receipt}})['result']['structuredContent']
    check(automatic_after['original']==automatic_before['original'])
    check(automatic_after['assessment']['agreement']==3 and automatic_after['assessment']['reEvidencePerformed'])
    check(automatic_after['assessment']['currentOriginalCount']=='8' and automatic_after['assessment']['step']=='1')
    check(partial(receipt,0,1)['structuredContent']['partial'])
    check(c.call('tools/call',{'name':'vrs_replay','arguments':{'receipt':receipt}})['result']['structuredContent']==automatic_after)
    refreshed=c.call('tools/call',{'name':'vrs_re_evidence','arguments':{'receipt':receipt,'seed':'19','step':'2'}})['result']['structuredContent']
    exported=c.call('tools/call',{'name':'vrs_replay','arguments':{'receipt':receipt}})['result']['structuredContent']
    check(exported['original']==automatic_after['original'] and exported['assessment']['step']=='2')
    for field in ('agreement','status','reEvidencePerformed','currentHead','rememberedHead'):
        check(exported['assessment'][field]==refreshed[field])
    check(exported['assessment']['currentOriginalCount']==str(len(refreshed['currentOriginals'])))
    bad_refresh=c.call('tools/call',{'name':'vrs_re_evidence','arguments':{'receipt':receipt,'seed':'bad','step':'3'}})
    check(bad_refresh['result']['isError'])
    check(c.call('tools/call',{'name':'vrs_replay','arguments':{'receipt':receipt}})['result']['structuredContent']==exported)
    check(c.call('tools/call',{'name':'vrs_replay','arguments':{'receipt':receipt,'candidate':'0'}})['result']['structuredContent']['original']==support_original)
    checked=c.call('tools/call',{'name':'vrs_re_evidence','arguments':{'receipt':receipt,'seed':'7','step':'0'}})['result']['structuredContent']
    check(checked['status']==2 and checked['agreement']==3)
    check(checked['currentOriginals']==current and checked['replayedOriginal']==support_original)
    check(checked['rememberedHead']!=checked['currentHead'])
    last_strength=1.0
    for n in range(16,24):
        result=c.call('swegca/observe',observation(n,21,'refute'))['result']['refinement']
        expected=last_strength*1.01 if result['status']==1 else last_strength*0.995 if result['status']==2 else last_strength
        check(result['strength']==expected);last_strength=result['strength'];states.add(result['status'])
    check(last_strength<1.0 and states=={0,1,2})
    check(c.call('swegca/end')['result']=={})
    check(c.call('swegca/work',{'seed':'7','step':'0'})['result']['merged']=='1')
    c.close()
    # Bounded pages preserve all pinned addresses, absolute indexes and order.
    paged_root=root/'paged';paged_root.mkdir()
    c=Client('create',paged_root,path);c.initialize()
    check(c.call('swegca/start',{'identity':identity(4),'name':'paged'})['result']=={})
    originals=[]
    for n in range(70):
        result=c.call('swegca/receive',event('paged',sequence=str(n)))['result']
        check(result['candidateCount']==str(n))
        check(len(result['candidates'])==min(n,64))
        check(result['nextOffset']==('64' if n>64 else None))
        originals.append(result['original'])
    receipt=result['receipt']
    check([entry['original'] for entry in result['candidates']]==originals[:64])
    # Read pagination must neither record another event nor expire Replay.
    gathered=[];offset='0'
    while offset is not None:
        page=c.call('swegca/candidates',{'receipt':receipt,'offset':offset,'candidateLimit':'7'})['result']
        check(page['receipt']==receipt and page['candidateCount']=='69')
        gathered.extend(page['candidates']);offset=page['nextOffset']
    check([entry['index'] for entry in gathered]==[str(n) for n in range(69)])
    check([entry['original'] for entry in gathered]==originals[:69])
    tail=c.call('swegca/candidates',{'receipt':receipt,'offset':'69'})['result']
    check(tail['candidates']==[] and tail['nextOffset'] is None)
    for params in ({'offset':'70'},{'offset':'18446744073709551615'},
                   {'candidateLimit':'0'},{'candidateLimit':'257'},{'candidateLimit':1}):
        request={'receipt':receipt,'offset':'0'};request.update(params)
        check('error' in c.call('swegca/candidates',request))
    # Invalid receive limits are rejected before clearing the old receipt or writing.
    check('error' in c.call('swegca/receive',event('paged',sequence='70',candidateLimit='0')))
    replay=c.call('tools/call',{'name':'vrs_replay','arguments':{'receipt':receipt,'candidate':'68'}})['result']['structuredContent']
    check(replay['original']==originals[68] and bytes.fromhex(replay['contentHex'])==text.encode())
    result=c.call('swegca/receive',event('paged',sequence='70',candidateLimit='1'))['result']
    check(result['candidateCount']=='70' and len(result['candidates'])==1 and result['nextOffset']=='1')
    check('error' in c.call('swegca/candidates',{'receipt':receipt,'offset':'0'}))
    receipt=result['receipt']
    check(c.call('swegca/end')['result']=={})
    check('error' in c.call('swegca/candidates',{'receipt':receipt,'offset':'0'}))
    check(c.call('swegca/work',{'seed':'7','step':'0'})['result']['merged']=='1')
    check(c.call('swegca/start',{'identity':identity(5),'name':'paged-main'})['result']=={})
    main_page=c.call('swegca/receive',event('paged-main',candidateLimit='1'))['result']
    check(not main_page['temporary'] and main_page['candidateCount']=='71')
    tail=c.call('swegca/candidates',{'receipt':main_page['receipt'],'offset':'64','candidateLimit':'256'})['result']
    check(tail['nextOffset'] is None and len(tail['candidates'])==7)
    check([entry['original'] for entry in tail['candidates'][:6]]==originals[64:70])
    check(c.call('swegca/end')['result']=={});c.close()
    # Session event retention preserves the current input/Replay, goes through
    # refinement, and survives EOF recovery and explicit-end Main merging.
    events_root=root/'events';events_root.mkdir()
    c=Client('create',events_root,path);c.initialize()
    check('error' in c.call('swegca/retain',event('events')))
    check(c.call('swegca/start',{'identity':identity(6),'name':'events'})['result']=={})
    first=c.call('swegca/receive',event('events'))['result']
    recalled=c.call('swegca/receive',event('events',sequence='1'))['result']
    receipt=recalled['receipt']
    replay=c.call('tools/call',{'name':'vrs_replay','arguments':{'receipt':receipt,'candidate':'0'}})['result']['structuredContent']
    check(replay['original']==first['original'])
    retained=[]
    for n,source in enumerate(('assistant','tool/result','reasoning','compaction'),2):
        result=c.call('swegca/retain',event('events',source,sequence=str(n)))['result']
        check(set(result)=={'original','refinement'})
        check(result['refinement']['status']==0 and result['refinement']['strength']==1.0)
        check(result['refinement']['revision']==str(2*(n+1)))
        retained.append(result['original'])
    # No new receipt, candidate expansion or selected Replay replacement.
    page=c.call('swegca/candidates',{'receipt':receipt,'offset':'0'})['result']
    check(page['candidateCount']=='1' and page['candidates'][0]['original']==first['original'])
    checked=c.call('tools/call',{'name':'vrs_re_evidence','arguments':{'receipt':receipt,'seed':'7','step':'0'}})['result']['structuredContent']
    check(checked['status']==0 and checked['replayedOriginal']==first['original'])
    check(checked['currentOriginals']==[recalled['original']]+retained)
    # Malformed and notification events must not produce a stored original.
    invalid=event('events',sequence='6',contentHex='00')
    check('error' in c.call('swegca/retain',invalid))
    invalid=event('events',sequence='6');del invalid['content'];invalid['contentHex']='0g'
    check('error' in c.call('swegca/retain',invalid))
    c.notice('swegca/retain',event('events',sequence='6'))
    checked=c.call('tools/call',{'name':'vrs_re_evidence','arguments':{'receipt':receipt,'seed':'7','step':'0'}})['result']['structuredContent']
    check(checked['currentOriginals']==[recalled['original']]+retained)
    binary_event=event('events','tool/binary',sequence='6',media='application/octet-stream')
    del binary_event['content'];binary_event['contentHex']='00ff80fe0a'
    binary_saved=c.call('swegca/retain',binary_event)['result']
    check(binary_saved['refinement']['status']==0)
    # Different content is preserved independently, not assigned an invented
    # semantic relationship with the currently replayed experience.
    checked=c.call('tools/call',{'name':'vrs_re_evidence','arguments':{'receipt':receipt,'seed':'7','step':'0'}})['result']['structuredContent']
    check(checked['currentOriginals']==[recalled['original']]+retained)
    check(c.call('swegca/work',{'seed':'7','step':'0'})['result']['merged']=='0')
    c.close()
    c=Client('open',events_root,path);c.initialize()
    check(c.call('swegca/work',{'seed':'7','step':'0'})['result']['merged']=='0')
    check(c.call('swegca/resume',{'identity':identity(6)})['result']=={})
    result=c.call('swegca/receive',event('events',sequence='7'))['result']
    check(result['temporary'] and result['candidateCount']=='6')
    check([entry['original'] for entry in result['candidates']]==[first['original'],recalled['original']]+retained)
    for index,source in enumerate(('assistant','tool/result','reasoning','compaction'),2):
        replay=c.call('tools/call',{'name':'vrs_replay','arguments':{'receipt':result['receipt'],'candidate':str(index)}})['result']['structuredContent']
        check(replay['source']==source and replay['contentHex']==text.encode().hex())
    binary_event['sequence']='8'
    result=c.call('swegca/receive',binary_event)['result']
    check(result['candidateCount']=='1' and result['candidates'][0]['original']==binary_saved['original'])
    replay=c.call('tools/call',{'name':'vrs_replay','arguments':{'receipt':result['receipt'],'candidate':'0'}})['result']['structuredContent']
    check(replay['source']=='tool/binary' and replay['contentHex']=='00ff80fe0a')
    check(c.call('swegca/end')['result']=={})
    check('error' in c.call('swegca/retain',event('events',sequence='9')))
    check(c.call('swegca/work',{'seed':'7','step':'0'})['result']['merged']=='1')
    check(c.call('swegca/start',{'identity':identity(7),'name':'events-main'})['result']=={})
    result=c.call('swegca/receive',event('events-main'))['result']
    check(not result['temporary'] and result['candidateCount']=='7')
    check([entry['original'] for entry in result['candidates'][2:6]]==retained)
    replay=c.call('tools/call',{'name':'vrs_replay','arguments':{'receipt':result['receipt'],'candidate':'5'}})['result']['structuredContent']
    check(replay['source']=='compaction' and replay['contentHex']==text.encode().hex())
    check(c.call('swegca/end')['result']=={});c.close()
    # Native async work remains host-owned; input can run before publication.
    async_root=root/'async';async_root.mkdir()
    c=Client('create',async_root,path);c.initialize()
    check(c.call('swegca/work/start',{'seed':'7','step':'0'})['result']=={'started':False})
    check(c.call('swegca/start',{'identity':identity(8),'name':'async'})['result']=={})
    original=c.call('swegca/receive',event('async'))['result']['original']
    check(c.call('swegca/work/start',{'seed':'7','step':'0'})['result']=={'started':False})
    check(c.call('swegca/end')['result']=={})
    check(c.call('swegca/work/start',{'seed':'7','step':'0'})['result']=={'started':True})
    check('error' in c.call('swegca/work',{'seed':'7','step':'0'}))
    check('error' in c.call('swegca/work/start',{'seed':'7','step':'0'}))
    check(c.call('swegca/start',{'identity':identity(9),'name':'async-active'})['result']=={})
    live=c.call('swegca/receive',event('async-active',content='new active input'))['result']
    # Preparation never publishes itself, even if it has already finished.
    check(live['candidateCount']=='0')
    check(c.call('swegca/attach',{'identity':identity(19),'name':'async-later-end'})['result']=={})
    check(c.call('swegca/select',{'identity':identity(19)})['result']=={})
    later=c.call('swegca/receive',event('async-later-end',content='later ended content'))['result']
    check(c.call('swegca/select',{'identity':identity(9)})['result']=={})
    check(c.call('swegca/end',{'identity':identity(19)})['result']=={})
    check(c.call('swegca/candidates',{'receipt':live['receipt'],'offset':'0'})['result']['candidateCount']=='0')
    deadline=time.monotonic()+10
    while True:
        state=c.call('swegca/work/poll')['result']
        if not state['running']:
            check(state['merged']=='2');break
        check(state['merged'] is None and time.monotonic()<deadline)
    recalled=c.call('swegca/receive',event('async-active',sequence='1'))['result']
    check(not recalled['temporary'] and recalled['candidateCount']=='1')
    replay=c.call('tools/call',{'name':'vrs_replay','arguments':{'receipt':recalled['receipt'],'candidate':'0'}})['result']['structuredContent']
    check(replay['original']==original and replay['contentHex']==text.encode().hex())
    check(c.call('swegca/work/poll')['result']=={'running':False,'merged':'0'})
    check(c.call('swegca/attach',{'identity':identity(18),'name':'async-main-reader'})['result']=={})
    check(c.call('swegca/select',{'identity':identity(18)})['result']=={})
    from_later=c.call('swegca/receive',event('async-main-reader',content='later ended content'))['result']
    check(not from_later['temporary'] and from_later['candidateCount']=='1')
    check(from_later['candidates'][0]['original']==later['original'])
    check(c.call('swegca/select',{'identity':identity(9)})['result']=={})
    check(c.call('swegca/end')['result']=={});c.close()
    # Configured automatic work progresses with no work/start or work/poll RPC.
    auto_root=root/'automatic-main';auto_root.mkdir()
    auto_config=root/'automatic-main.json'
    auto_config.write_text(json.dumps(dict(config,automaticWork={'seed':'7','step':'0'})))
    def graph_bytes():return sum(p.stat().st_size for p in (auto_root/'graph').glob('m-*.block'))
    def wait_for_automatic_main(before):
        deadline=time.monotonic()+10
        while graph_bytes()<=before:
            check(c.p.poll() is None and time.monotonic()<deadline)
            time.sleep(0.005)
        # Main commit and input share an owner: this reply follows completed commit.
        check(c.call('ping')['result']=={})
    c=Client('create',auto_root,auto_config);c.initialize()
    check(c.call('swegca/start',{'identity':identity(40),'name':'automatic-source'})['result']=={})
    auto_original=c.call('swegca/receive',event('automatic-source'))['result']['original']
    time.sleep(0.03);check(graph_bytes()==0) # idle cannot end or publish a live session
    check(c.call('swegca/end',{'identity':identity(40)})['result']=={})
    wait_for_automatic_main(0)
    check(c.call('swegca/start',{'identity':identity(41),'name':'automatic-reader'})['result']=={})
    auto_read=c.call('swegca/receive',event('automatic-reader'))['result']
    check(not auto_read['temporary'] and auto_read['candidates'][0]['original']==auto_original)
    before_eof=graph_bytes();c.close()
    c=Client('open',auto_root,path);c.initialize()
    check(c.call('swegca/resume',{'identity':identity(41)})['result']=={})
    check(graph_bytes()==before_eof) # EOF left the reader active and unmerged
    check(c.call('swegca/end')['result']=={})
    check(graph_bytes()==before_eof);c.close() # manual profile leaves durable queued work
    c=Client('open',auto_root,auto_config);c.initialize()
    wait_for_automatic_main(before_eof) # startup recovers previously explicit-ended work
    c.close()
    # Automatic commit failure is not mislabeled as a malformed input frame.
    auto_failed=root/'automatic-main-storage-failure';auto_failed.mkdir()
    c=Client('create',auto_failed,path);c.initialize()
    check(c.call('swegca/start',{'identity':identity(42),'name':'automatic-failed'})['result']=={})
    c.call('swegca/receive',event('automatic-failed'))
    check(c.call('swegca/end')['result']=={});c.close()
    inodes={}
    for item in auto_failed.rglob('*'):
        if item.is_file():
            stat=item.stat();inodes[(stat.st_dev,stat.st_ino)]=stat.st_size
    exhausted_config=root/'automatic-exhausted.json'
    exhausted_config.write_text(json.dumps(dict(config,storageBytes=str(sum(inodes.values())),
        automaticWork={'seed':'7','step':'0'})))
    c=Client('open',auto_failed,exhausted_config)
    check('result' in c.call('initialize',{'protocolVersion':'2025-06-18','capabilities':{},'clientInfo':{'name':'fixture'}}))
    c.notice('notifications/initialized')
    check(c.p.wait(timeout=10)==2)
    c.p.stdin.close();check(c.p.stdout.read()==b'');c.p.stdout.close()
    check(b'automatic Main work failed' in c.p.stderr.read());c.p.stderr.close()
    c=Client('open',auto_failed,auto_config);c.initialize()
    deadline=time.monotonic()+10
    while not list((auto_failed/'graph').glob('m-*.block')):
        check(c.p.poll() is None and time.monotonic()<deadline);time.sleep(0.005)
    check(c.call('ping')['result']=={})
    check(c.call('swegca/start',{'identity':identity(43),'name':'automatic-recovered'})['result']=={})
    recovered=c.call('swegca/receive',event('automatic-recovered'))['result']
    check(not recovered['temporary'] and recovered['candidateCount']=='1');c.close()
    # One host/Main, independent live session receipts and explicit ends.
    multi_root=root/'multi';multi_root.mkdir()
    c=Client('create',multi_root,path);c.initialize()
    def select_session(n):
        check(c.call('swegca/select',{'identity':identity(n)})['result']=={})
    def replay_receipt(receipt):
        return c.call('tools/call',{'name':'vrs_replay','arguments':{'receipt':receipt,'candidate':'0'}})['result']
    def recheck(receipt):
        return c.call('tools/call',{'name':'vrs_re_evidence','arguments':{'receipt':receipt,'seed':'7','step':'0'}})['result']
    check(c.call('swegca/start',{'identity':identity(10),'name':'agent-a'})['result']=={})
    a0=c.call('swegca/receive',event('agent-a',content='a'))['result']
    a1=c.call('swegca/receive',event('agent-a',content='a',sequence='1'))['result']
    check(replay_receipt(a1['receipt'])['structuredContent']['original']==a0['original'])
    check(c.call('swegca/attach',{'identity':identity(11),'name':'agent-b'})['result']=={})
    # Attachment and failed attachment/selection preserve A's completed Replay.
    check('error' in c.call('swegca/attach',{'identity':identity(11),'name':'duplicate'}))
    check('error' in c.call('swegca/select',{'identity':identity(12)}))
    check('error' in c.call('swegca/attach/resume',{'identity':identity(12)}))
    check('structuredContent' in recheck(a1['receipt']))
    select_session(11)
    check(replay_receipt(a1['receipt'])['isError'])
    b0=c.call('swegca/receive',event('agent-b',content='b'))['result']
    b1=c.call('swegca/receive',event('agent-b',content='b',sequence='1'))['result']
    check(b1['receipt']!=a1['receipt'] and b1['candidateCount']=='1')
    check(replay_receipt(a1['receipt'])['isError'])
    check(replay_receipt(b1['receipt'])['structuredContent']['original']==b0['original'])
    select_session(10)
    check('structuredContent' in recheck(a1['receipt']))
    check(replay_receipt(b1['receipt'])['isError'])
    check(c.call('swegca/candidates',{'receipt':a1['receipt'],'offset':'0'})['result']['candidates'][0]['original']==a0['original'])
    check(c.call('swegca/work',{'seed':'7','step':'0'})['result']['merged']=='0')
    check(c.call('swegca/end')['result']=={})
    check('error' in c.call('swegca/select',{'identity':identity(10)}))
    check(replay_receipt(a1['receipt'])['isError'])
    check(c.call('swegca/work',{'seed':'7','step':'0'})['result']['merged']=='1')
    select_session(11)
    check('structuredContent' in recheck(b1['receipt']))
    c.close() # Parked/live B survives transport EOF without publication.
    c=Client('open',multi_root,path);c.initialize()
    check(c.call('swegca/attach/resume',{'identity':identity(11)})['result']=={})
    check('error' in c.call('swegca/end')) # attach alone has no selection
    check(c.call('swegca/work',{'seed':'7','step':'0'})['result']['merged']=='0')
    select_session(11)
    check(replay_receipt(b1['receipt'])['isError']) # receipts are process-local
    b2=c.call('swegca/receive',event('agent-b',content='b',sequence='2'))['result']
    check(b2['temporary'] and b2['candidateCount']=='2')
    check(c.call('swegca/end')['result']=={})
    check(c.call('swegca/work',{'seed':'7','step':'0'})['result']['merged']=='1')
    c.close()
    # Explicit end targets never inherit the last-selected agent session.
    targeted_root=root/'targeted-end';targeted_root.mkdir()
    c=Client('create',targeted_root,path);c.initialize()
    check(c.call('swegca/start',{'identity':identity(20),'name':'end-a'})['result']=={})
    ta0=c.call('swegca/receive',event('end-a',content='a'))['result']
    ta1=c.call('swegca/receive',event('end-a',content='a',sequence='1'))['result']
    check(replay_receipt(ta1['receipt'])['structuredContent']['original']==ta0['original'])
    check(c.call('swegca/attach',{'identity':identity(21),'name':'end-b'})['result']=={})
    select_session(21)
    c.call('swegca/receive',event('end-b',content='b'))
    select_session(20)
    for bad in (identity(250),'invalid',None):
        check('error' in c.call('swegca/end',{'identity':bad}))
        check('structuredContent' in recheck(ta1['receipt']))
    check(c.call('swegca/end',{'identity':identity(21)})['result']=={})
    check('structuredContent' in recheck(ta1['receipt']))
    check('error' in c.call('swegca/select',{'identity':identity(21)}))
    check('error' in c.call('swegca/end',{'identity':identity(21)}))
    check('structuredContent' in recheck(ta1['receipt']))
    check(c.call('swegca/work',{'seed':'7','step':'0'})['result']['merged']=='1')
    ta2=c.call('swegca/receive',event('end-a',content='a',sequence='2'))['result']
    check(ta2['temporary'] and ta2['candidateCount']=='2')
    check(c.call('swegca/attach',{'identity':identity(22),'name':'end-unselected'})['result']=={})
    check(c.call('swegca/end',{'identity':identity(20)})['result']=={})
    check('error' in c.call('swegca/end')) # no selected session
    check(c.call('swegca/end',{'identity':identity(22)})['result']=={})
    check(c.call('swegca/work',{'seed':'7','step':'0'})['result']['merged']=='1')
    c.close()
    # Actual producer outcomes bind to sealed natural inputs without guessing
    # a hypothesis digest or turning the prose itself into support/refutation.
    linked_root=root/'input-observations';linked_root.mkdir()
    c=Client('create',linked_root,path);c.initialize()
    check(c.call('swegca/start',{'identity':identity(31),'name':'input-observations'})['result']=={})
    linked=[];outcomes=[];last_strength=1.0
    def bound_observation(target,n,outcome='support',session='input-observations'):
        p=event(session,'test-runner',sequence=str(100+n),content='recorded result '+str(n))
        p['inputOriginal']=target
        p['observation']={'source':identity(100+n),'producer':identity(150+n),
            'expiresAt':'0','hasExpiry':False,'confidence':1.0,'axis':'0','outcome':outcome}
        return p
    for n in range(8):
        received=c.call('swegca/receive',event('input-observations',sequence=str(n)))['result']
        linked.append(received['original'])
        incoming=bound_observation(linked[-1],n)
        observed=c.call('swegca/observe',incoming)['result'];outcomes.append(observed['original'])
        check(int(observed['refinement']['revision'])==int(received['refinement']['revision'])+2)
        last_strength=observed['refinement']['strength']
    check(last_strength>1.0 and observed['refinement']['status']==1)
    # A scope sent through an unsupported envelope must not be silently
    # discarded, promoting its result to the whole input's connection.
    for nested in (False,True):
        bad=bound_observation(linked[0],8)
        (bad['observation'] if nested else bad)['scope']='one part only'
        check('error' in c.call('swegca/observe',bad))
    # Neither a forged address nor a supplied hypothesis/context can redirect it.
    bad=bound_observation(linked[0],8);bad['inputOriginal']={**linked[0],'digest':identity(250)}
    check('error' in c.call('swegca/observe',bad))
    for key in ('hypothesis','context'):
        bad=bound_observation(linked[0],8);bad['observation'][key]=identity(250)
        check('error' in c.call('swegca/observe',bad))
    check(c.call('swegca/attach',{'identity':identity(32),'name':'other-owner'})['result']=={})
    check(c.call('swegca/select',{'identity':identity(32)})['result']=={})
    check('error' in c.call('swegca/observe',bound_observation(linked[0],8,session='other-owner')))
    c.close()
    c=Client('open',linked_root,path);c.initialize()
    check(c.call('swegca/resume',{'identity':identity(31)})['result']=={})
    recovered=c.call('swegca/receive',event('input-observations',sequence='9'))['result']
    check(recovered['candidateCount']=='16')
    check(all(o in [x['original'] for x in recovered['candidates']] for o in outcomes))
    # Same-session native observations have their own source; they cannot forge
    # transport deliveries or consume native sequence numbers across recovery.
    bind={'provider':'codex','instance':'bound-test','session':'bound-native'}
    native_owner=c.call('swegca/agent/attach',bind)['result']['identity']
    check(c.call('swegca/select',{'identity':native_owner})['result']=={})
    native_params={'sequence':'0','observedAt':'0','seed':'7','step':'0',
        'native':json.dumps({'session_id':'bound-native','hook_event_name':'UserPromptSubmit','prompt':text})}
    native_input=c.call('swegca/agent/event',native_params)['result']
    incoming=bound_observation(native_input['original'],0,session='bound-native')
    check('result' in c.call('swegca/observe',incoming))
    for n,outcome in enumerate(('refute','insufficient'),1):
        check('result' in c.call('swegca/observe',bound_observation(native_input['original'],n,outcome,session='bound-native')))
    incoming['source']='codex/hook'
    check('error' in c.call('swegca/observe',incoming))
    c.close()
    c=Client('open',linked_root,path);c.initialize()
    check(c.call('swegca/agent/attach/resume',bind)['result']['nextSequence']=='1')
    check(c.call('swegca/select',{'identity':native_owner})['result']=={})
    native_params['sequence']='1'
    native_result=c.call('swegca/agent/event',native_params)['result']
    check(native_result['candidateCount']=='4')
    check(c.call('swegca/work',{'seed':'7','step':'0'})['result']['merged']=='0')
    c.close()
    # Native envelope preserves all fields while the prompt alone keys Recall.
    native_root=root/'native';native_root.mkdir()
    query_dir=root/'query';query_dir.mkdir(mode=0o700)
    query_socket=query_dir/'owner.sock'
    c=Client('create',native_root,path,query_socket=query_socket);c.initialize()
    binding={'provider':'codex','instance':'desktop-local','session':'native-session'}
    native_id=c.call('swegca/agent/attach',binding)['result']['identity']
    check(c.call('swegca/select',{'identity':native_id})['result']=={})
    def native_event(name, seq, **fields):
        data={'session_id':'native-session','hook_event_name':name,**fields}
        raw=json.dumps(data,ensure_ascii=False,indent=1)
        reply=c.call('swegca/agent/event',{'sequence':str(seq),'observedAt':str(seq),
            'seed':'7','step':str(seq),'native':raw})
        return raw,reply
    def resend_native(raw, seq, observed=None):
        return c.call('swegca/agent/event',{'sequence':str(seq),
            'observedAt':str(seq if observed is None else observed),
            'seed':'123','step':'999','native':raw})
    raw0,n0=native_event('UserPromptSubmit',0,prompt=text,unknown={'binary':'\u0000','turn':'a'})
    n0=n0['result'];check(n0['candidateCount']=='0' and n0['refinement']['status']==0)
    raw1,n1=native_event('UserPromptSubmit',1,prompt=text,unknown={'turn':'b'})
    n1=n1['result'];check(n1['candidateCount']=='1' and n1['candidates'][0]['original']==n0['original'])
    check(n0['memory']=={'completed':True,'original':None})
    check(n1['memory']['completed'] and n1['memory']['original']==n0['original'])
    check(n1['memory']['agreement']==1 and n1['memory']['reEvidencePerformed'] is False)
    native_checkpoint=c.call('swegca/agent/cognition',{'identity':native_id,'sequence':'1'})['result']['record']['recovery']
    check(native_checkpoint['temporary'] and native_checkpoint['keyKind']==1 and not native_checkpoint['seedOnly'])
    check(native_checkpoint['originalIndex']=='0' and native_checkpoint['observationBoundary']=='1')
    check(native_checkpoint['lookupKey']==native_checkpoint['inputCue']==n1['refinement']['connection'])
    check(native_checkpoint['connection']==n0['refinement']['connection'])
    check(native_checkpoint['rememberedHead']==native_checkpoint['observationHead'])
    automatic_played=c.call('tools/call',{'name':'vrs_replay','arguments':{'receipt':n1['receipt']}})['result']['structuredContent']
    check('structuredContent' in recheck(n1['receipt']))
    # The private adapter channel selects the receipt's native owner, without
    # changing the primary stream's active session or exposing host mutations.
    def agent_query(params,method='swegca/agent/replay'):
        with socket.socket(socket.AF_UNIX,socket.SOCK_STREAM) as peer:
            peer.settimeout(5);peer.connect(str(query_socket))
            peer.sendall(json.dumps({'jsonrpc':'2.0','id':71,'method':method,'params':params}).encode()+b'\n')
            data=bytearray()
            while not data.endswith(b'\n'):
                part=peer.recv(4096)
                if not part:break
                data.extend(part)
            return json.loads(data) if data else None
    query_args={'receipt':n1['receipt'],'inputOriginal':n1['original']}
    query_baseline=c.call('tools/call',{'name':'vrs_replay','arguments':query_args})['result']['structuredContent']
    check(query_socket.stat().st_mode&0o777==0o600)
    check(agent_query(query_args)['result']['structuredContent']==query_baseline)
    observer_requests=[{'jsonrpc':'2.0','id':1,'method':'initialize','params':{'protocolVersion':'2025-06-18',
        'capabilities':{},'clientInfo':{'name':'owner-query-test','version':'1'}}},
        {'jsonrpc':'2.0','method':'notifications/initialized'},
        {'jsonrpc':'2.0','id':2,'method':'tools/list'},
        {'jsonrpc':'2.0','id':3,'method':'tools/call','params':{'name':'vrs_replay','arguments':query_args}}]
    observer_env={**os.environ,'SWEGCA_QUERY_SOCKET':str(query_socket)}
    observation_query=subprocess.run([str(exe.parent/'swegca-content-observer'),'16777216','625000000','1048576'],
        input=b''.join(json.dumps(item).encode()+b'\n' for item in observer_requests),
        env=observer_env,capture_output=True,timeout=10,check=True)
    observer_replies=[json.loads(line) for line in observation_query.stdout.splitlines()]
    check(observation_query.stderr==b'')
    check({tool['name'] for tool in observer_replies[1]['result']['tools']}=={'vrs_replay','observe_file_content_equality'})
    check(observer_replies[-1]['id']==3 and observer_replies[-1]['result']['structuredContent']==query_baseline)
    check('error' in agent_query({**query_args,'inputOriginal':n0['original']}))
    check('error' in agent_query({**query_args,'seed':'1'}))
    check(agent_query({},'swegca/end') is None)
    second_owner=identity(232)
    check(c.call('swegca/attach',{'identity':second_owner,'name':'query-selection'})['result']=={})
    check(c.call('swegca/select',{'identity':second_owner})['result']=={})
    selected_first=c.call('swegca/receive',event('query-selection'))['result']
    selected_second=c.call('swegca/receive',event('query-selection',sequence='1'))['result']
    check(agent_query(query_args)['result']['structuredContent']==query_baseline)
    selected_replay=c.call('tools/call',{'name':'vrs_replay','arguments':{
        'receipt':selected_second['receipt'],'inputOriginal':selected_second['original']}})['result']
    check(selected_replay['structuredContent']['original']==selected_first['original'])
    # A stalled/partial side request must not block the native stream.
    with socket.socket(socket.AF_UNIX,socket.SOCK_STREAM) as stalled:
        stalled.connect(str(query_socket));stalled.sendall(b'{')
        check(c.call('ping')['result']=={})
    check(c.call('swegca/select',{'identity':native_id})['result']=={})
    # A lost response may be retried with the same event; no new refinement.
    same=resend_native(raw1,1)['result']
    check(same=={'duplicate':True,'original':n1['original'],'receipt':n1['receipt'],'memory':n1['memory']})
    recovered0=resend_native(raw0,0)['result']
    check(recovered0['duplicate'] and recovered0['original']==n0['original'] and recovered0['memory']==n0['memory'] and recovered0['receipt'] is not None)
    check('error' in resend_native(raw1+' ',1))
    check('error' in resend_native(raw1,1,observed=9))
    check('error' in native_event('Stop',8)[1])
    check('error' in c.call('swegca/retain',event('native-session')))
    played=replay_receipt(n1['receipt'])['structuredContent']
    check(played['media']=='application/json' and bytes.fromhex(played['contentHex'])==raw0.encode())
    check('error' in native_event('UserPromptSubmit',2,session_id='wrong-session',prompt=text)[1])
    check('error' in native_event('UserPromptSubmit',2,prompt=42)[1])
    for seq,name in enumerate(['PostToolUse','Stop','PostCompact','SessionEnd'],2):
        _,reply=native_event(name,seq,reason='other',agent_id='child',unknown={'keep':True})
        check('original' in reply['result'] and 'receipt' not in reply['result'])
    check('structuredContent' in recheck(n1['receipt']))
    check(c.call('swegca/work',{'seed':'7','step':'6'})['result']['merged']=='0')
    live_native_originals=[c.call('swegca/agent/original',{'identity':native_id,'sequence':str(n)})['result'] for n in (0,2,5)]
    c.close()
    check(not query_socket.exists())
    c=Client('open',native_root,path);c.initialize()
    check(c.call('swegca/agent/attach/resume',binding)['result']['identity']==native_id)
    check(c.call('swegca/select',{'identity':native_id})['result']=={})
    restored_native_originals=[c.call('swegca/agent/original',{'identity':native_id,'sequence':str(n)})['result'] for n in (0,2,5)]
    check(restored_native_originals==live_native_originals)
    # No process-local cache survives restart. The immutable cognition metadata
    # restores the original result without adding observations or strength.
    recovered0=resend_native(raw0,0)['result']
    check(recovered0['duplicate'] and recovered0['original']==n0['original'] and recovered0['memory']==n0['memory'] and recovered0['receipt'] is not None)
    before_recovery=sum(p.stat().st_size for p in native_root.rglob('*') if p.is_file())
    recovered1=resend_native(raw1,1)['result']
    check(recovered1['duplicate'] and recovered1['original']==n1['original'] and recovered1['memory']==n1['memory'])
    recovered_play=c.call('tools/call',{'name':'vrs_replay','arguments':{'receipt':recovered1['receipt']}})['result'];assert 'structuredContent' in recovered_play, recovered_play
    restored_play=recovered_play['structuredContent']
    check(restored_play['restored'] and not restored_play['historical'])
    check(c.call('swegca/agent/replay',{'receipt':recovered1['receipt'],'inputOriginal':recovered1['original']})['result']['structuredContent']==restored_play)
    check(c.call('tools/call',{'name':'vrs_replay','arguments':{'receipt':recovered1['receipt'],
        'inputOriginal':recovered1['original']}})['result']['structuredContent']==restored_play)
    check(c.call('tools/call',{'name':'vrs_replay','arguments':{'receipt':recovered1['receipt'],
        'inputOriginal':recovered0['original']}})['result']['isError'])
    check({k:v for k,v in restored_play.items() if k not in ('restored','historical','revision')}==automatic_played)
    restored_revision=c.call('swegca/agent/cognition',{'identity':native_id,'sequence':'1','latest':True})['result']
    check(restored_revision['revision']==restored_play['revision']==restored_revision['liveRevision'])
    check(restored_revision['record']['recovery']==native_checkpoint)
    check(c.call('swegca/agent/cognition',{'identity':native_id,'sequence':'1'})['result']['record']['recovery']==native_checkpoint)
    after_recovery=sum(p.stat().st_size for p in native_root.rglob('*') if p.is_file())
    check(after_recovery>=before_recovery)
    check(c.call('tools/call',{'name':'vrs_replay','arguments':{'receipt':recovered1['receipt']}})['result']['structuredContent']==restored_play)
    check(c.call('swegca/agent/cognition',{'identity':native_id,'sequence':'1'})['result']['record']['recovery']==native_checkpoint)
    check(recheck(recovered1['receipt'])['isError']) # historical judgment is not a new comparison authority
    check(c.call('tools/call',{'name':'vrs_replay','arguments':{'receipt':recovered1['receipt'],'candidate':'0'}})['result']['isError'])
    check(sum(p.stat().st_size for p in native_root.rglob('*') if p.is_file())==after_recovery)
    check('error' in resend_native(raw1+' ',1))
    _,n2=native_event('UserPromptSubmit',6,prompt=text)
    n2=n2['result'];check(n2['candidateCount']=='2')
    check(n2['refinement']['revision']==str(int(n1['refinement']['revision'])+2) and n2['refinement']['strength']==n0['refinement']['strength'])
    check(bytes.fromhex(replay_receipt(n2['receipt'])['structuredContent']['contentHex'])==raw0.encode())
    check(c.call('swegca/end')['result']=={})
    check(c.call('swegca/work',{'seed':'7','step':'6'})['result']['merged']=='1')
    second_binding=dict(binding,instance='desktop-other')
    second_id=c.call('swegca/agent/attach',second_binding)['result']['identity']
    check(second_id!=native_id)
    check(c.call('swegca/select',{'identity':second_id})['result']=={})
    main_raw,main_native=native_event('UserPromptSubmit',0,prompt=text)
    main_native=main_native['result']
    check(not main_native['temporary'] and main_native['candidateCount']=='3')
    main_checkpoint=c.call('swegca/agent/cognition',{'identity':second_id,'sequence':'0'})['result']['record']['recovery']
    check(not main_checkpoint['temporary'] and main_checkpoint['keyKind']==1)
    check(main_checkpoint['observationBoundary']=='0' and main_checkpoint['observationHead']['bytes']=='0')
    check(main_checkpoint['rememberedHead']['bytes']!='0')
    main_automatic=c.call('tools/call',{'name':'vrs_replay','arguments':{'receipt':main_native['receipt']}})['result']['structuredContent']
    check(bytes.fromhex(replay_receipt(main_native['receipt'])['structuredContent']['contentHex'])==raw0.encode())
    check(c.call('swegca/work',{'seed':'7','step':'0'})['result']['merged']=='0')
    c.close()
    c=Client('open',native_root,path);c.initialize()
    check(c.call('swegca/agent/attach/resume',second_binding)['result']['identity']==second_id)
    check(c.call('swegca/select',{'identity':second_id})['result']=={})
    main_recovered=resend_native(main_raw,0)['result']
    check(main_recovered['memory']==main_native['memory'])
    check(c.call('swegca/agent/cognition',{'identity':second_id,'sequence':'0'})['result']['record']['recovery']==main_checkpoint)
    restored_main=c.call('tools/call',{'name':'vrs_replay','arguments':{'receipt':main_recovered['receipt']}})['result']['structuredContent']
    check(restored_main['restored'] and not restored_main['historical'])
    check({k:v for k,v in restored_main.items() if k not in ('restored','historical','revision')}==main_automatic)
    c.close()
    # Commit the first event, deliberately leave its response unread, then kill
    # the transport. Resuming must identify its original from committed storage.
    lost_root=root/'lost-native-reply';lost_root.mkdir()
    c=Client('create',lost_root,path);c.initialize()
    lost_id=c.call('swegca/agent/attach',binding)['result']['identity']
    check(c.call('swegca/select',{'identity':lost_id})['result']=={})
    lost_raw=json.dumps({'session_id':'native-session','hook_event_name':'UserPromptSubmit','prompt':text})
    c.serial+=1
    c.p.stdin.write(json.dumps({'jsonrpc':'2.0','id':c.serial,'method':'swegca/agent/event',
        'params':{'native':lost_raw,'sequence':'0','observedAt':'0','seed':'7','step':'0'}}).encode()+b'\n')
    c.p.stdin.flush()
    check(bool(select.select([c.p.stdout],[],[],10)[0]))
    c.p.kill();check(c.p.wait(timeout=10)==-9)
    c.p.stdin.close();c.p.stdout.close();check(c.p.stderr.read()==b'');c.p.stderr.close()
    c=Client('open',lost_root,path);c.initialize()
    check(c.call('swegca/agent/attach/resume',binding)['result']['identity']==lost_id)
    check(c.call('swegca/select',{'identity':lost_id})['result']=={})
    before_retry=sum(p.stat().st_size for p in lost_root.rglob('*') if p.is_file())
    recovered=resend_native(lost_raw,0)['result'];check(recovered['duplicate'] and recovered['receipt'] is not None and recovered['memory']=={'completed':True,'original':None})
    check(sum(p.stat().st_size for p in lost_root.rglob('*') if p.is_file())==before_retry)
    _,after_loss=native_event('UserPromptSubmit',1,prompt=text)
    after_loss=after_loss['result'];check(after_loss['candidateCount']=='1')
    check(after_loss['candidates'][0]['original']==recovered['original'])
    c.close()
    # A native binding must not accept a store populated through an unrelated
    # ingress. Failed native index recovery remains unselectable.
    invalid_native=root/'invalid-native';invalid_native.mkdir()
    c=Client('create',invalid_native,path);c.initialize()
    check(c.call('swegca/start',{'identity':native_id,'name':'native-session'})['result']=={})
    check('result' in c.call('swegca/retain',event('native-session')))
    c.close()
    c=Client('open',invalid_native,path);c.initialize()
    check('error' in c.call('swegca/agent/attach/resume',binding))
    check('error' in c.call('swegca/select',{'identity':native_id}))
    check('error' in c.call('swegca/end'))
    check(c.call('swegca/work',{'seed':'7','step':'0'})['result']['merged']=='0')
    c.close()
    app_root=root/'app-server-events';app_root.mkdir()
    c=Client('create',app_root,path);c.initialize()
    app_binding={'provider':'codex','instance':'desktop-wire','session':'thread-x','protocol':'app-server'}
    app_id=c.call('swegca/agent/attach',app_binding)['result']['identity']
    check(c.call('swegca/select',{'identity':app_id})['result']=={})
    app_input=[{'type':'text','text':text},{'type':'localImage','path':'/never/open/asset.png'}]
    def app_event(seq,method,params,request_id=None):
        envelope={'method':method,'params':{'threadId':'thread-x',**params}}
        if request_id is not None:envelope['id']=request_id
        raw=json.dumps(envelope,ensure_ascii=False)
        # Embed exact native source, including whitespace and escaped unknown
        # data. Later string retries and Replay must preserve these same bytes.
        raw=raw[:-1]+r', "wireUnknown": "\u0041\\tail" }'
        c.serial+=1
        prefix=json.dumps({'jsonrpc':'2.0','id':c.serial,'method':'swegca/agent/event',
            'params':{'sequence':str(seq),'observedAt':str(seq),'seed':'7','step':str(seq)}})
        frame=prefix[:-2]+',"native":'+raw+'}}'
        reply=c.raw(frame.encode()+b'\n');check(reply['id']==c.serial)
        return raw,reply
    app_raw,a0=app_event(0,'turn/start',{'input':app_input,'unknown':True},1)
    a0=a0['result'];check(a0['candidateCount']=='0')
    _,a1=app_event(1,'turn/steer',{'input':app_input,'expectedTurnId':'turn-x'},2)
    a1=a1['result'];check(a1['candidateCount']=='1')
    check(a1['memory']['completed'] and a1['memory']['original']==a0['original'])
    check(a1['memory']['agreement']==1 and not a1['memory']['reEvidencePerformed'])
    check('structuredContent' in recheck(a1['receipt']))
    app_replay=replay_receipt(a1['receipt'])['structuredContent']
    check(app_replay['source']=='codex/app-server' and bytes.fromhex(app_replay['contentHex'])==app_raw.encode())
    check(resend_native(app_raw,0)['result']['duplicate'])
    for seq,method in enumerate(['item/agentMessage/delta','item/completed','turn/completed','thread/archived'],2):
        _,reply=app_event(seq,method,{'delta':'original output','unknown':{'keep':True}})
        check('original' in reply['result'] and 'receipt' not in reply['result'])
    check('structuredContent' in recheck(a1['receipt']))
    check(c.call('swegca/work',{'seed':'7','step':'6'})['result']['merged']=='0')
    c.close()
    c=Client('open',app_root,path);c.initialize()
    check(c.call('swegca/agent/attach/resume',app_binding)['result']['identity']==app_id)
    check(c.call('swegca/select',{'identity':app_id})['result']=={})
    check(resend_native(app_raw,0)['result']['duplicate'])
    _,a2=app_event(6,'turn/start',{'input':app_input},3)
    check(a2['result']['candidateCount']=='2')
    check(c.call('swegca/end')['result']=={})
    check(c.call('swegca/work',{'seed':'7','step':'6'})['result']['merged']=='1')
    c.close()
    # Hook text and an exact single app-server text share a natural cue. Main
    # publishes only after explicit end; selected Replay keeps the hook bytes.
    shared_root=root/'shared-text-cue';shared_root.mkdir()
    c=Client('create',shared_root,path);c.initialize()
    shared_hook=c.call('swegca/agent/attach',binding)['result']['identity']
    check(c.call('swegca/select',{'identity':shared_hook})['result']=={})
    shared_raw,shared=native_event('UserPromptSubmit',0,prompt=text,unknown={'keep':True})
    shared=shared['result']
    check(c.call('swegca/work',{'seed':'7','step':'0'})['result']['merged']=='0')
    check(c.call('swegca/end')['result']=={})
    check(c.call('swegca/work',{'seed':'7','step':'0'})['result']['merged']=='1')
    c.close()
    c=Client('open',shared_root,path);c.initialize()
    shared_app=c.call('swegca/agent/attach',app_binding)['result']['identity']
    check(c.call('swegca/select',{'identity':shared_app})['result']=={})
    _,shared_reply=app_event(0,'turn/start',{'input':[{'type':'text','text':text}]},81)
    shared_reply=shared_reply['result']
    check(not shared_reply['temporary'] and shared_reply['candidateCount']=='1')
    check(shared_reply['memory']['completed'] and shared_reply['memory']['original']==shared['original'])
    shared_play=replay_receipt(shared_reply['receipt'])['structuredContent']
    check(shared_play['original']==shared['original'] and bytes.fromhex(shared_play['contentHex'])==shared_raw.encode())
    _,shared_again=app_event(1,'turn/start',{'input':[{'type':'text','text':text}]},82)
    check(shared_again['result']['temporary'] and shared_again['result']['candidateCount']=='1')
    check(shared_again['result']['memory']['original']==shared_reply['original'])
    c.close()
    # Real native tool results may carry address-bound recorded observations.
    # Core approval/rejection still comes from independent shuffled experience.
    producers_root=root/'native-producers';producers_root.mkdir()
    c=Client('create',producers_root,path);c.initialize()
    producer_binding={'provider':'codex','instance':'producer-test','session':'producer-thread','protocol':'app-server'}
    producer_owner=c.call('swegca/agent/attach',producer_binding)['result']['identity']
    check(c.call('swegca/select',{'identity':producer_owner})['result']=={})
    producer_seq=0
    def producer_frame(sender,frame,request=None):
        global producer_seq
        n=producer_seq
        p={'sequence':str(n),'observedAt':str(n),'seed':'7','step':str(n),'sender':sender,'native':json.dumps(frame)}
        if request is not None:p['requestSequence']=str(request)
        response=c.call('swegca/agent/event',p)
        assert 'result' in response,response
        result=response['result'];producer_seq+=1
        return n,result
    def producer_trial(prompt,n,outcome,mutate=None,items=None):
        seq,received=producer_frame('client',{'id':producer_seq+1,'method':'turn/start',
            'params':{'threadId':'producer-thread','input':items if items is not None else [{'type':'text','text':prompt}]}})
        turn='producer-turn-'+str(seq)
        producer_frame('server',{'id':seq+1,'result':{'turn':{'id':turn}}},seq)
        observation={'inputOriginal':received['original'],'outcome':outcome,'axis':'0',
            'confidence':1.0,'hasExpiry':False,'expiresAt':'0'}
        if mutate:mutate(observation)
        item={'type':'mcpToolCall','id':'tool-'+str(seq),'server':'recorded-producer-'+str(n),
            'tool':'verify','status':'completed','result':{'content':[],
                'structuredContent':{'swegcaObservation':observation}}}
        frame={'method':'item/completed','params':{'threadId':'producer-thread','turnId':turn,'item':item}}
        output_seq,result=producer_frame('server',frame)
        raw=c.call('swegca/agent/original',{'identity':producer_owner,'sequence':str(output_seq)})['result']
        check(json.loads(raw['native'])==frame and raw['context']==received['original']['digest'])
        return received,result
    support_targets=[]
    for outcome,expected in (('support',1),('refute',2)):
        for n in range(8):
            trial_sequence=producer_seq
            received,result=producer_trial('claim '+outcome,n,outcome)
            if outcome=='support':support_targets.append((received['original'],'producer-turn-'+str(trial_sequence)))
        check(result['refinement']['status']==expected)
        check((result['refinement']['strength']>1) if expected==1 else (result['refinement']['strength']<1))
    for n,mutate in enumerate((lambda v:v.update(axis='999'),lambda v:v.update(confidence=2),
        lambda v:v.update(outcome='approve'),lambda v:v.update(source=identity(250)),
        lambda v:v.update(inputOriginal={**v['inputOriginal'],'digest':identity(250)}),
        lambda v:v.update(scope=''),lambda v:v.update(scope=None),lambda v:v.update(scope={}))):
        received,result=producer_trial('unverified claim '+str(n),n,'support',mutate)
        check(result['refinement']['status']==0 and result['refinement']['strength']==1)
    # A producer can propose an exact user span, but cannot cite its own
    # result, another input, a character offset in place of a UTF-8 byte offset,
    # or an invented requirement. Invalid claims retain their full native bytes
    # as insufficient parent observations, never positive scoped evidence.
    anchored_prompt='파일 내용 유지. 권한도 유지.'
    anchor={'textIndex':'0','byteOffset':str(len('파일 '.encode())), 'quote':'내용 유지'}
    def anchored_scope(anchor):
        return json.dumps({'predicate':'anchored byte comparison','requirement':anchor},
            ensure_ascii=False,separators=(',',':'))
    received,linked=producer_trial(anchored_prompt,0,'support',
        lambda v:v.update(scope=anchored_scope(anchor),requirement=anchor))
    check(linked['refinement']['connection']!=received['refinement']['connection'])
    check(received['refinement']['status']==0)
    for bad in ({**anchor,'quote':'삭제해'}, {**anchor,'byteOffset':'3'},
                {**anchor,'byteOffset':'18446744073709551615'}, {**anchor,'textIndex':'1'},
                {**anchor,'quote':''}, {**anchor,'quote':None},
                {**anchor,'byteOffset':'-1'}, {**anchor,'extra':True}):
        received,rejected=producer_trial(anchored_prompt,1,'support',
            lambda v:v.update(scope=anchored_scope(bad),requirement=bad))
        check(rejected['refinement']['connection']==received['refinement']['connection'])
        check(rejected['refinement']['status']==0 and rejected['refinement']['strength']==1)
    received,rejected=producer_trial(anchored_prompt,1,'support',lambda v:v.update(requirement=anchor))
    check(rejected['refinement']['connection']==received['refinement']['connection'])
    check(rejected['refinement']['status']==0)
    for text_index,valid in (('1',True),('0',False)):
        received,result=producer_trial(anchored_prompt,2,'support',
            lambda v:v.update(scope=anchored_scope({**anchor,'textIndex':text_index}),requirement={**anchor,'textIndex':text_index}),
            items=[{'type':'text','text':'앞선 별도 내용'}, {'type':'text','text':anchored_prompt}])
        check((result['refinement']['connection']!=received['refinement']['connection'])==valid)
    # Two real spans of the same user input must not share a connection merely
    # because a producer reuses a scope label. The full declared scope carries
    # the exact authenticated anchor; removing the outer claim cannot bypass it.
    other_anchor={'textIndex':'0','byteOffset':str(len('파일 내용 유지. '.encode())),
        'quote':'권한도 유지'}
    first_connection=linked['refinement']['connection']
    for mutate in (
        lambda v:v.update(scope=anchored_scope(anchor),requirement=other_anchor),
        lambda v:v.update(scope=anchored_scope(anchor)),
        lambda v:v.update(scope='anchored byte comparison',requirement=anchor),
        lambda v:v.update(scope=json.dumps({'requirement':None}),requirement=anchor),
    ):
        received,rejected=producer_trial(anchored_prompt,3,'support',mutate)
        check(rejected['refinement']['connection']==received['refinement']['connection'])
        check(rejected['refinement']['status']==0)
    received,second=producer_trial(anchored_prompt,4,'refute',
        lambda v:v.update(scope=anchored_scope(other_anchor),requirement=other_anchor))
    check(second['refinement']['connection']!=first_connection)
    check(second['refinement']['connection']!=received['refinement']['connection'])
    _,again=producer_trial(anchored_prompt,5,'support',
        lambda v:v.update(scope=anchored_scope(anchor),requirement=anchor))
    check(again['refinement']['connection']==first_connection)
    check(again['refinement']['revision']=='4')
    # Two measured subclaims of one compound request have independent core
    # connections. Neither is evidence that the entire request was fulfilled.
    compound='Move the file, preserve its contents and permissions.'
    scoped_outputs=[]
    scoped_connections=[]
    scoped_support_targets=[]
    for scope,outcome,expected in (('byte content unchanged','support',1),
                                   ('permissions unchanged','refute',2)):
        for n in range(8):
            trial_sequence=producer_seq
            received,result=producer_trial(compound,n,outcome,lambda v:v.update(scope=scope))
            if scope=='byte content unchanged':
                scoped_support_targets.append((received['original'],'producer-turn-'+str(trial_sequence)))
            check(received['refinement']['status']==0 and received['refinement']['strength']==1)
            check(received['refinement']['connection']!=result['refinement']['connection'])
        check(result['refinement']['status']==expected and result['refinement']['revision']=='16')
        scoped_outputs.append(result['original'])
        scoped_connections.append(result['refinement']['connection'])
    check(scoped_outputs[0]!=scoped_outputs[1])
    check(scoped_connections[0]!=scoped_connections[1])
    # The same declared scope attached to a different parent cannot reuse the
    # first parent's accumulated support, even with identical producer names.
    _,separate=producer_trial('Another file with a different requested result',0,'support',
        lambda v:v.update(scope='byte content unchanged'))
    check(separate['refinement']['status']==0 and separate['refinement']['revision']=='2')
    check(separate['refinement']['connection'] not in scoped_connections)
    # Execute the actual C++ producer on real files, then preserve its complete
    # result through native ingress. No fabricated support/refute labels here.
    measured_left=root/'measured-left';measured_right=root/'measured-right'
    measured_left.write_bytes(b'actual measured bytes\x00\xff')
    measured_right.write_bytes(measured_left.read_bytes())
    measured_seq,measured_input=producer_frame('client',{'id':producer_seq+1,'method':'turn/start',
        'params':{'threadId':'producer-thread','input':[{'type':'text','text':'Check the file contents and preserve permissions.'}]}})
    measured_turn='measured-turn-'+str(measured_seq)
    producer_frame('server',{'id':measured_seq+1,'result':{'turn':{'id':measured_turn}}},measured_seq)
    observed_originals=[];measured_connection=None;measured_packets=[]
    for index,outcome in enumerate(('support','refute','insufficient','refute','support','insufficient')):
        if index==3:measured_right.write_bytes(measured_left.read_bytes())
        if index%3==1:measured_right.write_bytes(b'different measured bytes')
        if index%3==2:measured_right.unlink()
        requests=[{'jsonrpc':'2.0','id':1,'method':'initialize','params':{'protocolVersion':'2025-06-18',
            'capabilities':{},'clientInfo':{'name':'native-observation-test','version':'1'}}},
            {'jsonrpc':'2.0','method':'notifications/initialized'},
            {'jsonrpc':'2.0','id':2,'method':'tools/call','params':{'name':'observe_file_content_equality',
                'arguments':{'inputOriginal':measured_input['original'],'left':str(measured_left),'right':str(measured_right),
                    'requirement':{'textIndex':'0','byteOffset':'10','quote':'file contents'}}}}]
        if index>=3:requests[-1]['params']['arguments']['expectEqual']=False
        measured=subprocess.run([str(exe.parent/'swegca-content-observer'),'16777216','625000000','1048576'],
            input=b''.join(json.dumps(value).encode()+b'\n' for value in requests),capture_output=True,timeout=10,check=True)
        check(measured.stderr==b'')
        actual_result=json.loads(measured.stdout.splitlines()[-1])['result']
        check(actual_result['structuredContent']['swegcaObservation']['outcome']==outcome)
        measured_packets.append(json.loads(json.dumps(actual_result)))
        check('not a task-completion judgment' in actual_result['content'][0]['text'])
        frame={'method':'item/completed','params':{'threadId':'producer-thread','turnId':measured_turn,
            'item':{'type':'mcpToolCall','id':'measured-'+str(index),'server':'swegca-content-observer',
                'tool':'observe_file_content_equality','status':'completed','result':actual_result}}}
        n,recorded=producer_frame('server',frame)
        check(recorded['refinement']['connection']!=measured_input['refinement']['connection'])
        if measured_connection is None:measured_connection=recorded['refinement']['connection']
        if index<3:check(recorded['refinement']['connection']==measured_connection)
        else:
            check(recorded['refinement']['connection']!=measured_connection)
            if index==3:measured_difference_connection=recorded['refinement']['connection']
            check(recorded['refinement']['connection']==measured_difference_connection)
        # One producer/context cannot manufacture independent evidence by
        # repeating a measurement or renaming a tool item.
        check(recorded['refinement']['status']==0)
        observed_originals.append((n,frame,recorded['original']))
    # A reported predicate must agree with its measurement, even with a valid
    # user quote and scope. Preserve contradictory reports as insufficient.
    inconsistent=json.loads(json.dumps(actual_result))
    inconsistent['structuredContent']['swegcaObservation']['outcome']='support'
    bad_frame={'method':'item/completed','params':{'threadId':'producer-thread','turnId':measured_turn,
        'item':{'type':'mcpToolCall','id':'inconsistent-measurement','server':'swegca-content-observer',
        'tool':'observe_file_content_equality','status':'completed','result':inconsistent}}}
    bad_sequence,bad_recorded=producer_frame('server',bad_frame)
    check(bad_recorded['refinement']['connection']==measured_input['refinement']['connection'])
    check(bad_recorded['refinement']['status']==0)
    kept=c.call('swegca/agent/original',{'identity':producer_owner,'sequence':str(bad_sequence)})['result']
    check(json.loads(kept['native'])==bad_frame)
    mutations=[
        lambda x:x['measurement']['right'].update(digest=identity(250)),
        lambda x:x['measurement']['right'].update(path='/unrelated/path'),
        lambda x:x['measurement']['left'].update(readBytes='0'),
        lambda x:x['measurement']['left']['after'].update(mtimeSeconds='999999999'),
        lambda x:x.pop('measurement'),
    ]
    for corrupt in mutations:
        mismatched=json.loads(json.dumps(measured_packets[0]));corrupt(mismatched['structuredContent'])
        bad_frame['params']['item']['result']=mismatched
        # Renaming the tool cannot bypass a declared measured predicate.
        bad_frame['params']['item']['tool']='renamed-file-observer'
        _,rejected=producer_frame('server',bad_frame)
        check(rejected['refinement']['connection']==measured_input['refinement']['connection'])
        check(rejected['refinement']['status']==0)

    # The opposite predicate is checked even when the producer renames its
    # tool. Equal measured bytes cannot support a request for different bytes.
    forged_difference=json.loads(json.dumps(measured_packets[3]))
    forged_difference['structuredContent']['swegcaObservation']['outcome']='support'
    bad_frame['params']['item']['result']=forged_difference
    bad_frame['params']['item']['tool']='renamed-file-observer'
    _,rejected_difference=producer_frame('server',bad_frame)
    check(rejected_difference['refinement']['connection']==measured_input['refinement']['connection'])
    check(rejected_difference['refinement']['status']==0)

    # Different sizes must not skip complete-measurement digest validation.
    # Packet 4 is a genuine unequal-size observation supporting difference.
    for packet_index in (1,4): # equality refutation and difference support
        for operand in ('left','right'):
            for malformed in (None,'bad'):
                broken=json.loads(json.dumps(measured_packets[packet_index]))
                if malformed is None:broken['structuredContent']['measurement'][operand].pop('digest')
                else:broken['structuredContent']['measurement'][operand]['digest']=malformed
                bad_frame['params']['item']['result']=broken
                _,invalid_complete=producer_frame('server',bad_frame)
                check(invalid_complete['refinement']['connection']==measured_input['refinement']['connection'])
                check(invalid_complete['refinement']['status']==0)

    next_sequence=str(producer_seq);c.close()
    c=Client('open',producers_root,path);c.initialize()
    check(c.call('swegca/agent/attach/resume',producer_binding)['result']['nextSequence']==next_sequence)
    check(c.call('swegca/select',{'identity':producer_owner})['result']=={})
    for n,frame,original in observed_originals:
        restored=c.call('swegca/agent/original',{'identity':producer_owner,'sequence':str(n)})['result']
        check(restored['original']==original and json.loads(restored['native'])==frame)
        check(restored['context']==measured_input['original']['digest'])
    # Authenticated anchor scopes keep their identity and selected originals
    # across a process restart; the other user requirement remains separate.
    received,restored_anchor=producer_trial(anchored_prompt,6,'support',
        lambda v:v.update(scope=anchored_scope(anchor),requirement=anchor))
    check(restored_anchor['refinement']['connection']==first_connection)
    check(restored_anchor['refinement']['revision']=='6')
    replayed_anchor=c.call('tools/call',{'name':'vrs_replay','arguments':{
        'receipt':received['receipt'],'scope':anchored_scope(anchor)}})['result']['structuredContent']
    check(replayed_anchor['scope']==anchored_scope(anchor))
    check(replayed_anchor['original']==restored_anchor['original'])
    check(c.call('swegca/agent/replay',{'receipt':received['receipt'],'inputOriginal':received['original'],'scope':anchored_scope(anchor)})['result']['structuredContent']==replayed_anchor)
    check(c.call('tools/call',{'name':'vrs_replay','arguments':{'receipt':received['receipt'],
        'inputOriginal':received['original'],'scope':anchored_scope(anchor)}})['result']['structuredContent']==replayed_anchor)
    check(c.call('tools/call',{'name':'vrs_replay','arguments':{'receipt':received['receipt'],
        'inputOriginal':restored_anchor['original'],'scope':anchored_scope(anchor)}})['result']['isError'])
    # Resume must retain both scoped histories and all native sequence numbers.
    # An exact parent Recall still contains only its input originals.
    scoped_input_sequence=producer_seq
    received,result=producer_trial(compound,8,'support',lambda v:v.update(scope='byte content unchanged'))
    check(received['refinement']['status']==0 and received['refinement']['strength']==1)
    check(received['candidateCount']=='16')
    check(result['refinement']['status']==1 and result['refinement']['revision']=='18')
    check(result['refinement']['connection']==scoped_connections[0])
    def scoped_replay(receipt,scope,**extra):
        return c.call('tools/call',{'name':'vrs_replay','arguments':{'receipt':receipt,'scope':scope,**extra}})['result']
    scoped_receipt=received['receipt']
    scoped_input_original=received['original']
    scope_query={'identity':producer_owner,'inputOriginal':scoped_input_original,
        'scope':'byte content unchanged','latest':True}
    parent_record_before=c.call('swegca/agent/cognition',{'identity':producer_owner,
        'inputOriginal':scoped_input_original,'latest':True})['result']
    parent_before=c.call('tools/call',{'name':'vrs_replay','arguments':{'receipt':scoped_receipt}})['result']['structuredContent']
    selected_scope=scoped_replay(scoped_receipt,'byte content unchanged')['structuredContent']
    check(selected_scope['original']==result['original'] and selected_scope['temporary'])
    check(selected_scope['scope']=='byte content unchanged' and selected_scope['parentCognitionUnchanged'])
    scoped_initial=c.call('swegca/agent/cognition',scope_query)['result']
    check(scoped_initial['revision']==scoped_initial['liveRevision']==selected_scope['revision'])
    check(scoped_initial['record']['observationBoundary']=='9')
    before_scope_repeat=sum(p.stat().st_size for p in producers_root.rglob('*.block'))
    check(scoped_replay(scoped_receipt,'byte content unchanged')['structuredContent']==selected_scope)
    check(sum(p.stat().st_size for p in producers_root.rglob('*.block'))==before_scope_repeat)
    check('error' in c.call('swegca/agent/cognition',{**scope_query,'scope':'different scope'}))
    check('error' in c.call('swegca/agent/cognition',{'identity':producer_owner,'inputOriginal':scoped_input_original,
        'revision':scoped_initial['revision']}))
    check('error' in c.call('swegca/agent/cognition',{'identity':producer_owner,'inputOriginal':scoped_input_original,
        'scope':'different scope','revision':scoped_initial['revision']}))
    for bad_scope in ('','unknown scope',None):
        check(scoped_replay(scoped_receipt,bad_scope)['isError'])
    check(scoped_replay(scoped_receipt,'byte content unchanged',candidate='0')['isError'])
    check(c.call('tools/call',{'name':'vrs_re_evidence','arguments':{'receipt':scoped_receipt,
        'scope':'byte content unchanged','seed':'7','step':'0'}})['result']['isError'])
    # Late counterevidence updates the retained scoped Replay, without silently
    # replacing it by a recent refutation or changing the parent cognition.
    for n,(target,turn) in enumerate(scoped_support_targets):
        scoped_counter={'method':'item/completed','params':{'threadId':'producer-thread','turnId':turn,
            'item':{'type':'mcpToolCall','id':'scoped-counter-'+str(n),'server':'scoped-counter-'+str(n),
                'tool':'verify','status':'completed','result':{'content':[],
                    'structuredContent':{'swegcaObservation':{'inputOriginal':target,'axis':'0',
                        'scope':'byte content unchanged','outcome':'refute','confidence':1.0,
                        'hasExpiry':False,'expiresAt':'0'}}}}}}
        _,scoped_counter_record=producer_frame('server',scoped_counter)
    # This is a read-only journal lookup before another scoped Replay. The
    # late-conflict comparison must already have been persisted before ACK.
    scoped_after_event=c.call('swegca/agent/cognition',scope_query)['result']
    saved_scope_assessment=json.loads(scoped_after_event['record']['replayPrefix']+'"}')['assessment']
    check(saved_scope_assessment['agreement']==3 and saved_scope_assessment['reEvidencePerformed'])
    check(scoped_after_event['revision']!=scoped_initial['revision'])
    refreshed_scope=scoped_replay(scoped_receipt,'byte content unchanged')['structuredContent']
    check(refreshed_scope['revision']==scoped_after_event['revision'])
    check(refreshed_scope['original']==selected_scope['original'])
    check(refreshed_scope['assessment']['agreement']==3 and refreshed_scope['assessment']['status']==2)
    check(refreshed_scope['assessment']['reEvidencePerformed'] and refreshed_scope['assessment']['currentOriginalCount']=='8')
    parent_after=c.call('tools/call',{'name':'vrs_replay','arguments':{'receipt':scoped_receipt}})['result']['structuredContent']
    check(parent_after==parent_before)
    check(c.call('swegca/agent/cognition',{'identity':producer_owner,
        'inputOriginal':scoped_input_original,'latest':True})['result']==parent_record_before)
    current_input_sequence=producer_seq
    _,recalled=producer_frame('client',{'id':producer_seq+1,'method':'turn/start',
        'params':{'threadId':'producer-thread','input':[{'type':'text','text':'claim support'}]}})
    check(recalled['candidateCount']=='16')

    # Restart before the counterevidence. General restoration must reissue an
    # actual comparison receipt, not just export the old assessment.
    current_original=c.call('swegca/agent/original',{'identity':producer_owner,'sequence':str(current_input_sequence)})['result']
    c.close();c=Client('open',producers_root,path);c.initialize()
    check(c.call('swegca/agent/attach/resume',producer_binding)['result']['nextSequence']==str(producer_seq))
    check(c.call('swegca/select',{'identity':producer_owner})['result']=={})
    replay_event={'sequence':str(current_input_sequence),'observedAt':current_original['observedAt'],
        'seed':'7','step':str(current_input_sequence),'sender':'client','native':current_original['native']}
    recovered_parent=c.call('swegca/agent/event',replay_event)['result']
    restored_parent=c.call('tools/call',{'name':'vrs_replay','arguments':{'receipt':recovered_parent['receipt']}})['result']['structuredContent']
    check(restored_parent['restored'] and not restored_parent['historical'])
    check(restored_parent['original']==recalled['memory']['original'])
    # Later counterevidence from the original independent contexts refreshes
    # the selected Replay without requesting Replay/Re-evidence again.
    for n,(target,turn) in enumerate(support_targets):
        producer_frame('server',{'method':'item/completed','params':{'threadId':'producer-thread','turnId':turn,
            'item':{'type':'mcpToolCall','id':'counter-'+str(n),'server':'counter-producer-'+str(n),
                'tool':'verify','status':'completed','result':{'content':[],
                    'structuredContent':{'swegcaObservation':{'inputOriginal':target,'axis':'0',
                        'outcome':'refute','confidence':1.0,'hasExpiry':False,'expiresAt':'0'}}}}}})
    refreshed=c.call('swegca/agent/cognition',{'identity':producer_owner,'sequence':str(current_input_sequence),'latest':True})['result']
    check(refreshed['record']['memory']['agreement']==3 and refreshed['record']['memory']['reEvidencePerformed'])
    check(refreshed['record']['selectedOriginal']==recalled['memory']['original'])
    assessment=json.loads(refreshed['record']['replayPrefix']+'"}')['assessment']
    check(assessment['status']==2 and assessment['currentOriginalCount']=='9')
    check(refreshed['liveRevision']==refreshed['revision'] and refreshed['revision']!=restored_parent['revision'])
    after_counter=c.call('tools/call',{'name':'vrs_replay','arguments':{'receipt':recovered_parent['receipt']}})['result']['structuredContent']
    check(after_counter['original']==restored_parent['original'] and after_counter['revision']==refreshed['revision'])
    check(after_counter['assessment']==assessment)

    c.close()
    c=Client('open',producers_root,path);c.initialize()
    check(c.call('swegca/agent/attach/resume',producer_binding)['result']['nextSequence']==str(producer_seq))

    check(c.call('swegca/select',{'identity':producer_owner})['result']=={})
    recovered_parent=c.call('swegca/agent/event',replay_event)['result']
    twice_restored=c.call('tools/call',{'name':'vrs_replay','arguments':{'receipt':recovered_parent['receipt']}})['result']['structuredContent']
    check(twice_restored==after_counter)
    scoped_recovered=c.call('swegca/agent/cognition',scope_query)['result']
    check(scoped_recovered['record']==scoped_after_event['record'] and scoped_recovered['revision']==scoped_after_event['revision'])
    check(scoped_recovered['liveRevision'] is None)
    # Evidence may arrive after restart before anyone restores a comparison.
    # Resume must use the owner's latest actual refinement parameters.
    check(c.call('swegca/select',{'identity':producer_owner})['result']=={})
    _,before_restore=producer_frame('server',scoped_counter)
    old_input=c.call('swegca/agent/original',{'identity':producer_owner,'sequence':str(scoped_input_sequence)})['result']
    historical=c.call('swegca/agent/event',{'sequence':str(scoped_input_sequence),'observedAt':old_input['observedAt'],
        'seed':'7','step':str(scoped_input_sequence),'sender':'client','native':old_input['native']})['result']
    archived_scope=scoped_replay(historical['receipt'],'byte content unchanged')['structuredContent']
    check(not archived_scope['historical'] and archived_scope['restored'] and archived_scope['revision']!=scoped_after_event['revision'])
    check(archived_scope['original']==refreshed_scope['original'] and archived_scope['assessment']['currentOriginalCount']=='9')
    check(archived_scope['assessment']['status']==2 and archived_scope['assessment']['reEvidencePerformed'])
    check(archived_scope['contentHex']==refreshed_scope['contentHex'])
    check(c.call('swegca/agent/cognition',scope_query)['result']['liveRevision']==archived_scope['revision'])
    # A real late observation refreshes restored comparison before its ACK;
    # no new input and no second Replay request is needed.
    _,after_restart=producer_frame('server',scoped_counter)
    scoped_after_event=c.call('swegca/agent/cognition',scope_query)['result']
    after_restart_assessment=json.loads(scoped_after_event['record']['replayPrefix']+'"}')['assessment']
    check(scoped_after_event['liveRevision']==scoped_after_event['revision'])
    check(scoped_after_event['record']['selectedOriginal']==archived_scope['original'])
    check(after_restart_assessment['currentOriginalCount']=='10' and after_restart_assessment['reEvidencePerformed'])
    recovered_cognition=c.call('swegca/agent/cognition',{'identity':producer_owner,'sequence':str(current_input_sequence),'latest':True})['result']
    check(recovered_cognition['revision']==refreshed['revision'] and recovered_cognition['record']==refreshed['record'])
    check(c.call('swegca/work',{'seed':'7','step':'0'})['result']['merged']=='0')
    check(c.call('swegca/end',{'identity':producer_owner})['result']=={})
    check(c.call('swegca/work',{'seed':'7','step':'0'})['result']['merged']=='1')
    c.close()
    # Merged native evidence and its cognition revision survive a fresh process
    # and can be recalled by another agent session, including counterevidence.
    c=Client('open',producers_root,path);c.initialize()
    archived=c.call('swegca/agent/cognition',{'identity':producer_owner,
        'inputOriginal':recalled['original'],'latest':True})['result']
    check(archived['record']==refreshed['record'] and archived['revision']==refreshed['revision'])
    ended_scope=c.call('swegca/agent/cognition',scope_query)['result']
    check(ended_scope['record']==scoped_after_event['record'] and ended_scope['revision']==scoped_after_event['revision'])
    check(ended_scope['liveRevision'] is None)
    consumer_binding=dict(producer_binding,session='consumer-thread')
    consumer=c.call('swegca/agent/attach',consumer_binding)['result']['identity']
    check(c.call('swegca/select',{'identity':consumer})['result']=={})
    producer_seq=0
    frame={'id':1,'method':'turn/start','params':{'threadId':'consumer-thread',
        'input':[{'type':'text','text':'claim support'}]}}
    _,main_recall=producer_frame('client',frame)
    check(not main_recall['temporary'] and main_recall['candidateCount']=='25')
    replay=c.call('tools/call',{'name':'vrs_replay','arguments':{'receipt':main_recall['receipt']}})['result']['structuredContent']
    replayed=json.loads(bytes.fromhex(replay['contentHex']))
    check(replayed['params']['item']['result']['structuredContent']['swegcaObservation']['outcome']=='refute')
    check(not replay['grantsAuthority'] and replay['session']=='producer-thread')
    frame['id']=2
    _,temporary_recall=producer_frame('client',frame)
    check(temporary_recall['temporary'] and temporary_recall['candidateCount']=='1')
    check(temporary_recall['candidates'][0]['original']==main_recall['original'])
    check(c.call('swegca/work',{'seed':'7','step':'0'})['result']['merged']=='0')
    compound_consumer=c.call('swegca/agent/attach',dict(producer_binding,session='compound-consumer'))['result']['identity']
    check(c.call('swegca/select',{'identity':compound_consumer})['result']=={})
    producer_seq=0
    frame={'id':1,'method':'turn/start','params':{'threadId':'compound-consumer',
        'input':[{'type':'text','text':compound}]}}
    _,compound_main=producer_frame('client',frame)
    check(not compound_main['temporary'] and compound_main['candidateCount']=='17')
    check(compound_main['refinement']['status']==0 and compound_main['refinement']['strength']==1)
    compound_replay=c.call('tools/call',{'name':'vrs_replay','arguments':{'receipt':compound_main['receipt']}})['result']['structuredContent']
    check(compound_replay['assessment']['status']==0 and compound_replay['assessment']['agreement']==1)
    main_scope=scoped_replay(compound_main['receipt'],'byte content unchanged')['structuredContent']
    check(not main_scope['temporary'] and main_scope['original']==after_restart['original'])
    check(json.loads(bytes.fromhex(main_scope['contentHex']))==scoped_counter)
    check(scoped_replay(scoped_receipt,'byte content unchanged')['isError'])
    # Restore a Main-selected scope, then compare new local observations before ACK.
    producer_frame('server',{'id':1,'result':{'turn':{'id':'main-restore-turn'}}},0)
    main_support_targets=[]
    for n in range(8):
        request=producer_seq+100
        seq,prior_input=producer_frame('client',{'id':request,'method':'turn/start','params':{
            'threadId':'compound-consumer','input':[{'type':'text','text':compound}]}})
        turn='main-distinct-'+str(n)
        producer_frame('server',{'id':request,'result':{'turn':{'id':turn}}},seq)
        main_support_targets.append((prior_input['original'],turn))
    main_scope_query={'identity':compound_consumer,'inputOriginal':compound_main['original'],
        'scope':'byte content unchanged','latest':True}
    parent_query={'identity':compound_consumer,'inputOriginal':compound_main['original']}
    main_parent_record=c.call('swegca/agent/cognition',parent_query)['result']['record']
    c.close();c=Client('open',producers_root,path);c.initialize()
    check(c.call('swegca/agent/attach/resume',dict(producer_binding,session='compound-consumer'))['result']['nextSequence']==str(producer_seq))
    check(c.call('swegca/select',{'identity':compound_consumer})['result']=={})
    main_old=c.call('swegca/agent/original',{'identity':compound_consumer,'sequence':'0'})['result']
    main_receipt=c.call('swegca/agent/event',{'sequence':'0','observedAt':main_old['observedAt'],
        'seed':'7','step':'0','sender':'client','native':main_old['native']})['result']['receipt']
    resumed_main=scoped_replay(main_receipt,'byte content unchanged')['structuredContent']
    check(resumed_main['restored'] and not resumed_main['temporary'] and not resumed_main['historical'])
    check(resumed_main['original']==main_scope['original'] and resumed_main['contentHex']==main_scope['contentHex'])
    for n,(target,turn) in enumerate(main_support_targets):
        producer_frame('server',{'method':'item/completed','params':{'threadId':'compound-consumer',
            'turnId':turn,'item':{'type':'mcpToolCall','id':'main-new-'+str(n),
                'server':'main-new-'+str(n),'tool':'verify','status':'completed','result':{'content':[],
                    'structuredContent':{'swegcaObservation':{'inputOriginal':target,
                        'scope':'byte content unchanged','axis':'0','outcome':'support','confidence':1.0,
                        'hasExpiry':False,'expiresAt':'0'}}}}}})
    main_updated=c.call('swegca/agent/cognition',main_scope_query)['result']
    main_assessment=json.loads(main_updated['record']['replayPrefix']+'"}')['assessment']
    check(main_updated['liveRevision']==main_updated['revision'])
    check(main_updated['record']['selectedOriginal']==main_scope['original'])
    check(main_assessment['status']==1 and main_assessment['reEvidencePerformed'] and main_assessment['currentOriginalCount']=='8')
    check(c.call('swegca/agent/cognition',parent_query)['result']['record']==main_parent_record)
    c.close();c=Client('open',producers_root,path);c.initialize()
    check(c.call('swegca/agent/attach/resume',dict(producer_binding,session='compound-consumer'))['result']['nextSequence']==str(producer_seq))
    check(c.call('swegca/select',{'identity':compound_consumer})['result']=={})
    main_receipt=c.call('swegca/agent/event',{'sequence':'0','observedAt':main_old['observedAt'],
        'seed':'7','step':'0','sender':'client','native':main_old['native']})['result']['receipt']
    resumed_again=scoped_replay(main_receipt,'byte content unchanged')['structuredContent']
    check(resumed_again['original']==main_scope['original'] and resumed_again['assessment']==main_assessment)
    # The actual measured experience is retrievable through Main by its scope,
    # with its original failure result, rather than rereading current files.
    measurement_consumer=c.call('swegca/agent/attach',dict(producer_binding,session='measurement-consumer'))['result']['identity']
    check(c.call('swegca/select',{'identity':measurement_consumer})['result']=={})
    producer_seq=0
    _,measured_parent=producer_frame('client',{'id':1,'method':'turn/start','params':{
        'threadId':'measurement-consumer','input':[{'type':'text','text':'Check the file contents and preserve permissions.'}]}})
    # The files now match again. Replay must still return the recorded missing
    # file observation, not silently replace history with a fresh measurement.
    measured_right.write_bytes(measured_left.read_bytes())
    measurement_scope=actual_result['structuredContent']['swegcaObservation']['scope']
    measured_recall=scoped_replay(measured_parent['receipt'],measurement_scope)['structuredContent']
    check(not measured_recall['temporary'] and measured_recall['original']==observed_originals[-1][2])
    check(json.loads(bytes.fromhex(measured_recall['contentHex']))==observed_originals[-1][1])
    c.close()
    # Discover a recorded requirement outcome through the replayed input's
    # experience links, without knowing its scope string. Its comparison has
    # its own durable channel and cannot replace the parent cognition.
    related_root=root/'related-query';related_root.mkdir()
    related_config_path=root/'related-query-config.json'
    related_config_path.write_text(json.dumps(dict(config,frameBytes='16384')))
    c=Client('create',related_root,related_config_path,query_socket=query_socket);c.initialize()
    producer_binding={'provider':'codex','instance':'related-query','session':'producer-thread','protocol':'app-server'}
    producer_owner=c.call('swegca/agent/attach',producer_binding)['result']['identity']
    check(c.call('swegca/select',{'identity':producer_owner})['result']=={})
    producer_seq=0;related_targets=[]
    for n in range(8):
        turn='producer-turn-'+str(producer_seq)
        received,reported=producer_trial('Preserve the requested result.',n,'support',
            lambda v:v.update(scope='measured content requirement'))
        related_targets.append((received['original'],turn))
        if n==0:
            check(c.call('tools/call',{'name':'vrs_replay','arguments':{
                'receipt':received['receipt'],'related':True}})['result']['isError'])
    check(reported['refinement']['status']==1 and reported['refinement']['strength']>1)
    related_sequence=producer_seq
    related_frame={'id':producer_seq+1,'method':'turn/start','params':{'threadId':'producer-thread',
        'input':[{'type':'text','text':'Preserve the requested result.'}]}}
    _,related_input=producer_frame('client',related_frame)
    related_parent_query={'identity':producer_owner,'inputOriginal':related_input['original'],'latest':True}
    check('structuredContent' in c.call('tools/call',{'name':'vrs_re_evidence','arguments':{'receipt':related_input['receipt'],'seed':'7','step':str(related_sequence)}})['result'])
    related_parent_reply=c.call('swegca/agent/cognition',related_parent_query)
    assert 'result' in related_parent_reply,related_parent_reply
    related_parent_before=related_parent_reply['result']['record']
    def related_call(receipt):
        return c.call('swegca/agent/replay',{'receipt':receipt,'inputOriginal':related_input['original'],'related':True})
    discovered=related_call(related_input['receipt'])['result']['structuredContent']
    check(discovered['related'] and discovered['parentCognitionUnchanged'])
    check(discovered['relatedFrom']==related_targets[-1][0])
    check(discovered['original']==reported['original'])
    related_mcp_requests=[{'jsonrpc':'2.0','id':1,'method':'initialize','params':{'protocolVersion':'2025-06-18',
        'capabilities':{},'clientInfo':{'name':'related-test','version':'1'}}},
        {'jsonrpc':'2.0','method':'notifications/initialized'},
        {'jsonrpc':'2.0','id':2,'method':'tools/call','params':{'name':'vrs_replay','arguments':{
            'receipt':related_input['receipt'],'inputOriginal':related_input['original'],'related':True}}}]
    related_mcp=subprocess.run([str(exe.parent/'swegca-content-observer'),'16777216','625000000','1048576'],
        input=b''.join(json.dumps(item).encode()+b'\n' for item in related_mcp_requests),
        env={**os.environ,'SWEGCA_QUERY_SOCKET':str(query_socket)},capture_output=True,timeout=10,check=True)
    check(related_mcp.stderr==b'')
    related_mcp_reply=json.loads(related_mcp.stdout.splitlines()[-1])
    assert 'result' in related_mcp_reply,related_mcp_reply
    check(related_mcp_reply['result']['structuredContent']==discovered)
    def through_observer(arguments,endpoint=query_socket):
        requests=related_mcp_requests[:2]+[{'jsonrpc':'2.0','id':3,'method':'tools/call',
            'params':{'name':'vrs_replay','arguments':arguments}}]
        done=subprocess.run([str(exe.parent/'swegca-content-observer'),'16777216','625000000','1048576'],
            input=b''.join(json.dumps(x).encode()+b'\n' for x in requests),
            env={**os.environ,'SWEGCA_QUERY_SOCKET':str(endpoint)},capture_output=True,timeout=10,check=True)
        check(done.stderr==b'')
        return json.loads(done.stdout.splitlines()[-1])
    input_page_args={'receipt':related_input['receipt'],'inputOriginal':related_input['original'],
        'inputCandidates':{'limit':'1'}}
    page_storage=sum(p.stat().st_size for p in related_root.rglob('*.block'))
    input_page=through_observer(input_page_args)['result']['structuredContent']['inputCandidates']
    check(input_page['inputOriginal']==related_input['original'] and len(input_page['candidates'])==1)
    check(not input_page['semanticVerified'] and not input_page['requirementsComplete'])
    check(sum(p.stat().st_size for p in related_root.rglob('*.block'))==page_storage)
    invalid_page=through_observer({**input_page_args,'inputCandidates':{'limit':'0'}},root/'absent-query.sock')
    check('candidate limit' in invalid_page['error']['message'])
    base_args={'receipt':related_input['receipt'],'inputOriginal':related_input['original'],'related':True}
    observed_page=through_observer({**base_args,'connections':{'limit':'1'}})['result']['structuredContent']
    check(observed_page['selectionOnly'] and not observed_page['requirementsComplete'])
    check(observed_page['next'] is None and len(observed_page['connections'])==1)
    observed_connection=observed_page['connections'][0]['connection']
    observed_replay=through_observer({**base_args,'connection':observed_connection})['result']['structuredContent']
    check(observed_replay['original']==discovered['original'] and observed_replay['relatedConnection']==observed_connection)
    # A nonexistent endpoint distinguishes local syntax rejection from a
    # request that reached (or tried connecting to) the VRS owner.
    for extra,message in (({'connections':{'limit':'0'}},'limit must be'),
        ({'connection':'bad'},'invalid identity'),
        ({'connections':{'limit':'1','after':observed_connection}},'requires snapshot'),
        ({'connection':observed_connection,'connections':{'limit':'1'}},'without listing'),
        ({'related':False,'connections':{'limit':'1'}},'requires related')):
        bad=through_observer({**base_args,**extra},root/'absent-query.sock')
        check(message in bad['error']['message'])

    check(json.loads(bytes.fromhex(discovered['contentHex']))['params']['item']['result']['structuredContent']['swegcaObservation']['scope']=='measured content requirement')
    stable_bytes=sum(p.stat().st_size for p in related_root.rglob('*.block'))
    check(related_call(related_input['receipt'])['result']['structuredContent']==discovered)
    check(sum(p.stat().st_size for p in related_root.rglob('*.block'))==stable_bytes)
    check(c.call('swegca/agent/cognition',related_parent_query)['result']['record']==related_parent_before)
    for fields in ({'scope':'measured content requirement'},{'candidate':'0'},{'related':'true'}):
        check('error' in c.call('swegca/agent/replay',{'receipt':related_input['receipt'],
            'inputOriginal':related_input['original'],'related':True,**fields}))
    # New independent counterevidence arrives without a new input; comparison
    # and its revision must be saved before each observation ACK.
    for n,(target,turn) in enumerate(related_targets):
        producer_frame('server',{'method':'item/completed','params':{'threadId':'producer-thread','turnId':turn,
            'item':{'type':'mcpToolCall','id':'related-counter-'+str(n),'server':'related-counter-'+str(n),
                'tool':'verify','status':'completed','result':{'content':[],
                    'structuredContent':{'swegcaObservation':{'inputOriginal':target,'scope':'measured content requirement',
                        'axis':'0','outcome':'refute','confidence':1.0,'hasExpiry':False,'expiresAt':'0'}}}}}})
    related_journal=c.call('swegca/agent/cognition',{**related_parent_query,'related':True})['result']
    check(related_journal['revision']==related_journal['liveRevision'])
    related_assessment=json.loads(related_journal['record']['replayPrefix']+'"}')['assessment']
    check(related_assessment['status']==2 and related_assessment['reEvidencePerformed'])
    check(related_journal['record']['relatedFrom']==discovered['relatedFrom'])
    contradicted=related_call(related_input['receipt'])['result']['structuredContent']
    check(contradicted['original']==discovered['original'] and contradicted['revision']!=discovered['revision'])
    check(contradicted['assessment']['status']==2 and contradicted['assessment']['reEvidencePerformed'])
    check(contradicted['assessment']['currentOriginalCount']=='8')
    check(contradicted['revision']==related_journal['revision'])
    check(c.call('swegca/agent/cognition',related_parent_query)['result']['record']==related_parent_before)
    c.close();c=Client('open',related_root,related_config_path,query_socket=query_socket);c.initialize()
    check(c.call('swegca/agent/attach/resume',producer_binding)['result']['nextSequence']==str(producer_seq))
    check(c.call('swegca/select',{'identity':producer_owner})['result']=={})
    recovered_related=c.call('swegca/agent/event',{'sequence':str(related_sequence),'observedAt':str(related_sequence),
        'seed':'7','step':str(related_sequence),'sender':'client','native':json.dumps(related_frame)})['result']
    restored_related=related_call(recovered_related['receipt'])['result']['structuredContent']
    check(restored_related==contradicted)
    stable_bytes=sum(p.stat().st_size for p in related_root.rglob('*.block'))
    check(related_call(recovered_related['receipt'])['result']['structuredContent']==restored_related)
    check(sum(p.stat().st_size for p in related_root.rglob('*.block'))==stable_bytes)
    check(c.call('swegca/agent/cognition',related_parent_query)['result']['record']==related_parent_before)
    check(c.call('swegca/work',{'seed':'7','step':str(producer_seq)})['result']['merged']=='0')
    c.close()
    # The same related query must select published Main observations and
    # restore that Main reference after another process restart.
    related_main_root=root/'related-main-query';related_main_root.mkdir()
    c=Client('create',related_main_root,path);c.initialize()
    producer_binding={'provider':'codex','instance':'related-main-query','session':'producer-thread','protocol':'app-server'}
    producer_owner=c.call('swegca/agent/attach',producer_binding)['result']['identity']
    check(c.call('swegca/select',{'identity':producer_owner})['result']=={})
    producer_seq=0
    for n in range(8):
        main_input,main_report=producer_trial('A purpose kept in Main.',n,'support',
            lambda v:v.update(scope='archived requirement outcome'))
    check(main_report['refinement']['status']==1)
    check(c.call('swegca/end')['result']=={})
    check(c.call('swegca/work',{'seed':'7','step':'100'})['result']['merged']=='1')
    main_binding={**producer_binding,'session':'related-main-consumer'}
    producer_owner=c.call('swegca/agent/attach',main_binding)['result']['identity']
    check(c.call('swegca/select',{'identity':producer_owner})['result']=={})
    producer_seq=0
    related_main_frame={'id':1,'method':'turn/start','params':{'threadId':'related-main-consumer',
        'input':[{'type':'text','text':'A purpose kept in Main.'}]}}
    _,related_input=producer_frame('client',related_main_frame)
    check(not related_input['temporary'])
    main_related=related_call(related_input['receipt'])['result']['structuredContent']
    check(main_related['relatedFrom']==main_input['original'] and main_related['original']==main_report['original'])
    main_query={'identity':producer_owner,'inputOriginal':related_input['original'],'related':True,'latest':True}
    main_related_journal=c.call('swegca/agent/cognition',main_query)['result']
    check(not main_related_journal['record']['recovery']['temporary'])
    main_consumer=producer_owner
    producer_owner=c.call('swegca/agent/attach',{**producer_binding,'instance':'late-main-counter'})['result']['identity']
    check(c.call('swegca/select',{'identity':producer_owner})['result']=={})
    producer_seq=0
    for n in range(8):
        producer_trial('A purpose kept in Main.',n+20,'refute',
            lambda v:v.update(scope='archived requirement outcome'))
    check(c.call('swegca/work',{'seed':'7','step':'100'})['result']['merged']=='0')
    check(c.call('swegca/end')['result']=={})
    check(c.call('swegca/work',{'seed':'7','step':'100'})['result']['merged']=='1')
    stale=c.call('swegca/agent/cognition',main_query)['result']
    check(stale['liveRevision'] is None and stale['revision']==main_related_journal['revision'])
    check(c.call('swegca/select',{'identity':main_consumer})['result']=={})
    refreshed=related_call(related_input['receipt'])['result']['structuredContent']
    check(refreshed['original']==main_related['original'] and refreshed['revision']!=main_related['revision'])
    check(refreshed['assessment']['status']==2 and refreshed['assessment']['reEvidencePerformed'])
    check(refreshed['assessment']['currentOriginalCount']=='8')
    main_related=refreshed
    main_related_journal=c.call('swegca/agent/cognition',main_query)['result']
    check(main_related_journal['liveRevision']==main_related['revision'])
    producer_owner=main_consumer
    c.close();c=Client('open',related_main_root,path);c.initialize()
    check(c.call('swegca/agent/attach/resume',main_binding)['result']['nextSequence']=='1')
    check(c.call('swegca/select',{'identity':producer_owner})['result']=={})
    main_recovered=c.call('swegca/agent/event',{'sequence':'0','observedAt':'0','seed':'7','step':'0',
        'sender':'client','native':json.dumps(related_main_frame)})['result']
    check(related_call(main_recovered['receipt'])['result']['structuredContent']==main_related)
    check(c.call('swegca/agent/cognition',main_query)['result']['revision']==main_related_journal['revision'])
    c.close()
    # Actual anchored observations for the automatic recalled-requirement table.
    coverage_root=root/'anchored-coverage';coverage_root.mkdir()
    c=Client('create',coverage_root,path);c.initialize()
    coverage_binding={**producer_binding,'instance':'anchored-coverage'}
    producer_owner=c.call('swegca/agent/attach',coverage_binding)['result']['identity']
    check(c.call('swegca/select',{'identity':producer_owner})['result']=={})
    producer_seq=0
    coverage_prompt='Keep A.\nChange B.\nVerify C.\nLeave D unverified.'
    coverage_anchor={'textIndex':'0','byteOffset':'0','quote':'Keep A.'}
    coverage_parent,coverage_positive=producer_trial(coverage_prompt,0,'support',
        lambda v:v.update(scope=anchored_scope(coverage_anchor),requirement=coverage_anchor))
    coverage_expected=[coverage_positive['original']]
    for phrase,outcome in (('Change B.','refute'),('Verify C.','insufficient')):
        coverage_anchor={'textIndex':'0','byteOffset':str(coverage_prompt.encode().index(phrase.encode())),'quote':phrase}
        _,saved=producer_frame('server',{'method':'item/completed','params':{
            'threadId':'producer-thread','turnId':'producer-turn-0','item':{
            'type':'mcpToolCall','id':'coverage-'+phrase,'server':'coverage-producer','tool':'verify','status':'completed',
            'result':{'content':[],'structuredContent':{'swegcaObservation':{
                'inputOriginal':coverage_parent['original'],'outcome':outcome,
                'scope':anchored_scope(coverage_anchor),'requirement':coverage_anchor,'axis':'0',
                'confidence':1.0,'hasExpiry':False,'expiresAt':'0'}}}}}})
        coverage_expected.append(saved['original'])
    check(c.call('swegca/end')['result']=={})
    check(c.call('swegca/work',{'seed':'7','step':str(producer_seq)})['result']['merged']=='1')
    c.close()
    # Per-connection metadata pages retain distinct scoped outcomes. Listing
    # neither replays originals nor claims all natural-language requirements.
    pages_root=root/'related-connection-pages';pages_root.mkdir()
    c=Client('create',pages_root,path);c.initialize()
    pages_binding={**producer_binding,'instance':'connection-pages'}
    producer_owner=c.call('swegca/agent/attach',pages_binding)['result']['identity']
    check(c.call('swegca/select',{'identity':producer_owner})['result']=={})
    producer_seq=0
    page_parent,page_positive=producer_trial('Keep A, change B, verify C.',0,'support',lambda v:v.update(scope='A'))
    def page_observation(scope,outcome):
        return producer_frame('server',{'method':'item/completed','params':{
            'threadId':'producer-thread','turnId':'producer-turn-0','item':{
            'type':'mcpToolCall','id':'page-'+scope,'server':'page-producer','tool':'verify','status':'completed',
            'result':{'content':[],'structuredContent':{'swegcaObservation':{
                'inputOriginal':page_parent['original'],'outcome':outcome,'scope':scope,'axis':'0',
                'confidence':1.0,'hasExpiry':False,'expiresAt':'0'}}}}}})[1]
    page_negative=page_observation('B','refute')
    page_uncertain=page_observation('C','insufficient')
    page_sequence=producer_seq
    page_frame={'id':99,'method':'turn/start','params':{'threadId':'producer-thread',
        'input':[{'type':'text','text':'Keep A, change B, verify C.'}]}}
    _,page_input=producer_frame('client',page_frame)
    def pages(current,arguments):
        return c.call('swegca/agent/replay',{'receipt':current['receipt'],'inputOriginal':current['original'],
            'related':True,'connections':arguments})
    whole=pages(page_input,{'limit':'64'})['result']['structuredContent']
    check(whole['selectionOnly'] and not whole['requirementsComplete'] and not whole['grantsAuthority'])
    check(whole['relatedFrom']==page_parent['original'] and whole['inputOriginal']==page_input['original'])
    check(len(whole['connections'])==3 and whole['next'] is None)
    check(whole['temporary'] and not whole['mixedTiers'] and all(x['temporary'] for x in whole['connections']))
    check({x['original']['digest'] for x in whole['connections']}=={
        page_positive['original']['digest'],page_negative['original']['digest'],page_uncertain['original']['digest']})
    first=pages(page_input,{'limit':'1'})['result']['structuredContent']
    rest=pages(page_input,{'limit':'2','after':first['next'],'snapshot':first['snapshot']})['result']['structuredContent']
    check(first['connections']+rest['connections']==whole['connections'] and rest['next'] is None)
    for invalid in ({'limit':'0'},{'limit':'65'},{'limit':'1','after':first['next']},
        {'limit':'1','after':first['next'],'snapshot':identity(250)}):
        check('error' in pages(page_input,invalid))
    def connection_replay(current,connection):
        return c.call('swegca/agent/replay',{'receipt':current['receipt'],'inputOriginal':current['original'],
            'related':True,'connection':connection})
    connection_results={}
    for entry in whole['connections']:
        value=connection_replay(page_input,entry['connection'])['result']['structuredContent']
        check(value['original']==entry['original'] and value['relatedConnection']==entry['connection'])
        check(value['relatedFrom']==page_parent['original'] and value['parentCognitionUnchanged'])
        connection_results[entry['connection']]=value
    for connection,value in connection_results.items():
        check(connection_replay(page_input,connection)['result']['structuredContent']==value)
        journal=c.call('swegca/agent/cognition',{'identity':producer_owner,'inputOriginal':page_input['original'],
            'related':True,'connection':connection,'latest':True})['result']
        check(journal['record']['selectedOriginal']==value['original'])
        check(journal['record']['relatedConnection']==connection and journal['revision']==value['revision'])
    check('error' in connection_replay(page_input,identity(250)))
    c.close();c=Client('open',pages_root,path);c.initialize()
    check(c.call('swegca/agent/attach/resume',pages_binding)['result']['nextSequence']==str(producer_seq))
    check(c.call('swegca/select',{'identity':producer_owner})['result']=={})
    restored_page_input=c.call('swegca/agent/event',{'sequence':str(page_sequence),'observedAt':str(page_sequence),
        'seed':'7','step':str(page_sequence),'sender':'client','native':json.dumps(page_frame)})['result']
    restored_pages=pages(restored_page_input,{'limit':'64'})['result']['structuredContent']
    check(restored_pages==whole)
    for connection,value in connection_results.items():
        restored=connection_replay(restored_page_input,connection)['result']['structuredContent']
        check(restored==value)

    page_observation('D','support')
    check('error' in pages(restored_page_input,{'limit':'2','after':first['next'],'snapshot':first['snapshot']}))
    check(len(pages(restored_page_input,{'limit':'64'})['result']['structuredContent']['connections'])==4)
    for connection,value in connection_results.items():
        check(connection_replay(restored_page_input,connection)['result']['structuredContent']==value)
    newer_b=page_observation('B','support')
    latest_listing=pages(restored_page_input,{'limit':'64'})['result']['structuredContent']
    for connection,value in connection_results.items():
        current=connection_replay(restored_page_input,connection)['result']['structuredContent']
        check(current['original']==value['original'])
        if(value['original']==page_negative['original']):
            check(current['revision']!=value['revision'])
            listed_b=next(x for x in latest_listing['connections'] if x['connection']==connection)
            check(listed_b['original']==newer_b['original'] and listed_b['original']!=current['original'])
        else:check(current==value)

    producer_frame('server',{'id':99,'result':{'turn':{'id':'multi-latest-turn'}}},page_sequence)
    multi_expected=[]
    for scope,outcome in (('A','support'),('B','refute'),('C','insufficient')):
        _,saved=producer_frame('server',{'method':'item/completed','params':{'threadId':'producer-thread',
            'turnId':'multi-latest-turn','item':{'type':'mcpToolCall','id':'multi-'+scope,'server':'multi-producer',
            'tool':'verify','status':'completed','result':{'content':[],'structuredContent':{'swegcaObservation':{
            'inputOriginal':page_input['original'],'outcome':outcome,'scope':scope,'axis':'0','confidence':1.0,
            'hasExpiry':False,'expiresAt':'0'}}}}}})
        multi_expected.append(saved['original'])
    check(c.call('swegca/end')['result']=={})
    check(c.call('swegca/work',{'seed':'7','step':str(producer_seq)})['result']['merged']=='1')
    c.close()
    # Regression: a weakened refutation must remain the related observation
    # even when its ordinary turn response retained a higher connection strength.
    bound_root=root/'bound-observation-selection';bound_root.mkdir()
    c=Client('create',bound_root,path);c.initialize()
    bound_binding={**producer_binding,'instance':'bound-observation-selection'}
    producer_owner=c.call('swegca/agent/attach',bound_binding)['result']['identity']
    check(c.call('swegca/select',{'identity':producer_owner})['result']=={})
    producer_seq=0;bound_pairs=[]
    for n in range(8):
        bound_input,bound_report=producer_trial('A purpose kept in Main.',n,'refute',
            lambda v:v.update(scope='archived requirement outcome'))
        bound_pairs.append((bound_input,bound_report))
    def bound_query(current):
        return c.call('swegca/agent/replay',{'receipt':current['receipt'],
            'inputOriginal':current['original'],'related':True})['result']['structuredContent']
    selected_bound=bound_query(bound_input)
    check(selected_bound['relatedFrom']==bound_pairs[-2][0]['original'])
    check(selected_bound['original']==bound_pairs[-2][1]['original'])
    check(json.loads(bytes.fromhex(selected_bound['contentHex']))['params']['item']['result']['structuredContent']['swegcaObservation']['outcome']=='refute')
    check(c.call('swegca/end')['result']=={})
    check(c.call('swegca/work',{'seed':'7','step':'100'})['result']['merged']=='1')
    consumer_binding={**bound_binding,'session':'bound-consumer'}
    bound_consumer=c.call('swegca/agent/attach',consumer_binding)['result']['identity']
    check(c.call('swegca/select',{'identity':bound_consumer})['result']=={})
    producer_seq=0
    bound_frame={'id':1,'method':'turn/start','params':{'threadId':'bound-consumer',
        'input':[{'type':'text','text':'A purpose kept in Main.'}]}}
    _,consumer_input=producer_frame('client',bound_frame)
    from_main=bound_query(consumer_input)
    check(from_main['relatedFrom']==bound_input['original'] and from_main['original']==bound_report['original'])
    main_page=pages(consumer_input,{'limit':'1'})['result']['structuredContent']
    check(not main_page['temporary'] and main_page['next'] is None)
    check(not main_page['mixedTiers'] and not main_page['connections'][0]['temporary'])
    check(main_page['connections'][0]['original']==from_main['original'])
    main_connection=main_page['connections'][0]['connection']
    chosen_main=connection_replay(consumer_input,main_connection)['result']['structuredContent']
    check(chosen_main['original']==from_main['original'] and chosen_main['relatedConnection']==main_connection)


    journal=c.call('swegca/agent/cognition',{'identity':bound_consumer,'inputOriginal':consumer_input['original'],
        'related':True,'latest':True})['result']
    check(journal['record']['recovery']['seedOnly'])
    c.close();c=Client('open',bound_root,path);c.initialize()
    check(c.call('swegca/agent/attach/resume',consumer_binding)['result']['nextSequence']=='1')
    check(c.call('swegca/select',{'identity':bound_consumer})['result']=={})
    recovered=c.call('swegca/agent/event',{'sequence':'0','observedAt':'0','seed':'7','step':'0',
        'sender':'client','native':json.dumps(bound_frame)})['result']
    check(bound_query(recovered)==from_main)
    check(connection_replay(recovered,main_connection)['result']['structuredContent']==chosen_main)

    # Fresh differently worded input after resume must retain the live cursor;
    # do not resend a historical event to reconstruct it after the restart.
    def continuation_event(sequence,text):
        return c.call('swegca/agent/event',{'sequence':str(sequence),
            'observedAt':str(sequence),'seed':'7','step':str(sequence),
            'sender':'client','native':json.dumps({'id':sequence+10,'method':'turn/start',
                'params':{'threadId':'bound-consumer','input':[{'type':'text','text':text}]}})})['result']
    live_followup=continuation_event(1,'Continue from that observation, please.')
    check(live_followup['memory']['original'] is not None)
    # Snapshot the quiescent acknowledged state. Both branches now receive
    # identical next input, with all intervening experience held equal.
    resumed_root=root/'bound-continuation-resume'
    subprocess.run(['cp','-a',str(bound_root),str(resumed_root)],check=True)
    next_text='Keep going with the preceding experience.'
    uninterrupted=continuation_event(2,next_text)
    c.close()
    shutil.rmtree(bound_root);resumed_root.rename(bound_root)
    c=Client('open',bound_root,path);c.initialize()
    check(c.call('swegca/agent/attach/resume',consumer_binding)['result']['nextSequence']=='2')
    check(c.call('swegca/select',{'identity':bound_consumer})['result']=={})
    reopened_followup=continuation_event(2,next_text)
    check(reopened_followup['memory']==uninterrupted['memory'])
    check(reopened_followup['temporary']==uninterrupted['temporary'])
    check(reopened_followup['candidates']==uninterrupted['candidates'])
    c.close()
    # Turn notifications keep the exact originating input, including after
    # restart and with overlapping turns. They are not evidence of success.
    turns_root=root/'native-turns';turns_root.mkdir()
    c=Client('create',turns_root,path);c.initialize()
    turns_binding={'provider':'codex','instance':'turn-test','session':'turn-thread','protocol':'app-server'}
    turns_id=c.call('swegca/agent/attach',turns_binding)['result']['identity']
    check(c.call('swegca/select',{'identity':turns_id})['result']=={})
    def turn_frame(n,sender,frame,request=None):
        p={'sequence':str(n),'observedAt':str(n),'seed':'7','step':str(n),
            'sender':sender,'native':json.dumps(frame)}
        if request is not None:p['requestSequence']=str(request)
        return c.call('swegca/agent/event',p)['result']
    def start_turn(n,rpc,text):
        return turn_frame(n,'client',{'id':rpc,'method':'turn/start',
            'params':{'threadId':'turn-thread','input':[{'type':'text','text':text}]}})
    first_terms='\n'.join('requirement '+str(i) for i in range(12))
    first_turn=start_turn(0,1,first_terms)
    candidate_arguments={'receipt':first_turn['receipt'],'inputOriginal':first_turn['original'],
        'inputCandidates':{'limit':'3'}}
    collected=[]
    while True:
        candidate_page=c.call('swegca/agent/replay',candidate_arguments)['result']['structuredContent']['inputCandidates']
        check(candidate_page['inputOriginal']==first_turn['original'])
        check(not candidate_page['semanticVerified'] and not candidate_page['requirementsComplete'])
        collected.extend(candidate_page['candidates'])
        if candidate_page['next'] is None:break
        candidate_arguments['inputCandidates']['after']=candidate_page['next']
    check(len(collected)==12 and ''.join(x['quote'] for x in collected)==first_terms)
    for bad in ({'limit':'0'},{'limit':'65'},{'limit':'3','after':{'textIndex':'0','byteOffset':'1'}},
                {'limit':'3','after':{'textIndex':'9','byteOffset':'0'}}):
        check('error' in c.call('swegca/agent/replay',{**candidate_arguments,'inputCandidates':bad}))
    check('error' in c.call('swegca/agent/replay',{**candidate_arguments,'related':True}))
    turn_frame(1,'server',{'id':1,'result':{'turn':{'id':'turn-a'}}},0)
    second_turn=start_turn(2,2,'second independent purpose')
    check('error' in c.call('swegca/agent/replay',candidate_arguments))
    turn_frame(3,'server',{'id':2,'result':{'turn':{'id':'turn-b'}}},2)
    c.close()
    c=Client('open',turns_root,path);c.initialize()
    check(c.call('swegca/agent/attach/resume',turns_binding)['result']['nextSequence']=='4')
    check(c.call('swegca/select',{'identity':turns_id})['result']=={})
    for n,turn,target in ((4,'turn-a',first_turn),(5,'turn-b',second_turn)):
        frame={'method':'item/completed','params':{'threadId':'turn-thread','turnId':turn,
            'item':{'type':'commandExecution','id':'command-'+turn,'exitCode':0}}}
        result=turn_frame(n,'server',frame)
        check(result['refinement']['status']==0)
        original=c.call('swegca/agent/original',{'identity':turns_id,'sequence':str(n)})['result']
        check(original['context']==target['original']['digest'])
        check(json.loads(original['native'])==frame)
        check(turn_frame(n,'server',frame)['duplicate'])
    unknown={'method':'item/completed','params':{'threadId':'turn-thread','turnId':'unknown'}}
    turn_frame(6,'server',unknown)
    original=c.call('swegca/agent/original',{'identity':turns_id,'sequence':'6'})['result']
    check(original['context'] not in (first_turn['original']['digest'],second_turn['original']['digest']))
    # Another thread cannot acquire this owner's turn binding.
    check('error' in c.call('swegca/agent/event',{'sequence':'7','observedAt':'7','seed':'7','step':'7',
        'sender':'server','native':json.dumps({'method':'item/completed',
            'params':{'threadId':'other-thread','turnId':'turn-a'}})}))
    # Conflicting same-thread ownership cannot silently choose either purpose.
    fourth=start_turn(7,4,'fourth purpose')
    turn_frame(8,'server',{'id':4,'result':{'turn':{'id':'turn-a'}}},7)
    turn_frame(9,'server',{'method':'item/completed','params':{'threadId':'turn-thread','turnId':'turn-a'}})
    ambiguous=c.call('swegca/agent/original',{'identity':turns_id,'sequence':'9'})['result']
    check(ambiguous['context'] not in (first_turn['original']['digest'],fourth['original']['digest']))
    # Steer is another real input in the same turn; unaddressed output cannot
    # silently remain attached to its initial purpose after the correction.
    steer=turn_frame(10,'client',{'id':5,'method':'turn/steer','params':{'threadId':'turn-thread',
        'expectedTurnId':'turn-b','input':[{'type':'text','text':'Correction: "purpose" -> "updated purpose"'}]}})
    check(steer['inputRelations']['priorInput']==second_turn['original'])
    steer_ref=steer['inputRelations']['revisionReferences'][0]
    check(steer_ref['locationStatus']=='unique')
    check(steer_ref['priorAnchor']=={'textIndex':'0','byteOffset':'19','quote':'purpose'})
    check(not steer_ref['antecedentVerified'] and not steer_ref['replacementVerified'])
    check(not steer['inputRelations']['replacementVerified'] and not steer['inputRelations']['grantsAuthority'])
    turn_frame(11,'server',{'id':5,'result':{'turnId':'turn-b'}},10)
    turn_frame(12,'server',{'method':'item/completed','params':{'threadId':'turn-thread','turnId':'turn-b'}})
    unbound=c.call('swegca/agent/original',{'identity':turns_id,'sequence':'12'})['result']
    check(unbound['context'] not in (second_turn['original']['digest'],steer['original']['digest']))
    c.close()
    c=Client('open',turns_root,path);c.initialize()
    check(c.call('swegca/agent/attach/resume',turns_binding)['result']['nextSequence']=='13')
    check(c.call('swegca/select',{'identity':turns_id})['result']=={})
    recovered_steer=turn_frame(10,'client',{'id':5,'method':'turn/steer','params':{'threadId':'turn-thread',
        'expectedTurnId':'turn-b','input':[{'type':'text','text':'Correction: "purpose" -> "updated purpose"'}]}})
    check(recovered_steer['duplicate'] and recovered_steer['inputRelations']==steer['inputRelations'])
    stored_steer=c.call('swegca/agent/cognition',{'identity':turns_id,'sequence':'10'})['result']['record']
    check(stored_steer['inputRelations']==steer['inputRelations'])
    for n,target in ((13,steer),(14,second_turn)):
        tool={'method':'item/completed','params':{'threadId':'turn-thread','turnId':'turn-b',
            'item':{'type':'mcpToolCall','id':'steer-test-'+str(n),'server':'steer-verifier','tool':'verify','status':'completed',
                'result':{'content':[],'structuredContent':{'swegcaObservation':{'inputOriginal':target['original'],
                    'axis':'0','outcome':'support','confidence':1.0,'hasExpiry':False,'expiresAt':'0'}}}}}}
        turn_frame(n,'server',tool)
        saved=c.call('swegca/agent/original',{'identity':turns_id,'sequence':str(n)})['result']
        check(saved['context']==target['original']['digest'])
    # A different turn's original cannot be claimed as the corrected input.
    tool['params']['item']['result']['structuredContent']['swegcaObservation']['inputOriginal']=first_turn['original']
    turn_frame(15,'server',tool)
    saved=c.call('swegca/agent/original',{'identity':turns_id,'sequence':'15'})['result']
    check(saved['context'] not in (first_turn['original']['digest'],second_turn['original']['digest'],steer['original']['digest']))
    ambiguous_steer=turn_frame(16,'client',{'id':6,'method':'turn/steer','params':{'threadId':'turn-thread',
        'expectedTurnId':'turn-b','input':[{'type':'text','text':'change only the last condition'}]}})
    check(ambiguous_steer['inputRelations']['priorInput'] is None)
    unknown_steer=turn_frame(17,'client',{'id':7,'method':'turn/steer','params':{'threadId':'turn-thread',
        'expectedTurnId':'unknown','input':[{'type':'text','text':'keep the other requirements'}]}})
    check(unknown_steer['inputRelations']['priorInput'] is None)

    check(c.call('swegca/work',{'seed':'7','step':'0'})['result']['merged']=='0')
    c.close()
    # Explicit end permits Main merge; the original correction receipt remains
    # addressable by original after reopening without a live session binding.
    c=Client('open',turns_root,path);c.initialize()
    check(c.call('swegca/agent/attach/resume',turns_binding)['result']['nextSequence']=='18')
    check(c.call('swegca/select',{'identity':turns_id})['result']=={})
    check(c.call('swegca/end')['result']=={})
    check(c.call('swegca/work',{'seed':'7','step':'0'})['result']['merged']=='1')
    c.close()
    c=Client('open',turns_root,path);c.initialize()
    merged_steer=c.call('swegca/agent/cognition',{'identity':turns_id,'inputOriginal':steer['original']})['result']['record']
    check(merged_steer['inputRelations']==steer['inputRelations'])
    check(c.call('swegca/work',{'seed':'7','step':'0'})['result']['merged']=='0')
    c.close()
    large_candidates_root=root/'large-input-candidates';large_candidates_root.mkdir()
    large_candidates_config=root/'large-input-candidates.json'
    large_candidates_config.write_text(json.dumps(dict(config,frameBytes=str(1<<20),readLimit=str(2<<20),sessionBlockBytes=str(4<<20))))
    c=Client('create',large_candidates_root,large_candidates_config);c.initialize()
    large_binding={**turns_binding,'instance':'large-candidate-input'}
    large_owner=c.call('swegca/agent/attach',large_binding)['result']['identity']
    check(c.call('swegca/select',{'identity':large_owner})['result']=={})
    large_code='```\n'+('x \"quoted\" \\ y\n'*6000)+'```'
    large_frame={'id':1,'method':'turn/start','params':{'threadId':'turn-thread','input':[{'type':'text','text':large_code}]}}
    large_input=turn_frame(0,'client',large_frame)
    large_args={'receipt':large_input['receipt'],'inputOriginal':large_input['original'],'inputCandidates':{'limit':'1'}}
    small_page=c.call('swegca/agent/replay',large_args)['result']['structuredContent']['inputCandidates']
    check(small_page['byteLimited'] and small_page['candidates']==[] and small_page['next']=={'textIndex':'0','byteOffset':'0'})
    expanded_args={**large_args,'inputCandidates':{'limit':'1','byteBudget':str(1<<20),'after':small_page['next']}}
    expanded_page=c.call('swegca/agent/replay',expanded_args)['result']['structuredContent']['inputCandidates']
    check(not expanded_page['byteLimited'] and expanded_page['next'] is None)
    check(expanded_page['byteBudget']==str(1<<20) and expanded_page['candidates'][0]['quote']==large_code)
    for invalid in ('0','16777217','-1'):
        check('error' in c.call('swegca/agent/replay',{**large_args,'inputCandidates':{'limit':'1','byteBudget':invalid}}))
    c.close()
    # Responses bind to an earlier committed request original, not a selected
    # session guess or a volatile pending map. The relation survives restart.
    response_root=root/'app-responses';response_root.mkdir()
    c=Client('create',response_root,path);c.initialize()
    response_id=c.call('swegca/agent/attach',app_binding)['result']['identity']
    check(c.call('swegca/select',{'identity':response_id})['result']=={})
    def app_response(seq,request_seq,reply_id=1):
        raw=json.dumps({'id':reply_id,'result':{'exact':'response 원문','unknown':True}},ensure_ascii=False)
        params={'sequence':str(seq),'observedAt':str(seq),'seed':'7','step':str(seq),'native':raw}
        if request_seq is not None:params['requestSequence']=str(request_seq)
        return raw,c.call('swegca/agent/event',params)
    _,r0=app_event(0,'turn/start',{'input':app_input},1)
    check('error' in app_response(1,0,reply_id=9)[1])
    check('error' in app_response(1,None)[1])
    reply_raw,r1=app_response(1,0);r1=r1['result']
    check(r1['refinement']['status']==0)
    r2_raw,r2=app_event(2,'turn/start',{'input':app_input},1)
    r2=r2['result'];check(r2['candidateCount']=='1')
    def response_packet(receipt):
        return c.call('tools/call',{'name':'vrs_replay','arguments':{'receipt':receipt}})['result']['structuredContent']
    before_response=response_packet(r2['receipt'])
    check(before_response['assessment']['currentOriginalCount']=='1')
    check(partial(r2['receipt'],0,1)['structuredContent']['partial'])
    _,r3=app_response(3,2);r3=r3['result']
    retried_input=resend_native(r2_raw,2)['result']
    check(retried_input['receipt']==r2['receipt'] and retried_input['memory']['completed'])
    after_response=response_packet(r2['receipt'])
    check(after_response['original']==before_response['original'])
    check(after_response['assessment']['currentOriginalCount']=='2' and after_response['assessment']['step']=='3')
    check(after_response['assessment']['agreement']==1 and after_response['assessment']['status']==0)
    check(not after_response['assessment']['reEvidencePerformed'] and not after_response['grantsAuthority'])
    check(response_packet(r2['receipt'])==after_response)
    check(partial(r2['receipt'],0,1)['structuredContent']['partial'])
    check(response_packet(r2['receipt'])==after_response)
    def cognition_record(sequence, revision=None):
        params={'identity':response_id,'sequence':str(sequence)}
        if revision is not None:params['revision']=revision
        return c.call('swegca/agent/cognition',params)
    initial_cognition=cognition_record(2)['result']
    updated_revision=initial_cognition['liveRevision']
    check(updated_revision is not None and initial_cognition['revision'] is None)
    updated_cognition=cognition_record(2,updated_revision)['result']['record']
    check(updated_cognition['inputOriginal']==r2['original'])
    check(updated_cognition['seed']=='7' and updated_cognition['step']=='3')
    check(updated_cognition['recovery']==initial_cognition['record']['recovery'])
    check(updated_cognition['recovery']['observationBoundary']=='2')
    saved_packet=json.loads(updated_cognition['replayPrefix']+after_response['contentHex']+'"}')
    check(isinstance(after_response['relatedAvailable'],bool))
    check(saved_packet=={k:v for k,v in after_response.items() if k!='relatedAvailable'})
    check(json.loads(initial_cognition['record']['replayPrefix']+before_response['contentHex']+'"}')==
        {k:v for k,v in before_response.items() if k!='relatedAvailable'})
    check('error' in cognition_record(0,updated_revision))
    check('error' in cognition_record(2,identity(250)))
    revision_bytes=sum(p.stat().st_size for p in response_root.rglob('*') if p.is_file())
    check(response_packet(r2['receipt'])==after_response)
    check(sum(p.stat().st_size for p in response_root.rglob('*') if p.is_file())==revision_bytes)
    check('error' in app_response(3,0)[1]) # Same wire ID/body, wrong original lineage.
    check(app_response(3,2)[1]['result']['duplicate'])
    check('error' in app_response(4,1)[1]) # A response cannot masquerade as a request.
    _,r4=app_event(4,'turn/start',{'input':app_input},2)
    r4=r4['result'];check(r4['candidateCount']=='2')
    check('structuredContent' in c.call('tools/call',{'name':'vrs_re_evidence','arguments':{
        'receipt':r4['receipt'],'seed':'19','step':'5'}})['result'])
    explicit_revision=cognition_record(4)['result']['liveRevision']
    explicit_record=cognition_record(4,explicit_revision)['result']['record']
    check(explicit_revision is not None and explicit_record['seed']=='19' and explicit_record['step']=='5')
    check(json.loads(explicit_record['replayPrefix']+'"}')['assessment']['step']=='5')
    c.close()
    c=Client('open',response_root,path);c.initialize()
    check(c.call('swegca/agent/attach/resume',app_binding)['result']['identity']==response_id)
    check(c.call('swegca/select',{'identity':response_id})['result']=={})
    historic=resend_native(r2_raw,2)['result']
    restored_response=response_packet(historic['receipt'])
    check(restored_response['restored'] and not restored_response['historical'])
    check(restored_response['original']==before_response['original'] and restored_response['contentHex']==before_response['contentHex'])
    check(restored_response['assessment']['rememberedHead']==before_response['assessment']['rememberedHead'])
    check(int(restored_response['assessment']['currentOriginalCount'])>int(after_response['assessment']['currentOriginalCount']))
    check(not restored_response['assessment']['reEvidencePerformed'])
    check(cognition_record(2,updated_revision)['result']['record']==updated_cognition)
    check(cognition_record(4,explicit_revision)['result']['record']==explicit_record)
    check('error' in app_response(3,0)[1])
    check(app_response(3,2)[1]['result']=={'duplicate':True,'original':r3['original'],'receipt':None})
    _,r5=app_response(5,4,reply_id=2);r5=r5['result']
    check(r5['refinement']['status']==0)
    _,r6=app_event(6,'turn/start',{'input':app_input},3)
    r6=r6['result'];check(r6['candidateCount']=='3')
    check('structuredContent' in replay_receipt(r6['receipt']))
    _,continued=app_event(7,'turn/start',{'input':[{'type':'text','text':'continue recalled connection'}]},4)
    continued=continued['result']
    selected=next(item for item in continued['candidates'] if item['original']==r1['original'])
    response_replay=c.call('tools/call',{'name':'vrs_replay','arguments':{
        'receipt':continued['receipt'],'candidate':selected['index']}})['result']['structuredContent']
    check(bytes.fromhex(response_replay['contentHex'])==reply_raw.encode())
    manual_arguments={'receipt':continued['receipt'],'seed':'13','step':'7'}
    manual=c.call('tools/call',{'name':'vrs_re_evidence','arguments':manual_arguments})['result']['structuredContent']
    manual_revision=manual['revision']
    manual_record=cognition_record(7,manual_revision)['result']['record']
    check(manual_record['inputOriginal']==continued['original'] and manual_record['selectedOriginal']==r1['original'])
    check(manual_record['sourceSession']==response_id and manual_record['seed']=='13' and manual_record['step']=='7')
    check(manual_record['comparison']=={k:v for k,v in manual.items() if k!='revision'})
    manual_checkpoint=manual_record['recovery']
    check(manual_checkpoint['temporary'] and manual_checkpoint['keyKind']==2)
    check(manual_checkpoint['lookupKey']==manual_checkpoint['connection']==r1['refinement']['connection'])
    check(manual_checkpoint['inputCue']==continued['refinement']['connection'])
    check(manual_checkpoint['inputCue']!=manual_checkpoint['lookupKey'])
    check(manual_checkpoint['originalIndex']=='1')
    check(manual_checkpoint['rememberedHead']==manual['rememberedHead'])
    check(not manual['reEvidencePerformed'])
    before_manual_retry=sum(p.stat().st_size for p in response_root.rglob('*') if p.is_file())
    check(c.call('tools/call',{'name':'vrs_re_evidence','arguments':manual_arguments})['result']['structuredContent']==manual)
    check(sum(p.stat().st_size for p in response_root.rglob('*') if p.is_file())==before_manual_retry)
    check(partial(continued['receipt'],0,1)['structuredContent']['partial'])
    check(c.call('tools/call',{'name':'vrs_re_evidence','arguments':manual_arguments})['result']['isError'])
    check(c.call('swegca/work',{'seed':'7','step':'7'})['result']['merged']=='0')
    check(c.call('swegca/end')['result']=={})
    def archived_cognition(revision=updated_revision, source=response_id, original=r2['original']):
        params={'identity':source,'inputOriginal':original}
        if revision is not None:params['revision']=revision
        return c.call('swegca/agent/cognition',params)
    check(archived_cognition()['result']=={'revision':updated_revision,'liveRevision':None,'record':updated_cognition})
    check(archived_cognition(None)['result']['record']==initial_cognition['record'])
    check('error' in c.call('swegca/agent/cognition',{'identity':response_id,'sequence':'2','inputOriginal':r2['original']}))
    check('error' in archived_cognition(source=identity(249)))
    forged_original=dict(r2['original'],digest=identity(249))
    check('error' in archived_cognition(original=forged_original))
    check(c.call('swegca/start',{'identity':identity(248),'name':'history-neighbor'})['result']=={})
    check(archived_cognition()['result']['record']==updated_cognition)
    check('result' in c.call('swegca/receive',event('history-neighbor')))
    check(c.call('swegca/work',{'seed':'7','step':'7'})['result']['merged']=='1')
    check(archived_cognition()['result']['record']==updated_cognition)
    c.close()
    c=Client('open',response_root,path);c.initialize()
    check(archived_cognition()['result']['record']==updated_cognition)
    check(archived_cognition(None)['result']['record']==initial_cognition['record'])
    check(archived_cognition(explicit_revision,original=r4['original'])['result']['record']==explicit_record)
    check(archived_cognition(manual_revision,original=continued['original'])['result']['record']==manual_record)
    latest_query={'identity':response_id,'inputOriginal':continued['original'],'latest':True}
    latest_cognition=c.call('swegca/agent/cognition',latest_query)['result']
    check(latest_cognition=={'revision':manual_revision,'liveRevision':None,'record':manual_record})
    check('error' in c.call('swegca/agent/cognition',dict(latest_query,revision=manual_revision)))
    check('error' in c.call('swegca/agent/cognition',dict(latest_query,latest='true')))
    check(c.call('swegca/agent/cognition',dict(latest_query,latest=False))['result']['revision'] is None)
    check('error' in c.call('swegca/select',{'identity':response_id}))
    c.close()
    # Response routing follows the sealed connection, even when its identity
    # is not a digest reconstructed from this adapter's current input encoding.
    sealed_root=root/'sealed-request-connection';sealed_root.mkdir()
    c=Client('create',sealed_root,path);c.initialize()
    check(c.call('swegca/start',{'identity':response_id,'name':'thread-x'})['result']=={})
    check(c.call('swegca/define',{'identity':identity(251)})['result']=={})
    sealed_request=json.dumps({'id':71,'method':'turn/start','params':{'threadId':'thread-x','input':app_input}})
    params=event('thread-x','codex/app-server',media='application/json',content=sealed_request)
    params['observation']={'hypothesis':identity(251),'source':identity(252),'context':identity(253),
        'producer':identity(254),'expiresAt':'0','hasExpiry':False,'confidence':1.0,'axis':'0','outcome':'insufficient'}
    sealed_original=c.call('swegca/observe',params)['result']['original'];c.close()
    c=Client('open',sealed_root,path);c.initialize()
    check(c.call('swegca/agent/attach/resume',app_binding)['result']['identity']==response_id)
    check(c.call('swegca/select',{'identity':response_id})['result']=={})
    _,sealed_response=app_response(1,0,reply_id=71)
    check('result' in sealed_response)
    check(sealed_response['result']['refinement']['revision']=='4')
    relation=c.call('swegca/agent/original',{'identity':response_id,'sequence':'1'})['result']
    check(relation['context']==sealed_original['digest'])
    c.close()
    # The C++ wire owner chooses thread and requestSequence automatically. Only
    # acknowledge its plan after the real VRS process confirms ingestion.
    transport_path=root/'transport-resources.json'
    transport_path.write_text(json.dumps(dict(config,frameBytes='131072')))
    proxy_root=root/'proxy-process';proxy_root.mkdir()
    proxy_config=root/'proxy.json'
    proxy_config.write_text(json.dumps({'memoryBytes':str(8<<20),'frameBytes':'4096',
        'pendingRequests':'16','sessionCapacity':'5','connectionSession':{'session':'transport','mode':'attach'},'seed':'7','step':'0','instance':'proxy-tested',
        'sessions':[{'session':name,'mode':'attach'} for name in ('a','b')]}))
    client_side,client_proxy=socket.socketpair()
    server_side,server_proxy=socket.socketpair()
    vrs_side,vrs_proxy=socket.socketpair()
    vrs_process=subprocess.Popen([str(exe),'create',str(proxy_root),str(transport_path)],
        stdin=vrs_side,stdout=vrs_side,stderr=subprocess.PIPE)
    proxy_process=subprocess.Popen([str(exe.parent/'swegca-app-server-proxy'),
        str(client_proxy.fileno()),str(server_proxy.fileno()),str(vrs_proxy.fileno()),str(proxy_config),"--new-peer"],
        pass_fds=(client_proxy.fileno(),server_proxy.fileno(),vrs_proxy.fileno()),
        stdout=subprocess.PIPE,stderr=subprocess.PIPE)
    for endpoint in (client_proxy,server_proxy,vrs_side,vrs_proxy):endpoint.close()
    client_side.settimeout(10);server_side.settimeout(10)
    try:
        check(bool(select.select([proxy_process.stdout],[],[],10)[0]))
        check(proxy_process.stdout.readline()==b'ready\n')
        def proxy_frame(value):return json.dumps(value,ensure_ascii=False).encode()+b'\n'
        def proxy_read(endpoint):
            data=bytearray()
            while not data.endswith(b'\n'):
                chunk=endpoint.recv(1)
                if not chunk:raise RuntimeError('proxy closed before complete frame')
                data.extend(chunk)
            return bytes(data)
        def check_context_forward(forwarded, original, remembered=None):
            view=json.loads(forwarded);native=json.loads(original)
            context=view['params']['input'].pop(0)
            check(view==native and context['type']=='text')
            packet=json.loads(context['text'].split('\n',1)[1])
            check(packet['grantsAuthority'] is False)
            check(isinstance(packet['receipt'],str) and int(packet['receipt'])>0)
            if remembered is None:
                check(packet['recalledOriginal'] is None and set(packet['inputOriginal'])=={'block','digest','offset','bytes'})
            else:
                check(packet['assessment']['status']==0)
                check(json.loads(packet['content'])==json.loads(remembered))
            return packet
        global_frames=[{'id':1,'method':'initialize','params':{'clientInfo':{'name':'fixture'},'padding':'"'*1500}},
                       {'method':'initialized'}, {'id':2,'method':'thread/start','params':{}}]
        expanded=json.dumps({'native':proxy_frame(global_frames[0])[:-1].decode()}).encode()
        check(len(proxy_frame(global_frames[0]))<4096 and len(expanded)>4096)
        for frame in global_frames:
            raw=proxy_frame(frame);client_side.sendall(raw);check(proxy_read(server_side)==raw)
            if 'id' in frame:
                raw=proxy_frame({'id':frame['id'],'result':{}})
                server_side.sendall(raw);check(proxy_read(client_side)==raw)
        request=proxy_frame({'id':7,'method':'turn/start','params':{'threadId':'a',
            'input':[{'type':'text','text':'프록시 원문'}]}})
        note=proxy_frame({'method':'item/agentMessage/delta','params':{'threadId':'a','delta':'보존'}})
        # Fragment first frame, then coalesce its tail with the next whole frame.
        client_side.sendall(request[:5]);client_side.sendall(request[5:]+note)
        first_reference=check_context_forward(proxy_read(server_side),request);check(proxy_read(server_side)==note)
        response=proxy_frame({'id':7,'result':{'ok':True}})
        approval=proxy_frame({'id':7,'method':'item/commandExecution/requestApproval',
            'params':{'threadId':'b','command':'never executed'}})
        server_side.sendall(response+approval)
        check(proxy_read(client_side)==response);check(proxy_read(client_side)==approval)
        client_side.sendall(response);check(proxy_read(server_side)==response)
        started=proxy_frame({'method':'thread/started','params':{'thread':{'id':'c','unknown':'preserved'}}})
        server_side.sendall(started);check(proxy_read(client_side)==started)
        dynamic=proxy_frame({'id':19,'method':'turn/start','params':{'threadId':'c',
            'input':[{'type':'text','text':'새 세션 입력'}]}})
        client_side.sendall(dynamic);check_context_forward(proxy_read(server_side),dynamic)
        dynamic_reply=proxy_frame({'id':19,'result':{}})
        server_side.sendall(dynamic_reply);check(proxy_read(client_side)==dynamic_reply)
        # Leave one request pending in each direction, with the same numeric ID.
        pending_input=proxy_frame({'id':77,'method':'turn/start','params':{'threadId':'a','input':[]}})
        pending_approval=proxy_frame({'id':77,'method':'item/commandExecution/requestApproval',
                                      'params':{'threadId':'b','command':'never executed'}})
        client_side.sendall(pending_input);check_context_forward(proxy_read(server_side),pending_input,request)
        server_side.sendall(pending_approval);check(proxy_read(client_side)==pending_approval)
        dynamic_pending=proxy_frame({'id':78,'method':'turn/start','params':{'threadId':'c','input':[]}})
        client_side.sendall(dynamic_pending);check_context_forward(proxy_read(server_side),dynamic_pending,dynamic)
        # The actual backend-side socket obtains the first input address and
        # returns an observation bound to it, without an extra host write call.
        producer_started=proxy_frame({'method':'thread/started','params':{'thread':{'id':'producer-live'}}})
        server_side.sendall(producer_started);check(proxy_read(client_side)==producer_started)
        producer_request=proxy_frame({'id':79,'method':'turn/start','params':{'threadId':'producer-live',
            'input':[{'type':'text','text':'verify this recorded claim'}]}})
        client_side.sendall(producer_request)
        producer_reference=check_context_forward(proxy_read(server_side),producer_request)
        producer_response=proxy_frame({'id':79,'result':{'turn':{'id':'live-turn'}}})
        server_side.sendall(producer_response);check(proxy_read(client_side)==producer_response)
        producer_output=proxy_frame({'method':'item/completed','params':{'threadId':'producer-live','turnId':'live-turn',
            'item':{'type':'mcpToolCall','id':'live-tool','server':'fixture-producer','tool':'verify','status':'completed',
                'result':{'content':[],'structuredContent':{'swegcaObservation':{
                    'inputOriginal':producer_reference['inputOriginal'],'axis':'0','outcome':'support',
                    'confidence':1.0,'hasExpiry':False,'expiresAt':'0'}}}}}})
        server_side.sendall(producer_output);check(proxy_read(client_side)==producer_output)
        client_side.shutdown(socket.SHUT_WR);check(server_side.recv(1)==b'')
        server_side.shutdown(socket.SHUT_WR);check(client_side.recv(1)==b'')
        check(proxy_process.wait(timeout=10)==0)
        check(proxy_process.stdout.read()==b'' and proxy_process.stderr.read()==b'')
        check(vrs_process.wait(timeout=10)==0 and vrs_process.stderr.read()==b'')
    finally:
        client_side.close();server_side.close()
        for process in (proxy_process,vrs_process):
            if process.poll() is None:process.terminate();process.wait(timeout=10)
    c=Client('open',proxy_root,path);c.initialize()
    live_binding={'provider':'codex','instance':'proxy-tested','session':'producer-live','protocol':'app-server'}
    live=c.call('swegca/agent/attach/resume',live_binding)['result']
    check(live['nextSequence']=='4')
    live_original=c.call('swegca/agent/original',{'identity':live['identity'],'sequence':'3'})['result']
    check(live_original['native']==producer_output[:-1].decode())
    check(live_original['context']==producer_reference['inputOriginal']['digest'])
    check(c.call('swegca/select',{'identity':live['identity']})['result']=={})
    live_recalled=c.call('swegca/agent/event',{'sequence':'4','observedAt':'4','seed':'7','step':'0','sender':'client',
        'native':producer_request[:-1].decode()})['result']
    check(live_recalled['candidateCount']=='2')
    check(live_original['original'] in [x['original'] for x in live_recalled['candidates']])
    connection_binding={'provider':'codex','instance':'proxy-tested','session':'transport',
                        'protocol':'app-server-connection'}
    connection=c.call('swegca/agent/attach/resume',connection_binding)['result']
    check(connection['nextSequence']=='5')
    recovered=[]
    for sequence in range(5):
        item=c.call('swegca/agent/original',{'identity':connection['identity'],'sequence':str(sequence)})['result']
        check(item['source']=='codex/app-server-connection')
        check(json.loads(item['native'])==[
            global_frames[0],{'id':1,'result':{}},global_frames[1],
            global_frames[2],{'id':2,'result':{}}][sequence])
        check(item['native']==proxy_frame(json.loads(item['native']))[:-1].decode())
        check(item['sender']==('server' if sequence in (1,4) else 'client'))
        recovered.append(item)
    check(recovered[1]['context']==recovered[0]['original']['digest'])
    check(recovered[4]['context']==recovered[3]['original']['digest'])
    for bad in ({'identity':connection['identity'],'sequence':'5'},
                {'identity':identity(250),'sequence':'0'}, {'sequence':'0'}):
        check('error' in c.call('swegca/agent/original',bad))

    check('error' in c.call('swegca/agent/attach/ensure',dict(connection_binding,protocol='app-server')))
    params={'identity':connection['identity'],'sequence':'4','observedAt':'0','seed':'7','step':'0',
            'native':json.dumps({'id':2,'result':{}}),'requestSequence':'3'}
    # Recovered request validation must understand connection-owned originals.
    params['sequence']='5'
    check('error' in c.call('swegca/agent/event',params))
    check('error' in c.call('swegca/agent/event',dict(params,sender='client')))
    params['sender']='server'
    appended=c.call('swegca/agent/event',params)['result']
    check('original' in appended and 'refinement' in appended)
    check(c.call('swegca/agent/event',params)['result']['duplicate'] is True)
    bad=dict(params,sequence='6',native=json.dumps({'id':3,'method':'turn/start','params':{'input':[]}}))
    del bad['requestSequence']
    check('error' in c.call('swegca/agent/event',bad))
    for session,count in (('a','4'),('b','3'),('c','4')):
        attached=c.call('swegca/agent/attach/resume',{'provider':'codex','instance':'proxy-tested',
            'session':session,'protocol':'app-server'})['result']
        check(attached['nextSequence']==count)
        stored=c.call('swegca/agent/original',{'identity':attached['identity'],'sequence':'0'})['result']
        check(stored['sender']==('client' if session=='a' else 'server'))

        ensured=c.call('swegca/agent/attach/ensure',{'provider':'codex','instance':'proxy-tested',
            'session':session,'protocol':'app-server'})['result']
        check(ensured==attached)
        check('error' in c.call('swegca/agent/attach/ensure',{'provider':'codex','instance':'proxy-tested',
            'session':session,'protocol':'hook'}))
    check(c.call('swegca/work',{'seed':'7','step':'0'})['result']['merged']=='0')
    c.close()
    # The executable must not pass native content when its VRS endpoint is gone.
    resumed=json.loads(proxy_config.read_text())
    resumed['connectionSession']['mode']='resume'
    for entry in resumed['sessions']:entry['mode']='resume'
    proxy_config.write_text(json.dumps(resumed))
    client_side,client_proxy=socket.socketpair();server_side,server_proxy=socket.socketpair()
    vrs_side,vrs_proxy=socket.socketpair()
    vrs_process=subprocess.Popen([str(exe),'open',str(proxy_root),str(transport_path)],
        stdin=vrs_side,stdout=vrs_side,stderr=subprocess.PIPE)
    proxy_process=subprocess.Popen([str(exe.parent/'swegca-app-server-proxy'),
        str(client_proxy.fileno()),str(server_proxy.fileno()),str(vrs_proxy.fileno()),str(proxy_config),"--resume-peer"],
        pass_fds=(client_proxy.fileno(),server_proxy.fileno(),vrs_proxy.fileno()),
        stdout=subprocess.PIPE,stderr=subprocess.PIPE)
    for endpoint in (client_proxy,server_proxy,vrs_side,vrs_proxy):endpoint.close()
    server_side.settimeout(10)
    try:
        check(bool(select.select([proxy_process.stdout],[],[],10)[0]))
        check(proxy_process.stdout.readline()==b'ready\n')
        # The new proxy reconstructs both outstanding bindings before ready.
        client_side.settimeout(10)
        pending_reply=proxy_frame({'id':77,'result':{'recovered':True}})
        server_side.sendall(pending_reply);check(proxy_read(client_side)==pending_reply)
        client_side.sendall(pending_reply);check(proxy_read(server_side)==pending_reply)
        # Rediscover an already retained session from its real lifecycle notice.
        client_side.settimeout(10)
        server_side.sendall(started);check(proxy_read(client_side)==started)
        dynamic_recovered=proxy_frame({'id':78,'result':{'recovered':'dynamic'}})
        server_side.sendall(dynamic_recovered);check(proxy_read(client_side)==dynamic_recovered)
        vrs_process.terminate();vrs_process.wait(timeout=10)
        client_side.sendall(request)
        check(proxy_process.wait(timeout=10)==1)
        check(server_side.recv(1)==b'')
        check(b'VRS' in proxy_process.stderr.read())
    finally:
        client_side.close();server_side.close()
        for process in (proxy_process,vrs_process):
            if process.poll() is None:process.terminate();process.wait(timeout=10)
    c=Client('open',proxy_root,path);c.initialize()
    for session,request_seq,response_seq,count in (('a',3,4,5),('b',2,3,4)):
        attached=c.call('swegca/agent/attach/resume',{'provider':'codex','instance':'proxy-tested',
            'session':session,'protocol':'app-server'})['result']
        check(attached['nextSequence']==str(count))
        originals=[c.call('swegca/agent/original',{'identity':attached['identity'],'sequence':str(seq)})['result']
                   for seq in (request_seq,response_seq)]
        check(originals[1]['context']==originals[0]['original']['digest'])
        check(originals[0]['sender']!=originals[1]['sender'])
    closed_params={'provider':'codex','instance':'proxy-tested','session':'c','protocol':'app-server'}
    restored=c.call('swegca/agent/attach/ensure',closed_params)['result']
    check(restored['nextSequence']=='6')
    check(c.call('swegca/select',{'identity':restored['identity']})['result']=={})
    check(c.call('swegca/end')['result']=={})
    check('error' in c.call('swegca/agent/attach/ensure',closed_params))
    check('error' in c.call('swegca/agent/attach/ensure',closed_params))
    # Failed lifecycle reattachment must not leave an ended source leased.
    check(c.call('swegca/work',{'seed':'7','step':'0'})['result']['merged']=='1')
    c.close()
    wire_root=root/'wire-owner';wire_root.mkdir()
    c=Client('create',wire_root,path);c.initialize()
    wire_ids={}
    for session in ('a','b'):
        attached=c.call('swegca/agent/attach',{'provider':'codex','instance':'wire-tested',
            'session':session,'protocol':'app-server'})['result']
        check(attached['nextSequence']=='0');wire_ids[session]=attached['identity']
    owner=subprocess.Popen([str(exe.parent/'app-server-pump-tests'),'--exchange'],
        stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,bufsize=0)
    owner.stdin.write(json.dumps(wire_ids).encode()+b'\n');owner.stdin.flush()
    for index,session in enumerate(('a','b','a','b')):
        check(bool(select.select([owner.stdout],[],[],10)[0]))
        plan=json.loads(owner.stdout.readline());check(plan['session']==session)
        params=plan['parameters'];check(params['sequence']==('0' if index<2 else '1'))
        if index>=2:check(params['requestSequence']=='0')
        else:check('requestSequence' not in params)
        check(bool(select.select([owner.stdout],[],[],10)[0]))
        request=json.loads(owner.stdout.readline());check(request['method']=='swegca/agent/event')
        check(request['params']==dict(params,identity=wire_ids[session]))
        if index==0:
            # No standalone select RPC: invalid targets cannot record an event
            # in the previously selected or another attached session.
            for target in (identity(250),wire_ids['b'],'bad'):
                check('error' in c.call('swegca/agent/event',dict(params,identity=target)))
        response=c.raw(json.dumps(request).encode()+b'\n')
        check(response['id']==request['id'] and 'result' in response)
        if index==0:
            first=response
            response=c.raw(json.dumps(request).encode()+b'\n')
            check(response['result']['duplicate'] is True)
            check(response['id']==request['id'] and
                  response['result']['original']==first['result']['original'])
        owner.stdin.write(json.dumps(response).encode()+b'\n');owner.stdin.flush()
        check(bool(select.select([owner.stdout],[],[],10)[0]))
        forwarded=json.loads(owner.stdout.readline());check(forwarded['forwarded']==params['native'])
    owner.stdin.close();check(owner.wait(timeout=10)==0)
    check(owner.stdout.read()==b'' and owner.stderr.read()==b'')
    check(c.call('swegca/work',{'seed':'7','step':'0'})['result']['merged']=='0')
    c.close()
    c=Client('open',wire_root,path);c.initialize()
    for session in ('a','b'):
        attached=c.call('swegca/agent/attach/resume',{'provider':'codex','instance':'wire-tested',
            'session':session,'protocol':'app-server'})['result']
        check(attached['nextSequence']=='2')
        check(c.call('swegca/select',{'identity':attached['identity']})['result']=={})
        check(c.call('swegca/end')['result']=={})
    check(c.call('swegca/work',{'seed':'7','step':'0'})['result']['merged']=='2');c.close()
    for mode in ([],['--guess-peer']):
        missing_mode=subprocess.run([str(exe.parent/'swegca-app-server-proxy'),'3','4','5','/does/not/exist',*mode],
            capture_output=True,timeout=10)
        check(missing_mode.returncode==1 and missing_mode.stdout==b'')
        check(b'peer' in missing_mode.stderr)
    # Desktop-style stdio: launcher owns backend, proxy and real VRS processes.
    desktop_root=root/'desktop-host';desktop_root.mkdir()
    desktop_proxy=root/'desktop-proxy.json'
    desktop_proxy.write_text(json.dumps({'memoryBytes':str(8<<20),'frameBytes':'4096',
        'pendingRequests':'8','sessionCapacity':'2','seed':'7','step':'0','instance':'desktop-fixture',
        'connectionSession':{'session':'transport','mode':'ensure'},'sessions':[]}))
    captured_inputs=root/'backend-inputs.jsonl'
    backend_code="""import sys,json
for line in sys.stdin:
    value=json.loads(line)
    if value.get('method')=='thread/start':
        print(json.dumps({'method':'thread/started','params':{'thread':{'id':'desktop-thread'}}}),flush=True)
    if value.get('method')=='fixture/pending':
        print(json.dumps({'method':'fixture/received','params':{}}),flush=True)
        continue
    if 'id' in value:
        print(json.dumps({'id':value['id'],'result':{}}),flush=True)
"""
    backend_code=backend_code.replace("    value=json.loads(line)",
        "    value=json.loads(line)\n    with open("+repr(str(captured_inputs))+",'a') as capture:capture.write(line)")
    captured_queries=root/'backend-queries.jsonl'
    query_backend="""    if value.get('method') in ('turn/start','turn/steer'):
        context=json.loads(value['params']['input'][0]['text'].split('\\n',1)[1])
        if 'assessment' in context:
            import os,subprocess
            requests=[{'jsonrpc':'2.0','id':1,'method':'initialize','params':{'protocolVersion':'2025-06-18','capabilities':{},'clientInfo':{'name':'desktop-query-fixture','version':'1'}}},
                {'jsonrpc':'2.0','method':'notifications/initialized'},
                {'jsonrpc':'2.0','id':2,'method':'tools/call','params':{'name':'vrs_replay','arguments':{'receipt':context['receipt'],'inputOriginal':context['assessment']['inputOriginal']}}}]
            response=subprocess.run([OBSERVER,'16777216','625000000','1048576'],input=''.join(json.dumps(r)+'\\n' for r in requests),text=True,capture_output=True,timeout=10,check=True)
            with open(QUERY_FILE,'a') as output:output.write(json.dumps({'socket':os.environ['SWEGCA_QUERY_SOCKET'],'reply':json.loads(response.stdout.splitlines()[-1])})+'\\n')
"""
    query_backend=query_backend.replace('OBSERVER',repr(str(exe.parent/'swegca-content-observer'))).replace('QUERY_FILE',repr(str(captured_queries)))
    backend_code=backend_code.replace("    if 'id' in value:",query_backend+"    if 'id' in value:")
    desktop_backend=root/'fixture backend'
    desktop_backend.write_text('#!'+sys.executable+'\n'+backend_code);desktop_backend.chmod(0o700)
    wrapper_config=root/'wrapper.json'
    wrapper_config.write_text(json.dumps({'backend':str(desktop_backend),'host':str(exe.parent/'swegca-desktop-host'),
        'proxy':str(exe.parent/'swegca-app-server-proxy'),'vrs':str(exe),'mode':'ensure','root':str(desktop_root),
        'resourceConfig':str(transport_path),'proxyConfig':str(desktop_proxy)}))
    # The limited desktop owner and all three children share one real cgroup.
    aggregate_root=root/'aggregate-desktop';aggregate_root.mkdir()
    aggregate_info=root/'aggregate-pids.json'
    aggregate_backend=root/'aggregate backend'
    aggregate_backend.write_text('#!'+sys.executable+'\nimport os,json,sys\n'+
        'with open('+repr(str(aggregate_info))+',"w") as out:json.dump({"parent":os.getppid(),"pid":os.getpid(),"cwd":os.getcwd()},out)\n'+
        'for line in sys.stdin:\n value=json.loads(line)\n if "id" in value:print(json.dumps({"id":value["id"],"result":{}}),flush=True)\n')
    aggregate_backend.chmod(0o700)
    aggregate_config=dict(json.loads(wrapper_config.read_text()),mode='limited-ensure',
        root=str(aggregate_root),backend=str(aggregate_backend))
    aggregate_path=root/'aggregate-wrapper.json';aggregate_path.write_text(json.dumps(aggregate_config))
    aggregate=subprocess.Popen([str(exe.parent/'swegca-codex-wrapper'),'app-server'],
        env=dict(os.environ,SWEGCA_DESKTOP_CONFIG=str(aggregate_path)),cwd=root,
        stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,bufsize=0)
    try:
        aggregate.stdin.write(b'{"id":1,"method":"initialize","params":{}}\n')
        check(bool(select.select([aggregate.stdout],[],[],15)[0]))
        check(json.loads(aggregate.stdout.readline())=={'id':1,'result':{}})
        info=json.loads(aggregate_info.read_text());check(info['cwd']==str(root))
        owner=info['parent'];children=[int(pid) for pid in pathlib.Path(f'/proc/{owner}/task/{owner}/children').read_text().split()]
        check(len(children)==3 and info['pid'] in children)
        groups=[]
        for pid in [owner,*children]:
            group=next(line[3:] for line in pathlib.Path(f'/proc/{pid}/cgroup').read_text().splitlines() if line.startswith('0::/'))
            groups.append(group);check(os.sched_getaffinity(pid)=={6,7})
        check(len(set(groups))==1)
        group=pathlib.Path('/sys/fs/cgroup')/groups[0].lstrip('/')
        check(group.joinpath('memory.max').read_text().strip()==config['memoryBytes'])
        check(group.joinpath('memory.swap.max').read_text().strip()=='0')
        aggregate.stdin.close();check(aggregate.wait(timeout=15)==0)
        check(aggregate.stdout.read()==b'' and aggregate.stderr.read()==b'')
        for pid in [owner,*children]:check(not pathlib.Path(f'/proc/{pid}').exists())
    finally:
        if aggregate.poll() is None:aggregate.terminate();aggregate.wait(timeout=15)
    c=Client('open',aggregate_root,transport_path);c.initialize()
    check(c.call('swegca/work',{'seed':'7','step':'0'})['result']['merged']=='0');c.close()
    desktop=subprocess.Popen([str(exe.parent/'swegca-codex-wrapper'),'-c','features.code_mode_host=true',
        'app-server','--analytics-default-enabled'],env=dict(os.environ,SWEGCA_DESKTOP_CONFIG=str(wrapper_config)),
        stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,bufsize=0)
    desktop_input=[{'type':'text','text':'현재 사용자 입력을 그대로 보존\n한글🙂'},
                   {'type':'localImage','path':'/never/open/current.png'}]
    desktop_followup=[{'type':'text','text':'다른 문장으로 이어서 작업하자'},desktop_input[1]]
    def desktop_send(value):desktop.stdin.write(json.dumps(value).encode()+b'\n')
    def desktop_read():
        check(bool(select.select([desktop.stdout],[],[],10)[0]))
        return json.loads(desktop.stdout.readline())
    try:
        desktop_send({'id':901,'method':'initialize','params':{'clientInfo':{'name':'fixture'}}})
        check(desktop_read()=={'id':901,'result':{}})
        desktop_send({'method':'initialized'})
        desktop_send({'id':902,'method':'thread/start','params':{}})
        check(desktop_read()['method']=='thread/started')
        check(desktop_read()=={'id':902,'result':{}})
        desktop_send({'id':903,'method':'turn/start','params':{'threadId':'desktop-thread','input':desktop_input}})
        check(desktop_read()=={'id':903,'result':{}})
        desktop_send({'id':904,'method':'fixture/pending','params':{}})
        check(desktop_read()['method']=='fixture/received') # delivered but unanswered at shutdown
        desktop.stdin.close();check(desktop.wait(timeout=10)==0)
        check(desktop.stdout.read()==b'' and desktop.stderr.read()==b'')
    finally:
        if desktop.poll() is None:desktop.terminate();desktop.wait(timeout=10)
    # Relaunch using exactly the same wrapper/proxy settings and storage root.
    desktop=subprocess.Popen([str(exe.parent/'swegca-codex-wrapper'),'-c','features.code_mode_host=true',
        'app-server','--analytics-default-enabled'],env=dict(os.environ,SWEGCA_DESKTOP_CONFIG=str(wrapper_config)),
        stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,bufsize=0)
    try:
        desktop_send({'id':904,'method':'initialize','params':{}})
        check(desktop_read()=={'id':904,'result':{}})
        # Desktop may read a prior conversation before resuming it.
        desktop_send({'id':907,'method':'thread/read','params':{'threadId':'desktop-thread'}})
        check(desktop_read()=={'id':907,'result':{}})
        # Resume an existing conversation with no thread/started notification.
        desktop_send({'id':905,'method':'thread/resume','params':{'threadId':'desktop-thread'}})
        check(desktop_read()=={'id':905,'result':{}})
        desktop_send({'id':906,'method':'turn/start','params':{'threadId':'desktop-thread','input':desktop_followup}})
        check(desktop_read()=={'id':906,'result':{}})

        desktop.stdin.close();check(desktop.wait(timeout=10)==0)
        check(desktop.stdout.read()==b'' and desktop.stderr.read()==b'')
    finally:
        if desktop.poll() is None:desktop.terminate();desktop.wait(timeout=10)
    received_inputs=[json.loads(line) for line in captured_inputs.read_text().splitlines()]
    initial_input=next(value for value in received_inputs if value.get('id')==903)
    resumed_input=next(value for value in received_inputs if value.get('id')==906)
    check(initial_input['params']['input'][1:]==desktop_input)
    first_input_reference=json.loads(initial_input['params']['input'][0]['text'].split('\n',1)[1])
    check(first_input_reference['recalledOriginal'] is None and not first_input_reference['grantsAuthority'])
    first_candidates=first_input_reference['inputCandidates']
    check(first_candidates['inputOriginal']==first_input_reference['inputOriginal'])
    check(not first_candidates['semanticVerified'] and not first_candidates['requirementsComplete'])
    for candidate in first_candidates['candidates']:
        candidate_text=desktop_input[int(candidate['textIndex'])]['text'].encode()
        candidate_offset=int(candidate['byteOffset']);candidate_quote=candidate['quote'].encode()
        check(candidate_text[candidate_offset:candidate_offset+len(candidate_quote)]==candidate_quote)

    check(len(resumed_input['params']['input'])==len(desktop_followup)+1)
    check(resumed_input['params']['input'][1:]==desktop_followup)
    injected=resumed_input['params']['input'][0]
    check(injected['type']=='text' and injected['text'].startswith('SWEGCA recalled experience'))
    memory_packet=json.loads(injected['text'].split('\n',1)[1])
    check(memory_packet['grantsAuthority'] is False and memory_packet['assessment']['status']==0)
    native_initial=dict(initial_input,params=dict(initial_input['params'],input=desktop_input))
    check(json.loads(memory_packet['content'])==native_initial)
    check(memory_packet['original']==first_input_reference['inputOriginal'])
    check(memory_packet['inputCandidates']['inputOriginal']==memory_packet['assessment']['inputOriginal'])
    check(memory_packet['inputCandidates']['candidates'])

    backend_queries=[json.loads(line) for line in captured_queries.read_text().splitlines()]
    check(len(backend_queries)==1)
    check(backend_queries[0]['reply']['result']['structuredContent']['original']==memory_packet['original'])
    check(backend_queries[0]['reply']['result']['structuredContent']['assessment']['inputOriginal']==memory_packet['assessment']['inputOriginal'])
    check(not pathlib.Path(backend_queries[0]['socket']).exists())
    check(not pathlib.Path(backend_queries[0]['socket']).parent.exists())
    check(int(first_input_reference['receipt'])>0 and int(memory_packet['receipt'])>0)
    # A real proxy must fetch the sealed observation before the backend sees
    # this input. No related tool call is made by the synthetic backend.
    auto_proxy=root/'observation-proxy.json'
    auto_proxy.write_text(json.dumps({**json.loads(desktop_proxy.read_text()),'frameBytes':'16384',
        'instance':'observation-desktop'}))
    auto_resources=root/'observation-resources.json'
    auto_resources.write_text(json.dumps({**json.loads(transport_path.read_text()),'frameBytes':str(16384*6+65536)}))
    auto_wrapper=root/'observation-wrapper.json'
    auto_wrapper.write_text(json.dumps({**json.loads(wrapper_config.read_text()),'root':str(bound_root),
        'resourceConfig':str(auto_resources),'proxyConfig':str(auto_proxy)}))
    desktop=subprocess.Popen([str(exe.parent/'swegca-codex-wrapper'),'app-server'],
        env=dict(os.environ,SWEGCA_DESKTOP_CONFIG=str(auto_wrapper)),stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,stderr=subprocess.PIPE,bufsize=0)
    try:
        desktop_send({'id':930,'method':'initialize','params':{}})
        check(desktop_read()=={'id':930,'result':{}})
        desktop_send({'id':931,'method':'thread/start','params':{}})
        check(desktop_read()['method']=='thread/started')
        check(desktop_read()=={'id':931,'result':{}})
        desktop_send({'id':932,'method':'turn/start','params':{'threadId':'desktop-thread',
            'input':[{'type':'text','text':'A purpose kept in Main.'}]}})
        check(desktop_read()=={'id':932,'result':{}})
        desktop.stdin.close();check(desktop.wait(timeout=10)==0)
        check(desktop.stdout.read()==b'' and desktop.stderr.read()==b'')
    finally:
        if desktop.poll() is None:desktop.terminate();desktop.wait(timeout=10)
    forwarded=next(json.loads(line) for line in captured_inputs.read_text().splitlines() if json.loads(line).get('id')==932)
    injected_observation=json.loads(forwarded['params']['input'][0]['text'].split('\n',1)[1])
    linked=injected_observation['relatedExperiences'][0]
    check(not injected_observation['relatedCoverage']['limited'])
    check(injected_observation['original']==bound_input['original'])
    check(linked['original']==bound_report['original'] and linked['relatedFrom']==injected_observation['original'])
    check(linked['assessment']['inputOriginal']==injected_observation['assessment']['inputOriginal'])
    check(not linked['grantsAuthority'] and linked['parentCognitionUnchanged'])
    observed=json.loads(linked['content'])['params']['item']['result']['structuredContent']['swegcaObservation']
    check(observed['outcome']=='refute' and observed['scope']=='archived requirement outcome')
    check(forwarded['params']['input'][1:]==[{'type':'text','text':'A purpose kept in Main.'}])
    for run,limit,byte_budget,expected_count in ((0,8,65536,3),(1,1,65536,1),(2,8,0,0),(3,8,65536,3),(4,8,0,0)):
        selected_prompt=coverage_prompt if run>=3 else 'Keep A, change B, verify C.'
        selected_root=coverage_root if run>=3 else pages_root
        auto_proxy.write_text(json.dumps({**json.loads(desktop_proxy.read_text()),'frameBytes':'16384',
            'instance':'multi-observation-'+str(run),'relatedConnections':str(limit),'relatedContextBytes':str(byte_budget)}))
        auto_wrapper.write_text(json.dumps({**json.loads(wrapper_config.read_text()),'root':str(selected_root),
            'resourceConfig':str(auto_resources),'proxyConfig':str(auto_proxy)}))
        desktop=subprocess.Popen([str(exe.parent/'swegca-codex-wrapper'),'app-server'],
            env=dict(os.environ,SWEGCA_DESKTOP_CONFIG=str(auto_wrapper)),stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,stderr=subprocess.PIPE,bufsize=0)
        rpc=940+run*10
        try:
            desktop_send({'id':rpc,'method':'initialize','params':{}});check(desktop_read()=={'id':rpc,'result':{}})
            desktop_send({'id':rpc+1,'method':'thread/start','params':{}})
            check(desktop_read()['method']=='thread/started');check(desktop_read()=={'id':rpc+1,'result':{}})
            desktop_send({'id':rpc+2,'method':'turn/start','params':{'threadId':'desktop-thread',
                'input':[{'type':'text','text':selected_prompt}]}})
            check(desktop_read()=={'id':rpc+2,'result':{}})
            desktop.stdin.close();check(desktop.wait(timeout=10)==0)
            check(desktop.stdout.read()==b'' and desktop.stderr.read()==b'')
        finally:
            if desktop.poll() is None:desktop.terminate();desktop.wait(timeout=10)
        forwarded=next(json.loads(line) for line in captured_inputs.read_text().splitlines() if json.loads(line).get('id')==rpc+2)
        bundle=json.loads(forwarded['params']['input'][0]['text'].split('\n',1)[1])
        check(forwarded['params']['input'][1:]==[{'type':'text','text':selected_prompt}])
        check(bundle['original']==(coverage_parent['original'] if run>=3 else page_input['original']))
        check(len(bundle['relatedExperiences'])==expected_count)
        coverage=bundle['relatedCoverage'];check(coverage['limited']==(run in (1,2,4)))
        check(not coverage['mixedTiers'] and all(not x['temporary'] for x in coverage['connections']))
        check(not coverage['requirementsComplete'] and not coverage['grantsAuthority'])
        check(len(coverage['deliveredConnections'])==expected_count)
        check(len(coverage['deliveredExperiences'])==expected_count)
        check([x['original'] for x in coverage['deliveredExperiences']]==[x['original'] for x in bundle['relatedExperiences']])
        check(all(x['matchesListedOriginal'] for x in coverage['deliveredExperiences']))
        check((coverage['next'] is not None)==(run==1))
        if run in (0,3):
            check({x['original']['digest'] for x in bundle['relatedExperiences']}=={x['digest'] for x in (coverage_expected if run==3 else multi_expected)})
            check({json.loads(x['content'])['params']['item']['result']['structuredContent']['swegcaObservation']['outcome']
                for x in bundle['relatedExperiences']}=={'support','refute','insufficient'})
        recalled_coverage=bundle['recalledRequirementCoverage']
        check(recalled_coverage['inputOriginal']==bundle['original'])
        check(recalled_coverage['inputOriginal']!=bundle['inputCandidates']['inputOriginal'])
        check(not recalled_coverage['semanticVerified'] and not recalled_coverage['requirementsComplete'])
        check(not recalled_coverage['grantsAuthority'] and not recalled_coverage['byteLimited'])
        if run>=3:
            rows=recalled_coverage['candidates']
            check(len(rows)==4 and ''.join(row['requirement']['quote'] for row in rows)==coverage_prompt)
            check(rows[3]['relatedExperienceIndices']==[])
            check(recalled_coverage['unboundObservationIndices']==[])
            for row in rows[:3]:
                indices=row['relatedExperienceIndices']
                check(len(indices)==(1 if run==3 else 0))
                for index in indices:
                    observation=json.loads(bundle['relatedExperiences'][int(index)]['content'])['params']['item']['result']['structuredContent']['swegcaObservation']
                    check(observation['inputOriginal']==coverage_parent['original'])
                    check(row['requirement']['quote'].strip()==observation['requirement']['quote'])
        else:
            check(all(row['relatedExperienceIndices']==[] for row in recalled_coverage['candidates']))
            check(len(recalled_coverage['unboundObservationIndices'])==expected_count)
    # Actual desktop transport: a successful turn/start response establishes
    # the exact predecessor; steer keeps its raw text and unverified candidates.
    steer_capture=root/'steer-backend-inputs.jsonl'
    steer_backend=root/'steer-backend'
    steer_backend.write_text('#!'+sys.executable+'\n'+
        'import json,sys\n'+
        'capture='+repr(str(steer_capture))+'\n'+
        'for line in sys.stdin:\n'+
        ' v=json.loads(line)\n'+
        ' with open(capture,"a") as out:out.write(json.dumps(v)+"\\n")\n'+
        ' method=v.get("method")\n'+
        ' if method=="thread/start":print(json.dumps({"method":"thread/started","params":{"thread":{"id":"steer-desktop"}}}),flush=True)\n'+
        ' result={"turn":{"id":"turn-original"}} if method=="turn/start" else {"turnId":"turn-original"} if method=="turn/steer" else {}\n'+
        ' if "id" in v:print(json.dumps({"id":v["id"],"result":result}),flush=True)\n')
    steer_backend.chmod(0o700)
    auto_proxy.write_text(json.dumps({**json.loads(desktop_proxy.read_text()),'frameBytes':'16384','instance':'steer-desktop'}))
    auto_wrapper.write_text(json.dumps({**json.loads(wrapper_config.read_text()),'backend':str(steer_backend),
        'root':str(pages_root),'resourceConfig':str(auto_resources),'proxyConfig':str(auto_proxy)}))
    desktop=subprocess.Popen([str(exe.parent/'swegca-codex-wrapper'),'app-server'],
        env=dict(os.environ,SWEGCA_DESKTOP_CONFIG=str(auto_wrapper)),stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,stderr=subprocess.PIPE,bufsize=0)
    original_terms=[{'type':'text','text':'팰월드는 유지. ComfyUI만 중지'}]
    corrected_terms=[{'type':'text','text':'정정: “중지” → “유지”\n다른 조건은 그대로.'}]
    try:
        desktop_send({'id':980,'method':'initialize','params':{}});check(desktop_read()=={'id':980,'result':{}})
        desktop_send({'id':981,'method':'thread/start','params':{}})
        check(desktop_read()['method']=='thread/started');check(desktop_read()=={'id':981,'result':{}})
        desktop_send({'id':982,'method':'turn/start','params':{'threadId':'steer-desktop','input':original_terms}})
        check(desktop_read()=={'id':982,'result':{'turn':{'id':'turn-original'}}})
        desktop_send({'id':983,'method':'turn/steer','params':{'threadId':'steer-desktop',
            'expectedTurnId':'turn-original','input':corrected_terms}})
        check(desktop_read()=={'id':983,'result':{'turnId':'turn-original'}})
        desktop_send({'id':984,'method':'turn/steer','params':{'threadId':'steer-desktop',
            'expectedTurnId':'turn-original','input':[{'type':'text','text':'정정: “중지” → “유지”\n다른 조건은 그대로.'}]}})
        check(desktop_read()=={'id':984,'result':{'turnId':'turn-original'}})

        desktop.stdin.close();check(desktop.wait(timeout=10)==0)
        check(desktop.stdout.read()==b'' and desktop.stderr.read()==b'')
    finally:
        if desktop.poll() is None:desktop.terminate();desktop.wait(timeout=10)
    captured=[json.loads(x) for x in steer_capture.read_text().splitlines()]
    first_native=next(x for x in captured if x.get('id')==982)
    corrected_native=next(x for x in captured if x.get('id')==983)
    first_packet=json.loads(first_native['params']['input'][0]['text'].split('\n',1)[1])
    corrected_packet=json.loads(corrected_native['params']['input'][0]['text'].split('\n',1)[1])
    check(first_native['params']['input'][1:]==original_terms)
    check([x['quote'] for x in first_packet['inputCandidates']['candidates']]==['팰월드는 유지. ','ComfyUI만 중지'])
    check(not first_packet['inputCandidates']['boundariesVerified'])
    check(corrected_native['params']['input'][1:]==corrected_terms)
    check(corrected_packet['inputRelations']['priorInput']==first_packet['inputCandidates']['inputOriginal'])
    check(not corrected_packet['inputRelations']['replacementVerified'])
    corrected_ref=corrected_packet['inputRelations']['revisionReferences'][0]
    check(corrected_ref['locationStatus']=='unique')
    check(corrected_ref['priorAnchor']=={'textIndex':'0','byteOffset':str(original_terms[0]['text'].encode().index('중지'.encode())),'quote':'중지'})
    check(not corrected_ref['antecedentVerified'] and not corrected_ref['replacementVerified'])
    check(''.join(x['quote'] for x in corrected_packet['inputCandidates']['candidates'])==corrected_terms[0]['text'])
    explicit_native=next(x for x in captured if x.get('id')==984)
    explicit_packet=json.loads(explicit_native['params']['input'][0]['text'].split('\n',1)[1])
    proposals=explicit_packet['inputCandidates']['revisionProposals']
    check(len(proposals)==1 and not proposals[0]['antecedentVerified'] and not proposals[0]['replacementVerified'])
    check(explicit_packet['inputRelations']['priorInput'] is None)
    check(explicit_packet['inputRelations']['revisionReferences'][0]['locationStatus']=='unresolved-input')
    explicit_text=explicit_native['params']['input'][1]['text'].encode()
    for field in ('priorQuote','replacementQuote'):
        anchor=proposals[0][field];proposal_offset=int(anchor['byteOffset']);proposal_quote=anchor['quote'].encode()
        check(explicit_text[proposal_offset:proposal_offset+len(proposal_quote)]==proposal_quote)

    c=Client('open',desktop_root,path);c.initialize()
    for session,protocol,count in (('transport','app-server-connection','9'),('desktop-thread','app-server','9')):
        attached=c.call('swegca/agent/attach/resume',{'provider':'codex','instance':'desktop-fixture',
            'session':session,'protocol':protocol})['result']
        check(attached['nextSequence']==count)
        if session=='transport':
            old=c.call('swegca/agent/original',{'identity':attached['identity'],'sequence':'5'})['result']
            new=c.call('swegca/agent/original',{'identity':attached['identity'],'sequence':'7'})['result']
            response=c.call('swegca/agent/original',{'identity':attached['identity'],'sequence':'8'})['result']
            check(json.loads(old['native'])['id']==json.loads(new['native'])['id']==904)
            check(json.loads(old['native'])['method']=='fixture/pending')
            check(response['context']==new['original']['digest'] and response['context']!=old['original']['digest'])
    stored_input=c.call('swegca/agent/original',{'identity':attached['identity'],'sequence':'7'})['result']
    check(json.loads(stored_input['native'])['params']['input']==desktop_followup)
    check(stored_input['original']==memory_packet['assessment']['inputOriginal'])
    check(c.call('swegca/work',{'seed':'7','step':'0'})['result']['merged']=='0');c.close()
    # Interrupt only this test's launcher: all three owned children must exit.
    interrupt_root=root/'desktop-interrupt';interrupt_root.mkdir()
    desktop=subprocess.Popen([str(exe.parent/'swegca-desktop-host'),
        str(exe.parent/'swegca-app-server-proxy'),str(exe),'create',str(interrupt_root),str(transport_path),
        str(desktop_proxy),sys.executable,'-u','-c',backend_code],
        stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,bufsize=0)
    try:
        desktop_send({'id':901,'method':'initialize','params':{}})
        check(desktop_read()=={'id':901,'result':{}})
        children=pathlib.Path(f'/proc/{desktop.pid}/task/{desktop.pid}/children').read_text().split()
        check(len(children)==3)
        desktop.terminate();check(desktop.wait(timeout=10)==1)
        check(all(not pathlib.Path(f'/proc/{pid}').exists() for pid in children))
        check(b'interrupted' in desktop.stderr.read())
    finally:
        if desktop.poll() is None:desktop.kill();desktop.wait(timeout=10)
        desktop.stdin.close();desktop.stdout.close();desktop.stderr.close()
    # An idle owner must notice backend failure even if its launcher inherited
    # a blocked SIGCHLD mask. It cleans up only its three owned children.
    import signal
    failed_desktop_root=root/'desktop-child-failure';failed_desktop_root.mkdir()
    failing_backend="import sys,json,os\nfor line in sys.stdin:\n v=json.loads(line)\n if v.get('method')=='fixture/fail':os._exit(7)\n if 'id' in v:print(json.dumps({'id':v['id'],'result':{}}),flush=True)\n"
    def block_child_signal():signal.pthread_sigmask(signal.SIG_BLOCK,{signal.SIGCHLD})
    desktop=subprocess.Popen([str(exe.parent/'swegca-desktop-host'),str(exe.parent/'swegca-app-server-proxy'),
        str(exe),'create',str(failed_desktop_root),str(transport_path),str(desktop_proxy),sys.executable,'-u','-c',failing_backend],
        stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,bufsize=0,preexec_fn=block_child_signal)
    try:
        desktop_send({'id':1,'method':'initialize','params':{}});check(desktop_read()=={'id':1,'result':{}})
        children=pathlib.Path(f'/proc/{desktop.pid}/task/{desktop.pid}/children').read_text().split()
        check(len(children)==3)
        desktop_send({'method':'fixture/fail','params':{}})
        check(desktop.wait(timeout=10)==1)
        check(all(not pathlib.Path(f'/proc/{pid}').exists() for pid in children))
        check(b'child failed' in desktop.stderr.read())
    finally:
        if desktop.poll() is None:desktop.kill();desktop.wait(timeout=10)
        desktop.stdin.close();desktop.stdout.close();desktop.stderr.close()
    # A newly spawned backend owns a fresh RPC ID space while VRS experience survives.
    collision_root=root/'resume-id-conflict';collision_root.mkdir()
    c=Client('create',collision_root,path);c.initialize()
    collision_binding={'provider':'codex','instance':'collision-fixture','session':'prior','protocol':'app-server'}
    attached=c.call('swegca/agent/attach',collision_binding)['result']
    pending_native=json.dumps({'id':88,'method':'turn/start','params':{'threadId':'prior','input':[]}})
    pending_saved=c.call('swegca/agent/event',{'identity':attached['identity'],'sequence':'0','observedAt':'1',
        'seed':'7','step':'0','sender':'client','native':pending_native})['result']['original']
    c.close()
    collision_config=root/'collision-proxy.json'
    collision_config.write_text(json.dumps({'memoryBytes':str(8<<20),'frameBytes':'4096','pendingRequests':'8',
        'sessionCapacity':'1','seed':'7','step':'0','instance':'collision-fixture','sessions':[]}))
    collision=subprocess.run([str(exe.parent/'swegca-desktop-host'),str(exe.parent/'swegca-app-server-proxy'),
        str(exe),'open',str(collision_root),str(transport_path),str(collision_config),sys.executable,'-u','-c',backend_code],
        input=(json.dumps({'id':88,'method':'thread/resume','params':{'threadId':'prior'}})+'\n').encode(),
        capture_output=True,timeout=10)
    check(collision.returncode==0 and json.loads(collision.stdout)=={'id':88,'result':{}})
    check(collision.stderr==b'')
    c=Client('open',collision_root,path);c.initialize()
    restored=c.call('swegca/agent/attach/resume',collision_binding)['result']
    check(restored['nextSequence']=='3')
    fresh_request=c.call('swegca/agent/original',{'identity':restored['identity'],'sequence':'1'})['result']
    fresh_response=c.call('swegca/agent/original',{'identity':restored['identity'],'sequence':'2'})['result']
    check(fresh_response['context']==fresh_request['original']['digest'] and fresh_response['context']!=pending_saved['digest'])
    original=c.call('swegca/agent/original',{'identity':restored['identity'],'sequence':'0'})['result']
    check(original['original']==pending_saved and original['native']==pending_native)
    check(c.call('swegca/work',{'seed':'7','step':'0'})['result']['merged']=='0');c.close()
    # An insufficient host frame limit is rejected before any native forwarding.
    mismatch_root=root/'rpc-frame-mismatch';mismatch_root.mkdir()
    mismatch=subprocess.run([str(exe.parent/'swegca-desktop-host'),str(exe.parent/'swegca-app-server-proxy'),
        str(exe),'create',str(mismatch_root),str(path),str(desktop_proxy),sys.executable,'-u','-c',backend_code],
        input=(json.dumps({'id':1,'method':'initialize','params':{}})+'\n').encode(),
        capture_output=True,timeout=10)
    check(mismatch.returncode==1 and mismatch.stdout==b'')
    check(b'host frame budget smaller' in mismatch.stderr)
    check(not (mismatch_root/'sessions').exists())
    # Large selected Replay under a budget that cannot hold original + full hex.
    replay_root=root/'streamed-replay';replay_root.mkdir()
    replay_config=root/'streamed-replay.json'
    replay_settings=dict(config,frameBytes=str(2<<20),readLimit=str(2<<20),sessionBlockBytes=str(2<<20))
    replay_config.write_text(json.dumps(replay_settings))
    c=Client('create',replay_root,replay_config);c.initialize()
    replay_binding={'provider':'codex','instance':'streamed-fixture','session':'large-original'}
    attached=c.call('swegca/agent/attach',replay_binding)['result']
    large_native=json.dumps({'session_id':'large-original','hook_event_name':'UserPromptSubmit',
                             'prompt':'small cue','padding':'x'*(768<<10)})
    saved=c.call('swegca/agent/event',{'identity':attached['identity'],'sequence':'0','observedAt':'0',
        'seed':'7','step':'0','native':large_native})['result']['original'];c.close()
    replay_settings['memoryBytes']=str(2<<20);replay_config.write_text(json.dumps(replay_settings))
    check(3*len(large_native.encode())>int(replay_settings['memoryBytes']))
    c=Client('open',replay_root,replay_config);c.initialize()
    attached=c.call('swegca/agent/attach/resume',replay_binding)['result']
    recalled=c.call('swegca/agent/event',{'identity':attached['identity'],'sequence':'1','observedAt':'1',
        'seed':'7','step':'1','native':json.dumps({'session_id':'large-original',
            'hook_event_name':'UserPromptSubmit','prompt':'small cue'})})['result']
    check(recalled['candidateCount']=='1')
    played=c.call('tools/call',{'name':'vrs_replay','arguments':{'receipt':recalled['receipt'],'candidate':'0'}})['result']
    check(played['structuredContent']['original']==saved)
    check(bytes.fromhex(played['structuredContent']['contentHex'])==large_native.encode())
    verified=c.call('tools/call',{'name':'vrs_re_evidence','arguments':{'receipt':recalled['receipt'],'seed':'7','step':'1'}})['result']
    check('structuredContent' in verified)
    c.close()
    # Export the already authenticated automatic Replay without another read.
    # Only this disposable fixture's blocks are damaged; explicit fresh Replay
    # must still detect the corruption instead of treating the cache as storage.
    with tempfile.TemporaryDirectory(prefix='swegca-context-export-') as cached_directory:
        cached_root=pathlib.Path(cached_directory)
        c=Client('create',cached_root,path);c.initialize()
        check(c.call('swegca/start',{'identity':identity(201),'name':'context-export'})['result']=={})
        saved=c.call('swegca/receive',event('context-export'))['result']
        current=c.call('swegca/receive',event('context-export',sequence='1'))['result']
        request={'name':'vrs_replay','arguments':{'receipt':current['receipt']}}
        packet=c.call('tools/call',request)['result']['structuredContent']
        check(packet['assessment']['inputOriginal']==current['original'])
        blocks=list(cached_root.rglob('*.block'));check(bool(blocks))
        for block in blocks:
            with block.open('r+b') as stream:stream.truncate(0)
        exported=c.call('tools/call',request)['result']['structuredContent']
        check(exported==packet and exported['original']==saved['original'])
        check(bytes.fromhex(exported['contentHex'])==text.encode())
        check(c.call('tools/call',{'name':'vrs_replay','arguments':{
            'receipt':current['receipt'],'candidate':'0'}})['result']['isError'])
        c.close()
    # Idle memory management works independently of automatic Main merging.
    pressure_root=root/'idle-pressure';pressure_root.mkdir()
    pressure_config=root/'idle-pressure.json'
    pressure_config.write_text(json.dumps(dict(config,memoryTargetBytes='1')))
    c=Client('create',pressure_root,pressure_config);c.initialize()
    check(c.call('swegca/start',{'identity':identity(221),'name':'pressure'})['result']=={})
    expected=None
    for n in range(31):
        result=c.call('swegca/retain',event('pressure',sequence=str(n)))['result']
        if n==15:expected=result['original']
    check(c.call('swegca/end')['result']=={})
    check(c.call('swegca/work',{'seed':'7','step':'0'})['result']['merged']=='1')
    deadline=time.monotonic()+5
    while time.monotonic()<deadline and not list((pressure_root/'metadata-pages').glob('*.block')):
        check(c.p.poll() is None);time.sleep(.01)
    check(bool(list((pressure_root/'metadata-pages').glob('*.block'))))
    check(c.call('swegca/start',{'identity':identity(222),'name':'pressure-query'})['result']=={})
    recall=c.call('swegca/receive',event('pressure-query'))['result']
    replay=c.call('tools/call',{'name':'vrs_replay','arguments':{'receipt':recall['receipt'],'candidate':'15'}})['result']['structuredContent']
    check(replay['original']==expected)
    c.close()
    check(not list((pressure_root/'metadata-pages').glob('*.block')))
    # A full storage budget may stop optional paging, but must not stop the host.
    seen=set();pressure_bytes=0
    for entry in pressure_root.rglob('*'):
        if entry.is_file():
            stat=entry.stat();key=(stat.st_dev,stat.st_ino)
            if key not in seen:pressure_bytes+=stat.st_size;seen.add(key)
    pressure_config.write_text(json.dumps(dict(config,memoryTargetBytes='1',storageBytes=str(pressure_bytes))))
    c=Client('open',pressure_root,pressure_config);c.initialize()
    time.sleep(.1)
    check(c.call('ping')['result']=={})
    check(not list((pressure_root/'metadata-pages').glob('*.block')))
    c.close()
    # A separate config file must not change the root being measured.
    def stored_bytes():
        seen=set();total=0
        for entry in root.rglob('*'):
            if not entry.is_file():continue
            st=entry.stat();key=(st.st_dev,st.st_ino)
            if key not in seen:total+=st.st_size;seen.add(key)
        return total
    with tempfile.TemporaryDirectory(prefix='swegca-quota-config-') as external:
        quota=dict(config);quota['storageBytes']=str(stored_bytes())
        quota_path=pathlib.Path(external)/'config.json';quota_path.write_text(json.dumps(quota))
        before=stored_bytes()
        c=Client('open',root,quota_path);c.initialize()
        rejected=c.call('swegca/start',{'identity':identity(100),'name':'quota-denied'})
        check('error' in rejected)
        check(stored_bytes()==before)
        c.close()
print(f'stdio subprocess tests: {checks} checks passed')
