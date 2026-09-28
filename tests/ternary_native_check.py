"""Authenticate integer snapshots and export exact slope frequency tables."""
import pathlib,json,struct,hashlib,sys,csv,collections,resource
resource.setrlimit(resource.RLIMIT_AS,(4_000_000_000,4_000_000_000))
import numpy as np
root=pathlib.Path(sys.argv[1]);m=json.loads((root/'inputs.json').read_text());n=m['images'];t=m['tags'];rounds=m['rounds']
assert m['representation']=='accept_reject_abstain_u64' and m['activation']=='accept>=reject'
reports={r['round']:r for r in map(json.loads,(root/'rounds.jsonl').open())};assert len(reports)==rounds
refs={};blocks={}
for p in root.glob('collision-*.block'):
 f=p.open('rb');h=f.read(80);assert h[:8]==b'SWGCBLK1' and hashlib.sha256(h[:48]).digest()==h[48:];blocks[h[8:40].hex()]=f
for r in map(json.loads,(root/'checkpoints.jsonl').open()):
 key=(r['round'],r['kind'],r['start']);assert key not in refs;refs[key]=r
sizes={'tag_image':n*t,'tag_tag':t*(t-1)//2}
def read(r):
 a=r['record'];f=blocks[a['block']];f.seek(a['offset']);raw=f.read(a['bytes']);assert len(raw)==a['bytes']
 assert raw[:8]==b'SWGCEXP1' and hashlib.sha256(raw[:-48]).hexdigest()==a['digest'] and raw[-48:-16].hex()==a['digest'] and raw[-16:-8]==b'SWGCEND1'
 ss,so,sm,sc=struct.unpack_from('<QQQQ',raw,32);payload=raw[64+ss+so+sm:64+ss+so+sm+sc];assert len(payload)==r['count']*24
 return np.frombuffer(payload,dtype='<u8').reshape(-1,3)
hist={k:[collections.Counter() for _ in range(4)] for k in sizes};verified=0;tag_delta=np.zeros((t,3),dtype=np.int64);offsets=np.array([a*(2*t-a-1)//2 for a in range(t)],dtype=np.int64)
for rn in range(rounds+1):
 changed=0;active_changes=0;max_increment=0
 for kind,size in sizes.items():
  active_count=0;total_delta=np.zeros(3,dtype=np.uint64);end=0
  for start in range(0,size,16384):
   row=refs[(rn,kind,start)];cur=read(row);assert start==end and len(cur)==min(16384,size-start);end+=len(cur);verified+=cur.size
   active=cur[:,0]>=cur[:,1];active_count+=int(active.sum())
   if rn:
    prev=read(refs[(rn-1,kind,start)]);assert np.all(cur>=prev);delta=cur-prev;total_delta+=delta.sum(axis=0)
    changed+=int(np.any(delta!=0,axis=1).sum());active_changes+=int(np.count_nonzero(active!=(prev[:,0]>=prev[:,1])));max_increment=max(max_increment,int(delta.max()))
   if rn==rounds:
    base=read(refs[(0,kind,start)]);assert np.all(cur-base<2**63);d=(cur-base).astype(np.int64)
    for axis,v in enumerate((d[:,0],d[:,1],d[:,2],d[:,0]-d[:,1])):
     values,counts=np.unique(v,return_counts=True);hist[kind][axis].update({int(a):int(b) for a,b in zip(values,counts)})
    ids=np.arange(start,start+len(d))
    if kind=='tag_image':np.add.at(tag_delta,ids//n,d)
    else:
     left=np.searchsorted(offsets,ids,side='right')-1;right=left+1+ids-offsets[left];np.add.at(tag_delta,left,d);np.add.at(tag_delta,right,d)
  assert end==size
  if rn:
   assert active_count==reports[rn]['active_'+kind] and total_delta.tolist()==reports[rn][kind]
 if rn:assert changed==reports[rn]['changed_strengths'] and active_changes==reports[rn]['active_set_changes'] and max_increment==reports[rn]['max_component_increment']
 print('checked round',rn,flush=True)
# Per-original experience attribution: verdicts issued when that input arrived.
experience=np.zeros((rounds+1,n,3),dtype=np.int64);seen=set()
for r in map(json.loads,(root/'experience-counts.jsonl').open()):
 k=(r['round'],r['input']);assert k not in seen;seen.add(k);experience[k[0],k[1]]=r['counts']
assert len(seen)==rounds*n and np.all(experience[1:]>=experience[:-1])
for rn in range(1,rounds+1):assert (experience[rn]-experience[rn-1]).sum(axis=0).tolist()==[a+b for a,b in zip(reports[rn]['tag_image'],reports[rn]['tag_tag'])]
with (root/'experience-slopes.csv').open('w') as f:
 w=csv.writer(f);w.writerow(['input','accept','reject','abstain','accept_per_round','reject_per_round','abstain_per_round','net_per_round'])
 for i,c in enumerate(experience[-1]):w.writerow([i,*map(int,c),*(c/rounds),(int(c[0])-int(c[1]))/rounds])
with (root/'tag-slopes.csv').open('w') as f:
 w=csv.writer(f);w.writerow(['tag_id','accept_per_round','reject_per_round','abstain_per_round','net_per_round'])
 for i,c in enumerate(tag_delta):w.writerow([i,*(c/rounds),(int(c[0])-int(c[1]))/rounds])
with (root/'connection-slope-histograms.json').open('w') as f:json.dump({'rounds':rounds,'component_order':['accept','reject','abstain','net'],'delta_histograms':{k:[dict(sorted(c.items())) for c in cs] for k,cs in hist.items()}},f)
for e in map(json.loads,(root/'trace.jsonl').open()):
 before=e['previous'];after=e['current'];axis={1:0,2:1,0:2}[e['status']];want=before.copy();want[axis]+=1;assert after==want
result={'passed':True,'rounds':rounds,'integer_components_checked':verified,'connections':sum(sizes.values()),'experiences':n,'stored_order':['accept','reject','abstain'],'slope_definition':'(count_round_10-count_round_0)/10; net is an analysis-only accept-minus-reject projection','experience_totals':experience[-1].sum(axis=0).tolist(),'activation':'accept>=reject','initialization':'one recorded original verdict per connection; previous float run was not inverted'}
(root/'check.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result))
