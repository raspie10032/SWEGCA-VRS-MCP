"""Kill only an isolated real VRS test service, then verify durable recovery.

The deliberate mismatch (256MiB PMR / 64MiB kernel) forces a kernel OOM during
frame buffering. Normal limited-open recovery uses matching 64MiB controls.
"""
import contextlib
import json
import os
import pathlib
import select
import subprocess
import sys
import tempfile
import time
import uuid
exe=str(pathlib.Path(sys.argv[1]).resolve())
checks=0
def check(value):
    global checks
    checks+=1
    assert value

def identity(n):return bytes([n]+[0]*31).hex()
class Client:
    def __init__(self,args):
        self.p=subprocess.Popen(args,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
        self.serial=0
        cleanup.callback(self.abort)
    def abort(self):
        if self.p.poll() is not None:return
        try:self.p.stdin.close()
        except BrokenPipeError:pass
        try:self.p.wait(timeout=10)
        except subprocess.TimeoutExpired:
            self.p.kill();self.p.wait(timeout=10)
    def call(self,method,params=None):
        self.serial+=1
        line=json.dumps(dict(jsonrpc='2.0',id=self.serial,method=method,params=params or {})).encode()+b'\n'
        self.p.stdin.write(line);self.p.stdin.flush()
        check(bool(select.select([self.p.stdout],[],[],15)[0]))
        reply=json.loads(self.p.stdout.readline());check(reply['id']==self.serial)
        check('error' not in reply)
        return reply['result']
    def initialize(self):
        self.call('initialize',dict(protocolVersion='2025-06-18',capabilities={},clientInfo=dict(name='oom-recovery',version='1')))
        self.p.stdin.write(b'{"jsonrpc":"2.0","method":"notifications/initialized"}\n');self.p.stdin.flush()
    def close(self):
        self.p.stdin.close();check(self.p.wait(timeout=15)==0)
        check(self.p.stdout.read()==b'' and self.p.stderr.read()==b'')

with tempfile.TemporaryDirectory(prefix='swegca-oom-recovery-') as directory, contextlib.ExitStack() as cleanup:
    root=pathlib.Path(directory);config=json.loads(pathlib.Path('examples/stdio-config.json').read_text())
    config.update(memoryBytes=str(256<<20),frameBytes=str(128<<20),cpuAffinity='6 7')
    path=root/'config.json';path.write_text(json.dumps(config))
    unit='swegca-vrs-oom-test-'+uuid.uuid4().hex+'.service'
    c=Client(['systemd-run','--user','--pipe','--wait','--collect','--quiet','--unit='+unit,
        '--property=MemoryMax=67108864','--property=MemorySwapMax=0','--property=CPUAffinity=6 7',
        '--property=OOMPolicy=kill','--',exe,'create',str(root),str(path)])
    try:
        c.initialize();c.call('swegca/start',dict(identity=identity(1),name='original'))
        def event(session,n):return dict(sequence=str(n),observedAt='0',seed='7',step='0',session=session,
            source='user',media='text/plain',content='원경험\x00preserve after OOM')
        first=c.call('swegca/receive',event('original',0))
        second=c.call('swegca/receive',event('original',1))
        check(len(second['candidates'])==1)
        # No newline yet: this new frame is not admitted to Runtime or storage.
        fd=c.p.stdin.fileno();os.set_blocking(fd,False)
        pending=b'{"jsonrpc":"2.0","id":999,"method":"swegca/receive","params":{"content":"'
        deadline=time.monotonic()+20;sent=0
        while c.p.poll() is None and time.monotonic()<deadline:
            if not select.select([],[fd],[],0.2)[1]:continue
            try:
                if not pending:pending=b'x'*(1<<20)
                count=os.write(fd,pending);pending=pending[count:];sent+=count
            except BrokenPipeError:break
            except BlockingIOError:continue
            assert sent<128<<20
        check(c.p.wait(timeout=10)!=0)
        journal=subprocess.check_output(['journalctl','--user','--unit',unit,'--no-pager','-n','20','-o','cat'],text=True)
        assert 'oom-kill' in journal or 'OOM killer' in journal,(c.p.returncode,c.p.stderr.read().decode(),journal)
        check(c.p.stdout.read()==b'')
        print(f'Confirmed kernel OOM in {unit}; sent {sent} frame bytes without admitting the frame')
    finally:
        # Exact test-owned unit only; never touch another VRS/Palworld process.
        if c.p.poll() is None:
            subprocess.run(['systemctl','--user','stop',unit],check=False,capture_output=True)
            c.p.wait(timeout=10)
    config.update(memoryBytes=str(64<<20),frameBytes='4096');path.write_text(json.dumps(config))
    c=Client([exe,'limited-open',str(root),str(path)]);c.initialize()
    check(c.call('swegca/work',dict(seed='7',step='0'))['merged']=='0')
    c.call('swegca/resume',dict(identity=identity(1)))
    recovered=c.call('swegca/receive',event('original',2))
    check(recovered['temporary'] and len(recovered['candidates'])==2)
    replay=c.call('tools/call',dict(name='vrs_replay',arguments=dict(receipt=recovered['receipt'],candidate='0')))['structuredContent']
    check(replay['original']==first['original'])
    check(bytes.fromhex(replay['contentHex']).decode()==event('original',0)['content'])
    c.call('swegca/end');check(c.call('swegca/work',dict(seed='7',step='0'))['merged']=='1');c.close()
    c=Client([exe,'limited-open',str(root),str(path)]);c.initialize()
    c.call('swegca/start',dict(identity=identity(2),name='next'))
    main=c.call('swegca/receive',event('next',0))
    check(not main['temporary'] and len(main['candidates'])==3)
    replay=c.call('tools/call',dict(name='vrs_replay',arguments=dict(receipt=main['receipt'],candidate='0')))['structuredContent']
    check(replay['original']==first['original']);c.close()
print(f'PASS: real VRS OOM and recovery, {checks} checks')
