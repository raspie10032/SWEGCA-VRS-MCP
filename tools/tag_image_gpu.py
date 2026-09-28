"""GPU observation adapter. No verdicts, thresholds or strengthening here."""
import argparse,json,time,hashlib
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('directory',type=Path);p.add_argument('gpu',type=int);a=p.parse_args()
import numpy as np
import torch
torch.set_num_threads(1);torch.set_num_interop_threads(1)
root=a.directory;meta=json.loads((root/'gpu-input.json').read_text())
raw=(root/'members.i32').read_bytes()
if hashlib.sha256(raw).hexdigest()!=meta['members_sha256']:raise ValueError('input digest mismatch')
n,t,k=meta['images'],meta['tags'],meta['max_members'];lo=t*a.gpu//2;hi=t*(a.gpu+1)//2
if a.gpu not in (0,1):raise ValueError('expected one of the two assigned GPUs')
started=time.time();device=torch.device(f'cuda:{a.gpu}')
x=torch.from_numpy(np.frombuffer(raw,dtype='<i4').copy().reshape(n,k)).to(device)
start=torch.cuda.Event(enable_timing=True);end=torch.cuda.Event(enable_timing=True);start.record(torch.cuda.current_stream(device))
with (root/f'gpu{a.gpu}.observations.u8').open('wb') as out:
 for begin in range(lo,hi,32):
  ids=torch.arange(begin,min(begin+32,hi),device=device,dtype=torch.int32)
  # Each recorded tag is independently compared to EVERY image's raw member IDs.
  # No rarity filter, no tag-score cutoff, and no absent -> negative inference.
  observed=(ids[:,None,None]==x[None,:,:]).any(dim=2)
  out.write(observed.to(torch.uint8).cpu().numpy().tobytes())
  if (begin-lo)//32%16==0:print(f'gpu={a.gpu} tags={min(begin+32,hi)-lo}/{hi-lo}',flush=True)
end.record(torch.cuda.current_stream(device));torch.cuda.synchronize(device)
report={'gpu':a.gpu,'device':torch.cuda.get_device_name(device),'tag_begin':lo,'tag_end':hi,'images':n,'comparisons':(hi-lo)*n,'started_unix':started,'finished_unix':time.time(),'cuda_interval_ms':start.elapsed_time(end),'cuda_peak_allocated_bytes':torch.cuda.max_memory_allocated(device),'members_sha256':meta['members_sha256'],'output_sha256':hashlib.sha256((root/f'gpu{a.gpu}.observations.u8').read_bytes()).hexdigest(),'verdicts_computed':False}
(root/f'gpu{a.gpu}.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report),flush=True)
