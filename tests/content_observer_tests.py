#!/usr/bin/env python3
"""Real read-only MCP producer, no model/account/desktop configuration changes."""
import hashlib
import json
import os
import pathlib
import select
import subprocess
import sys
import tempfile

exe = pathlib.Path(sys.argv[1]).resolve()
checks = 0
def check(value):
    global checks
    checks += 1
    assert value

class Observer:
    def __init__(self, limit=100000):
        self.process = subprocess.Popen([str(exe), '16777216', '625000000', str(limit)],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.serial = 0
    def __enter__(self):
        return self
    def __exit__(self, *unused):
        try:
            self.process.stdin.close()
            check(self.process.wait(timeout=5) == 0)
            check(self.process.stderr.read() == b'')
        finally:
            if self.process.poll() is None:
                self.process.kill(); self.process.wait()
    def raw(self, data):
        self.process.stdin.write(data); self.process.stdin.flush()
        check(bool(select.select([self.process.stdout], [], [], 5)[0]))
        return json.loads(self.process.stdout.readline())
    def call(self, method, params=None):
        self.serial += 1
        reply = self.raw(json.dumps({'jsonrpc':'2.0','id':self.serial,
            'method':method,'params':params or {}}).encode()+b'\n')
        check(reply['id'] == self.serial)
        return reply
    def initialize(self):
        check('error' in self.call('tools/list'))
        check('result' in self.call('initialize', {'protocolVersion':'2025-06-18',
            'capabilities':{},'clientInfo':{'name':'file-observer-test','version':'1'}}))
        self.process.stdin.write(b'{"jsonrpc":"2.0","method":"notifications/initialized"}\n')
        self.process.stdin.flush()
    def measure(self, args):
        return self.call('tools/call', {'name':'observe_file_content_equality','arguments':args})

with tempfile.TemporaryDirectory(prefix='swegca-observer-') as directory:
    root=pathlib.Path(directory); left=root/'left'; right=root/'right'
    left.write_bytes(b''); right.write_bytes(b'')
    args={'inputOriginal':{'block':'11'*32,'digest':'22'*32,'offset':'0','bytes':'1'},
          'left':str(left),'right':str(right)}
    with Observer() as observer:
        observer.initialize()
        listing=observer.call('tools/list')['result']['tools']
        check(len(listing)==1 and listing[0]['annotations']['readOnlyHint'])
        def measured(outcome):
            result=observer.measure(args)['result']
            check(not result['isError'])
            check('not a task-completion judgment' in result['content'][0]['text'])
            body=result['structuredContent']; report=body['swegcaObservation']
            check(report['inputOriginal']==args['inputOriginal'] and report['axis']=='0')
            check(report['outcome']==outcome and not body['grantsAuthority'])
            check(json.loads(report['scope'])=={'predicate':'equal-file-bytes-v1',
                'left':str(left),'right':str(right)})
            return body['measurement']
        measured('support')
        content=b'\x00\xff'+b'x'*70000+b'\x00'
        left.write_bytes(content);right.write_bytes(content)
        before=(left.stat().st_mtime_ns,right.stat().st_mtime_ns)
        same=measured('support')
        check(same['complete'] and same['stable'] and same['ioError']==0)
        check(same['left']['digest']==same['right']['digest']==hashlib.sha256(content).hexdigest())
        check(same['left']['readBytes']==str(len(content)))
        check(before==(left.stat().st_mtime_ns,right.stat().st_mtime_ns))
        right.write_bytes(content[:-1]+b'z')
        different=measured('refute')
        check(different['left']['digest']!=different['right']['digest'])
        right.unlink()
        missing=measured('insufficient')
        check(not missing['complete'] and missing['ioError']!=0 and missing['right']['digest'] is None)
        check(missing['right']['before'] is None)
        os.mkfifo(right)
        pipe=measured('insufficient')
        check(not pipe['complete'] and pipe['ioError']!=0)
        right.unlink();right.write_bytes(content)
        for extra in ('scope','outcome','axis','confidence'):
            check('error' in observer.measure(dict(args, **{extra:'override'})))
        check('error' in observer.measure(dict(args, left='relative')))
        check('error' in observer.measure(dict(args, left=str(left)+'\0ignored')))
        check('error' in observer.measure(dict(args, inputOriginal={**args['inputOriginal'],'digest':'wrong'})))
        check(observer.raw(b'[]\n')['error']['code']==-32600)
        check(observer.raw(b'{\n')['error']['code']==-32700)
        check(observer.raw(b'x'*65537+b'\n')['error']['code']==-32700)
        check(observer.call('ping')['result']=={})
        measured('support')
    with Observer(limit=10) as observer:
        observer.initialize()
        limited=observer.measure(args)['result']['structuredContent']
        check(limited['swegcaObservation']['outcome']=='insufficient')
        check(limited['measurement']['left']['readBytes']=='0')
        check(limited['measurement']['left']['digest'] is None)
    check(left.read_bytes()==right.read_bytes()==content)
env=dict(os.environ);env.pop('SWEGCA_IO_OWNER',None)
missing=subprocess.run([str(exe),str(128<<20),'625000000',str(16<<20),'--require-shared-io'],
                       input=b'',capture_output=True,env=env,timeout=5)
check(missing.returncode!=0 and b'shared I/O owner required' in missing.stderr)
env['SWEGCA_IO_OWNER']='invalid'
malformed=subprocess.run([str(exe),str(128<<20),'625000000',str(16<<20),'--require-shared-io'],
                         input=b'',capture_output=True,env=env,timeout=5)
check(malformed.returncode!=0 and not malformed.stdout)
print(f'content observer tests: {checks} checks passed')
