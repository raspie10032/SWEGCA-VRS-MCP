"""Independent native checkpoint and raw-input collision audit; NumPy is test-only."""
import pathlib,json,struct,hashlib,sys,resource
resource.setrlimit(resource.RLIMIT_AS,(4_000_000_000,4_000_000_000))
import numpy as np
root=pathlib.Path(sys.argv[1]);data=pathlib.Path(sys.argv[2]);graph=pathlib.Path(sys.argv[3])
m=json.loads((root/'inputs.json').read_text());n=m['images'];t=m['tags']
reports={r['round']:r for r in map(json.loads,(root/'rounds.jsonl').open())}
assert set(reports)==set(range(1,m['rounds']+1))
blocks={}
for path in root.glob('collision-*.block'):
 f=path.open('rb');h=f.read(80);assert h[:8]==b'SWGCBLK1' and hashlib.sha256(h[:48]).digest()==h[48:];blocks[h[8:40].hex()]=f
rows=list(map(json.loads,(root/'checkpoints.jsonl').open()))
prev=None;round_counts=[];verified=0;active_intersection=None;active_union=None;growing=None;last_weights=None
for round_no in range(m['rounds']+1):
 arrays={k:np.empty(size,dtype='<f8') for k,size in [('tag_image',n*t),('tag_tag',t*(t-1)//2)]};ends=dict.fromkeys(arrays,0)
 for row in rows:
  if row['round']!=round_no:continue
  kind=row['kind'];assert row['start']==ends[kind];a=row['record'];f=blocks[a['block']];f.seek(a['offset']);raw=f.read(a['bytes'])
  assert len(raw)==a['bytes'] and raw[:8]==b'SWGCEXP1' and hashlib.sha256(raw[:-48]).hexdigest()==a['digest']
  assert raw[-48:-16].hex()==a['digest'] and raw[-16:-8]==b'SWGCEND1'
  ss,so,sm,sc=struct.unpack_from('<QQQQ',raw,32);payload=raw[64+ss+so+sm:64+ss+so+sm+sc]
  assert len(payload)==row['count']*8;end=row['start']+row['count'];arrays[kind][row['start']:end]=np.frombuffer(payload,dtype='<f8');ends[kind]=end;verified+=row['count']
 for kind,a in arrays.items():assert ends[kind]==len(a) and np.all(np.isfinite(a)) and np.all(a>=0)
 if prev is not None:
  r=reports[round_no];changed=0;active_changes=0;delta=0
  for kind,a in arrays.items():
   old=prev[kind];changed+=int(np.count_nonzero(a!=old));active_changes+=int(np.count_nonzero((a>=1)!=(old>=1)));delta=max(delta,float(np.max(np.abs(a-old))))
   assert int(np.count_nonzero(a>=1))==r['active_'+kind]
  assert changed==r['changed_strengths'] and active_changes==r['active_set_changes'] and delta==r['max_strength_delta']
  assert min(float(np.min(a)) for a in arrays.values())==r['min_strength'] and max(float(np.max(a)) for a in arrays.values())==r['max_strength']
  # This dataset has distinct image hashes; verify before using count as distinct.
  common=int(np.count_nonzero(np.count_nonzero(arrays['tag_image'].reshape(t,n)>=1,axis=1)>1));assert common==r['common_concepts']
  increased=arrays['tag_tag']>prev['tag_tag']
  growing=increased.copy() if growing is None else growing&increased
  active=arrays['tag_tag']>=1
  active_intersection=active.copy() if active_intersection is None else active_intersection&active
  active_union=active.copy() if active_union is None else active_union|active
  round_counts.append({'round':round_no,'active_tag_tag':r['active_tag_tag'],'changed_strengths':changed,'active_set_changes':active_changes,'max_strength_delta':delta})
 prev=arrays
# Check recorded collision traces against incoming raw tag observations,
# not the old tag-image acceptance matrices.
tags=list(map(json.loads,(graph/'tags.jsonl').open()));dictionary={(r['model'],r['index'],r['category'],r['tag']):r['id'] for r in tags}
sources=list(map(json.loads,(graph/'sources.jsonl').open()));raw_members=[];hashes=set()
for i,line in enumerate((data/'features.jsonl').open()):
 r=json.loads(line);assert r['sha256']==sources[i]['image_sha256'];hashes.add(r['sha256']);model=r['models']['tag_sha256'];raw_members.append({dictionary[(model,x['index'],x['category'],x['tag'])] for x in r['tags']})
assert len(hashes)==n==len(raw_members)
# Candidate discovery, not an automatic semantic promotion.
weights=prev['tag_tag'];ids=np.flatnonzero(growing);ranked=ids[np.argsort(weights[ids])[-20:][::-1]]
offsets=np.array([a*(2*t-a-1)//2 for a in range(t)],dtype=np.int64)
growing_left=np.searchsorted(offsets,ids,side='right')-1
growing_right=growing_left+1+ids-offsets[growing_left]
growing_tag_count=len(np.unique(np.concatenate((growing_left,growing_right))))
candidates=[]
for idx in ranked:
 left=int(np.searchsorted(offsets,idx,side='right')-1);right=left+1+int(idx-offsets[left]);common=set(tags[left]['witnesses'])&set(tags[right]['witnesses'])
 candidates.append({'left':tags[left]['tag'],'right':tags[right]['tag'],'final_strength':float(weights[idx]),'shared_originals':len(common),'left_originals':len(tags[left]['witnesses']),'right_originals':len(tags[right]['witnesses'])})
traces=0
for e in map(json.loads,(root/'trace.jsonl').open()):
 members=raw_members[e['input']];holds=e['left'] in members and (not e['tag_pair'] or e['right'] in members)
 assert e['status']==(1 if holds else 2);assert e['current']==e['previous']*(1.01 if holds else .995);traces+=1
result={'passed':True,'checkpoint_values_checked':verified,'rounds_checked':len(reports),'raw_input_traces_checked':traces,'always_active_tag_tag_after_each_round':int(active_intersection.sum()),'ever_active_tag_tag_after_each_round':int(active_union.sum()),'rounds':round_counts,'strictly_growing_every_round_tag_tag':int(growing.sum()),'tags_in_all_continuously_growing_pairs':growing_tag_count,'growing_candidate_examples':candidates,'pointwise_fixed_point_at_round_10':False,'reason':'This checks exact state equality, not statistical stationarity; topology changes and maximum strength changes remain nonzero'}
(root/'check.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result))
