#!/usr/bin/env python3
"""Real subprocess transport/lifecycle checks; no client app or service is changed."""
import json, os, pathlib, select, subprocess, sys, tempfile
exe=pathlib.Path(sys.argv[1]).resolve()
checks=0

def check(value):
    global checks
    checks+=1
    assert value

def identity(n):
    return bytes([n]+[0]*31).hex()

class Client:
    def __init__(self,mode,root,config):
        self.p=subprocess.Popen([str(exe),mode,str(root),str(config)],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
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
        check(self.call('initialize',{'protocolVersion':'2025-06-18','capabilities':{},'clientInfo':{'name':'test','version':'1'}})['result']['protocolVersion']=='2025-06-18')
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
        'memoryBytes':str(64<<20),'frameBytes':'4096','mainIdentity':identity(99),'initialStrength':1.0,
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
    payload=event('two','tool',sequence='1',media='application/octet-stream');del payload['content'];payload['contentHex']='00ff80fe0a'
    c.call('swegca/receive',payload)
    payload['sequence']='2';binary=c.call('swegca/receive',payload)['result']
    replay=c.call('tools/call',{'name':'vrs_replay','arguments':{'receipt':binary['receipt'],'candidate':'0'}})['result']['structuredContent']
    check(replay['contentHex']=='00ff80fe0a')
    check(c.call('swegca/end')['result']=={});c.close()
print(f'stdio subprocess tests: {checks} checks passed')
