import os,json,pathlib,subprocess,hashlib,time,tarfile,shutil,concurrent.futures,mimetypes,threading
os.umask(0o077)
repo=pathlib.Path('/var/home/raspie/Documents/Codex/SWEGCA-VRS-MCP-clean-cpp');out=pathlib.Path('/var/mnt/storage-cold/vrs-pc-whole-20260928');out.mkdir(exist_ok=True)
state={'stage':'preparing codec-backed whole-file input','files':0,'parts':0,'bytes':0,'duplicates':0,'errors':0,'accept':0,'reject':0,'abstain':0,'pair_verifications':0,'current':'','cpu_workers':10,'gpu_core_active':False};lock=threading.Lock();seen=set();started=time.time()
def progress():
 with lock:
  temp=out/'live.tmp';temp.write_text(json.dumps(state));temp.replace(out/'live.json')
progress()
log=(out/'receipts.jsonl').open('a',buffering=1);err=(out/'errors.jsonl').open('a',buffering=1)
# Stage originals before codec work; the immutable snapshot is the same byte
# payload supplied with decoded views. Original one-file identity survives.
def prepare(path):
 d=out/'staging'/hashlib.sha256(path.encode()).hexdigest();d.mkdir(parents=True,exist_ok=True)
 h=hashlib.sha256();source=pathlib.Path(path)
 with source.open('rb') as f,(d/'original').open('wb') as dest:
  before=os.fstat(f.fileno())
  while b:=f.read(1024*1024):h.update(b);dest.write(b)
  after=os.fstat(f.fileno())
 if (before.st_size,before.st_mtime_ns)!=(after.st_size,after.st_mtime_ns):raise RuntimeError('source changed during snapshot')
 digest=h.hexdigest()
 with lock:
  if digest in seen:state['duplicates']+=1;shutil.rmtree(d);return None
  seen.add(digest)
 mime=subprocess.run(['file','--brief','--mime-type',str(d/'original')],capture_output=True,text=True,check=True).stdout.strip()
 manifest={'source':path,'sha256':digest,'bytes':before.st_size,'media':mime,'codec_results':[],'experience_unit':'one source file plus its decoded views','language_filter':None}
 def run(args,name):
  with (d/(name+'.stderr')).open('wb') as stderr:
   r=subprocess.run(args,stdout=subprocess.DEVNULL,stderr=stderr,env={**os.environ,'MAGICK_THREAD_LIMIT':'1'})
  manifest['codec_results'].append({'operation':name,'exit_code':r.returncode})
  return r.returncode
 if mime.startswith(('video/','audio/')):
  r=subprocess.run(['ffprobe','-v','error','-show_streams','-show_format','-of','json',str(d/'original')],capture_output=True)
  (d/'streams.json').write_bytes(r.stdout)
  streams=json.loads(r.stdout or b'{}').get('streams',[])
  if any(x['codec_type'] in ('video','audio') for x in streams):
   run(['ffmpeg','-nostdin','-v','error','-threads','1','-i',str(d/'original'),'-map','0:v?','-map','0:a?','-c:v','rawvideo','-c:a','pcm_f64le','-threads','1','-f','nut',str(d/'decoded.nut')],'decode_av')
  for s in streams:
   if s.get('codec_type')=='subtitle':
    i=s['index'];run(['ffmpeg','-nostdin','-v','error','-threads','1','-i',str(d/'original'),'-map',f'0:{i}','-c:s','ass',str(d/f'subtitle-{i}.ass')],f'decode_subtitle_{i}')
    # The original stream remains in the source even for bitmap subtitles.
 elif mime.startswith('image/'):
  run(['magick',str(d/'original'),'-compress','None',str(d/'decoded.miff')],'decode_image')
 elif mime=='application/pdf':run(['pdftotext','-layout',str(d/'original'),str(d/'decoded.txt')],'decode_pdf')
 # Preserve all languages. Same-stem sidecars are labelled as candidates,
 # never asserted to be aligned transcripts based on filenames alone.
 side=[]
 for ext in ('.srt','.ass','.ssa','.vtt','.lrc','.txt'):
  sibling=source.with_suffix(ext)
  if sibling!=source and sibling.is_file():
   target='sidecar'+ext;shutil.copyfile(sibling,d/target);side.append({'source':str(sibling),'member':target,'relation':'same-stem candidate; alignment not asserted'})
 manifest['sidecars']=side
 (d/'manifest.json').write_text(json.dumps(manifest,ensure_ascii=True))
 bundle=d.with_suffix('.tar')
 with tarfile.open(bundle,'w',format=tarfile.PAX_FORMAT) as tar:
  for member in sorted(d.iterdir()):tar.add(member,arcname=member.name,recursive=False)
 shutil.rmtree(d)
 return {'path':str(bundle),'source':path,'media':'application/x-swegca-whole-file-bundle+tar','raw_sha256':digest,'raw_bytes':before.st_size,'codec_results':manifest['codec_results']}
def paths():
 roots=['/var/home/raspie/Pictures','/var/home/raspie/Videos','/var/home/raspie/Documents','/var','/run/media/raspie/NVMe 4.0 1TB','/run/media/raspie/NVMe 4.0 2TB','/usr','/etc','/opt','/root'];dirs=set()
 excluded={str(out),'/var/mnt/storage','/var/mnt/storage-cold/vrs-pc-raw-20260928','/var/home/raspie/Documents/Codex/vrs-pc-corpus-20260928'}
 for root in roots:
  for base,sub,files in os.walk(os.path.realpath(root),followlinks=False):
   if base in excluded:sub[:]=[];continue
   try:s=os.stat(base)
   except OSError:continue
   key=(s.st_dev,s.st_ino)
   if key in dirs:sub[:]=[];continue
   dirs.add(key)
   for name in files:
    path=os.path.join(base,name)
    if os.path.isfile(path) and not os.path.islink(path):yield path
p=subprocess.Popen([str(repo/'build/whole-file-ingress'),str(out/'runtime')],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=(out/'native.stderr').open('ab'))
try:
 with concurrent.futures.ThreadPoolExecutor(max_workers=10) as pool:
  iterator=iter(paths());pending={};batch=[]
  for _ in range(10):
   path=next(iterator,None)
   if path:pending[pool.submit(prepare,path)]=path
  while pending:
   completed,_=concurrent.futures.wait(pending,return_when=concurrent.futures.FIRST_COMPLETED)
   for f in completed:
    path=pending.pop(f);state['current']=path
    try:
     item=f.result()
     if item:batch.append(item)
    except Exception as e:state['errors']+=1;err.write(json.dumps({'path':path,'error':str(e)})+'\n')
   if batch:
    state['stage']='native whole-experience input';progress()
    p.stdin.write((json.dumps(batch)+'\n').encode());p.stdin.flush();line=p.stdout.readline()
    if not line:raise RuntimeError('native input failed; see native.stderr')
    for r in json.loads(line):
     item=batch[r['index']];state['files']+=1;state['parts']+=1;state['bytes']+=item['raw_bytes'];state[{0:'abstain',1:'accept',2:'reject'}[r['status']]]+=1
     log.write(json.dumps({'file':item,'receipt':r})+'\n')
    for item in batch:os.unlink(item['path'])
    batch.clear();progress()
   while len(pending)<10:
    path=next(iterator,None)
    if path is None:break
    pending[pool.submit(prepare,path)]=path
 state['stage']='completed';progress()
except Exception as e:
 state['stage']='stopped: '+str(e);progress();raise
finally:p.stdin.close();p.wait()
