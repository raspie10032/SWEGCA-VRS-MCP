#!/usr/bin/env python3
"""Real subprocess transport/lifecycle checks; no client app or service is changed."""
import json, os, pathlib, select, subprocess, sys, tempfile, time
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
    def __init__(self,mode,root,config):
        self.p=subprocess.Popen([str(exe),mode_prefix+mode,str(root),str(config)],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
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
        return reply
    def notice(self,method,params=None):
        self.p.stdin.write(json.dumps({'jsonrpc':'2.0','method':method,'params':params or {}}).encode()+b'\n');self.p.stdin.flush()
    def initialize(self):
        check('error' in self.call('tools/list'))
        initialized=self.call('initialize',{'protocolVersion':'2025-06-18','capabilities':{},'clientInfo':{'name':'test','version':'1'}})['result']
        check(initialized['protocolVersion']=='2025-06-18')
        check(initialized['capabilities']['experimental']['swegcaHostInput']['version']=='9')
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
    second=c.call('swegca/receive',event('one','assistant',sequence='1'))['result'];check(len(second['candidates'])==1)
    check(c.call('tools/call',{'name':'vrs_replay','arguments':{'receipt':first['receipt'],'candidate':'0'}})['result']['isError'])
    replay=c.call('tools/call',{'name':'vrs_replay','arguments':{'receipt':second['receipt'],'candidate':'0'}})['result']
    check(bytes.fromhex(replay['structuredContent']['contentHex'])==text.encode())
    check(replay['structuredContent']['original']==first['original'])
    check(c.call('tools/call',{'name':'vrs_re_evidence','arguments':{'receipt':second['receipt'],'seed':'7','step':'0'}})['result']['structuredContent']['status']==0)
    # Partial Replay retains only the requested raw bytes and cannot reuse an
    # earlier full Replay receipt for Re-evidence, even on the same candidate.
    def partial(receipt, offset, count):
        return c.call('tools/call',{'name':'vrs_replay','arguments':{
            'receipt':receipt,'candidate':'0','offset':str(offset),'count':str(count)}})['result']
    part=partial(second['receipt'],2,5)['structuredContent']
    check(part['partial'] is True and part['offset']=='2' and part['totalBytes']==str(len(text.encode())))
    check(bytes.fromhex(part['contentHex'])==text.encode()[2:7] and part['original']==first['original'])
    check(c.call('tools/call',{'name':'vrs_re_evidence','arguments':{'receipt':second['receipt'],'seed':'7','step':'0'}})['result']['isError'])
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
    # Recorded observations go through the same server/runtime/core route.
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
    check(c.call('tools/call',{'name':'vrs_replay','arguments':{'receipt':receipt,'candidate':'0'}})['result']['structuredContent']['original']==support_original)
    current=[]
    for n in range(8,16):
        result=c.call('swegca/observe',observation(n,20,'refute'))['result'];current.append(result['original'])
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
    deadline=time.monotonic()+10
    while True:
        state=c.call('swegca/work/poll')['result']
        if not state['running']:
            check(state['merged']=='1');break
        check(state['merged'] is None and time.monotonic()<deadline)
    recalled=c.call('swegca/receive',event('async-active',sequence='1'))['result']
    check(not recalled['temporary'] and recalled['candidateCount']=='1')
    replay=c.call('tools/call',{'name':'vrs_replay','arguments':{'receipt':recalled['receipt'],'candidate':'0'}})['result']['structuredContent']
    check(replay['original']==original and replay['contentHex']==text.encode().hex())
    check(c.call('swegca/work/poll')['result']=={'running':False,'merged':'0'})
    check(c.call('swegca/end')['result']=={});c.close()
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
    # Native envelope preserves all fields while the prompt alone keys Recall.
    native_root=root/'native';native_root.mkdir()
    c=Client('create',native_root,path);c.initialize()
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
    # A lost response may be retried with the same event; no new refinement.
    same=resend_native(raw1,1)['result']
    check(same=={'duplicate':True,'original':n1['original'],'receipt':n1['receipt']})
    check(resend_native(raw0,0)['result']=={'duplicate':True,'original':n0['original'],'receipt':None})
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
    c.close()
    c=Client('open',native_root,path);c.initialize()
    check(c.call('swegca/agent/attach/resume',binding)['result']['identity']==native_id)
    check(c.call('swegca/select',{'identity':native_id})['result']=={})
    # No process-local delivery cache survives this restart. The committed
    # originals alone identify retries without adding observations or strength.
    check(resend_native(raw0,0)['result']=={'duplicate':True,'original':n0['original'],'receipt':None})
    check(resend_native(raw1,1)['result']=={'duplicate':True,'original':n1['original'],'receipt':None})
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
    _,main_native=native_event('UserPromptSubmit',0,prompt=text)
    main_native=main_native['result']
    check(not main_native['temporary'] and main_native['candidateCount']=='3')
    check(bytes.fromhex(replay_receipt(main_native['receipt'])['structuredContent']['contentHex'])==raw0.encode())
    check(c.call('swegca/work',{'seed':'7','step':'0'})['result']['merged']=='0')
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
    recovered=resend_native(lost_raw,0)['result'];check(recovered['duplicate'] and recovered['receipt'] is None)
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
        return raw,resend_native(raw,seq)
    app_raw,a0=app_event(0,'turn/start',{'input':app_input,'unknown':True},1)
    a0=a0['result'];check(a0['candidateCount']=='0')
    _,a1=app_event(1,'turn/steer',{'input':app_input,'expectedTurnId':'turn-x'},2)
    a1=a1['result'];check(a1['candidateCount']=='1')
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
    _,r2=app_event(2,'turn/start',{'input':app_input},1)
    r2=r2['result'];check(r2['candidateCount']=='1')
    _,r3=app_response(3,2);r3=r3['result']
    check('error' in app_response(3,0)[1]) # Same wire ID/body, wrong original lineage.
    check(app_response(3,2)[1]['result']['duplicate'])
    check('error' in app_response(4,1)[1]) # A response cannot masquerade as a request.
    _,r4=app_event(4,'turn/start',{'input':app_input},2)
    r4=r4['result'];check(r4['candidateCount']=='2')
    c.close()
    c=Client('open',response_root,path);c.initialize()
    check(c.call('swegca/agent/attach/resume',app_binding)['result']['identity']==response_id)
    check(c.call('swegca/select',{'identity':response_id})['result']=={})
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
    check(c.call('swegca/work',{'seed':'7','step':'7'})['result']['merged']=='0')
    check(c.call('swegca/end')['result']=={})
    check(c.call('swegca/work',{'seed':'7','step':'7'})['result']['merged']=='1')
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
