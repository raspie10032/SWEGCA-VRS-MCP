#!/usr/bin/env python3
"""Synthetic desktop stdin -> actual core Recall entry; no model/account calls."""
import json,os,pathlib,select,subprocess,sys,tempfile,time
build=pathlib.Path(sys.argv[1]).resolve()
count=int(sys.argv[2]) if len(sys.argv)>2 else 5
if not 1<=count<=100:raise ValueError('sample count must be 1..100')
with tempfile.TemporaryDirectory(prefix='swegca-desktop-latency-') as directory:
    base=pathlib.Path(directory);root=base/'state';root.mkdir()
    policy=dict(chance_rate=.2,accept_margin=.25,confidence_level=.9,prior_alpha=1.,prior_beta=1.,
        regime_change_threshold=.3,minimum_effective_samples_per_axis='4',minimum_source_diversity='2',
        minimum_axis_source_diversity='1',minimum_context_diversity='4',recent_window='6',minimum_recent_samples='4',axis_count='1')
    resources=base/'resources.json'
    resources.write_text(json.dumps(dict(cpuAffinity='6 7',memoryBytes=str(128<<20),frameBytes=str(16<<20),
        mainIdentity=bytes([99]+[0]*31).hex(),initialStrength=1.,sessionBlockBytes=str(8<<20),mainBlockBytes='4096',
        readLimit=str(4<<20),ioBytesPerSecond='625000000',storageBytes='500000000000',mergeWorkers='2',policy=policy)))
    proxy=base/'proxy.json';proxy.write_text(json.dumps(dict(memoryBytes=str(64<<20),frameBytes=str(2<<20),
        pendingRequests='8',sessionCapacity='2',seed='7',step='0',instance='latency-fixture',sessions=[],
        connectionSession=dict(session='transport',mode='ensure'))))
    backend=base/'backend';backend.write_text('#!'+sys.executable+'\n'+'''import json,sys
for line in sys.stdin:
    v=json.loads(line)
    if v.get('method')=='thread/start':
        print(json.dumps({'method':'thread/started','params':{'thread':{'id':'fixture'}}}),flush=True)
    if 'id' in v:print(json.dumps({'id':v['id'],'result':{}}),flush=True)
''');backend.chmod(0o700)
    config=base/'wrapper.json';config.write_text(json.dumps(dict(backend=str(backend),host=str(build/'swegca-desktop-host'),
        proxy=str(build/'swegca-app-server-proxy'),vrs=str(build/'swegca-vrs-ingress-probe'),mode='ensure',root=str(root),
        resourceConfig=str(resources),proxyConfig=str(proxy))))
    process=subprocess.Popen([str(build/'swegca-codex-wrapper'),'-c','features.code_mode_host=true','app-server',
        '--analytics-default-enabled'],env=dict(os.environ,SWEGCA_DESKTOP_CONFIG=str(config)),
        stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,bufsize=0)
    def line(stream):
        if not select.select([stream],[],[],20)[0]:raise RuntimeError('diagnostic stream timeout')
        value=stream.readline()
        if not value:raise RuntimeError('diagnostic stream closed')
        return value
    def send(value):
        raw=(json.dumps(value,separators=(',',':'))+'\n').encode()
        # FileIO can make short writes for large native frames.
        view=memoryview(raw);began=time.monotonic_ns()
        while view:
            n=process.stdin.write(view)
            if not n:raise RuntimeError('diagnostic input closed')
            view=view[n:]
        return began,len(raw)-1
    try:
        send(dict(id=1,method='initialize',params={}));assert json.loads(line(process.stdout))['id']==1
        send(dict(method='initialized'))
        send(dict(id=2,method='thread/start',params={}))
        assert json.loads(line(process.stdout))['method']=='thread/started'
        assert json.loads(line(process.stdout))['id']==2
        serial=2
        for size in (128,4096,65536,1048576):
            samples=[];native_bytes=0
            for _ in range(count):
                serial+=1
                began,native_bytes=send(dict(id=serial,method='turn/start',params=dict(threadId='fixture',input=[dict(type='text',text='x'*size)])))
                marker=line(process.stderr).decode().strip().split()
                if len(marker)!=2 or marker[0]!='SWEGCA_RECALL_NS':raise RuntimeError('invalid Recall marker')
                elapsed=int(marker[1])-began
                assert elapsed>=0 and json.loads(line(process.stdout))['id']==serial
                samples.append(elapsed)
            ordered=sorted(samples)
            print(json.dumps(dict(boundary='desktop stdin write start -> core Recall entry',scenario='empty-main/new-temporary-session',
                promptBytes=size,nativeBytes=native_bytes,samplesNs=samples,medianNs=ordered[len(ordered)//2],
                maxNs=max(samples),atLeast1ms=sum(n>=1000000 for n in samples))),flush=True)
        process.stdin.close()
        assert process.wait(timeout=10)==0 and process.stdout.read()==b'' and process.stderr.read()==b''
    finally:
        if process.poll() is None:process.terminate();process.wait(timeout=10)
