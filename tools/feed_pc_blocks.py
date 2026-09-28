#!/usr/bin/env python3
"""Streaming whole-PC feeder with observable receipts. No content filters."""
import collections
import json
import os
from pathlib import Path
import random
import subprocess
import sys
import threading
import time

out = Path(sys.argv[1]).resolve()
out.mkdir(parents=True, exist_ok=True)
os.umask(0o077)
repo = Path(__file__).resolve().parents[1]
roots = [Path(p) for p in ('/var/home/raspie/Pictures', '/var/home/raspie/Videos',
    '/var/home/raspie/Music', '/var/mnt/storage-cold/Media', '/var', '/usr', '/etc', '/opt', '/root', '/run/media') if Path(p).exists()]
state = dict(stage='starting', submitted=0, retained=0, duplicates=0, errors=0,
             input_status=[0, 0, 0], relation_counts=[0, 0, 0], backend_inputs={},
             roots=[str(p) for p in roots], started=time.time(), completed=False,
             claim='raw ingress plus unobserved within-block pair hypotheses; absent evidence remains abstention')
lock = threading.Lock()
slots = threading.Semaphore(64)
inflight = {}
stop = threading.Event()
cmd = ['taskset', '-c', '3-7,11-15', str(repo/'build/whole-file-ingress-gpu'), str(out/'store'),
       '--gpus', '2', '--block-originals', '128', '--mix-unknown', '1']
(out/'launch.json').write_text(json.dumps(dict(argv=cmd,roots=state['roots'],content_filter=None),indent=2))
error_log = (out/'errors.jsonl').open('a', buffering=1)

def publish():
    with lock:
        state['elapsed_seconds'] = time.time()-state['started']
        temp = out/'live.tmp';temp.write_text(json.dumps(state));temp.replace(out/'live.json')

native = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
    stderr=(out/'native.stderr').open('a'), text=True, bufsize=1)

def consume():
    with (out/'receipts.jsonl').open('a', buffering=1) as log:
        for line in native.stdout:
            r = json.loads(line)
            with lock:
                source = inflight.pop(r['ticket'])
                if r.get('error'):
                    state['errors'] += 1;error_log.write(json.dumps(dict(source=source,receipt=r))+'\n')
                elif r.get('duplicate'):state['duplicates'] += 1
                else:
                    state['retained'] += 1;state['input_status'][r['status']] += 1
                    for i, n in enumerate(r['relation_counts']):state['relation_counts'][i] += n
                    backend = r['backend'];state['backend_inputs'][backend] = state['backend_inputs'].get(backend,0)+1
                log.write(json.dumps(dict(source=source,receipt=r))+'\n')
            slots.release()
    stop.set();slots.release()

thread = threading.Thread(target=consume);thread.start()
def monitor():
    while not stop.wait(1):publish()
monitor_thread = threading.Thread(target=monitor);monitor_thread.start()
visited = set()
def traversal(root):
    def walk_error(error):
        with lock:state['errors'] += 1;error_log.write(json.dumps(dict(path=error.filename,error=str(error)))+'\n')
    for base, dirs, files in os.walk(root.resolve(), followlinks=False, onerror=walk_error):
        path = Path(base).resolve()
        if path == out or out in path.parents:dirs[:] = [];continue
        try:st=path.stat()
        except OSError as error:walk_error(error);dirs[:] = [];continue
        key=(st.st_dev,st.st_ino)
        if key in visited:dirs[:] = [];continue
        visited.add(key)
        for name in files:
            p=Path(base)/name
            if p.is_symlink() or not p.is_file():continue
            if not os.access(p,os.R_OK):walk_error(PermissionError(13,'unreadable',str(p)));continue
            yield p
try:
    walkers=collections.deque(iter(traversal(p)) for p in roots)
    state['stage']='whole_pc_ingress_and_unknown_pair_mixing';publish()
    ticket=0
    while walkers:
        buffer=[]
        while walkers and len(buffer)<128:
            walker=walkers.popleft()
            try:buffer.append(next(walker));walkers.append(walker)
            except StopIteration:pass
        random.SystemRandom().shuffle(buffer)
        for path in buffer:
            slots.acquire()
            if stop.is_set():raise RuntimeError('native input stopped; see native.stderr')
            ticket+=1
            with lock:inflight[ticket]=str(path);state['submitted']+=1
            native.stdin.write(json.dumps(dict(path=str(path),source=str(path),media='application/octet-stream',ticket=ticket))+'\n');native.stdin.flush()
    native.stdin.close();code=native.wait();thread.join()
    with lock:state['stage']='completed' if code==0 else 'failed';state['completed']=code==0;state['exit_code']=code
except BaseException as error:
    if native.poll() is None:native.terminate()
    native.wait()
    with lock:state['stage']='stopped';state['error']=str(error);state['exit_code']=native.returncode
finally:
    stop.set();thread.join();monitor_thread.join();publish()
sys.exit(0 if state['completed'] else 1)
