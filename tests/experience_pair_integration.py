"""Small synthetic opaque-input fixture, not an image quality benchmark."""
import pathlib,json,hashlib,subprocess,csv,sys,tempfile
exe=pathlib.Path(sys.argv[1]).resolve();recall_exe=pathlib.Path(sys.argv[2]).resolve()
with tempfile.TemporaryDirectory(prefix='swegca-pair-test-') as temp:
 root=pathlib.Path(temp);data=root/'data';binding=root/'binding';data.mkdir();binding.mkdir();(data/'images').mkdir()
 sets=[['red','square'],['red','circle'],['red','triangle'],['blue','fish']]
 names={s:i for i,s in enumerate(dict.fromkeys(x for row in sets for x in row))}
 features=[];receipts=[]
 for i,tags in enumerate(sets):
  raw=f'opaque experience {i}'.encode();sha=hashlib.sha256(raw).hexdigest();(data/'images'/f'{sha}.bin').write_bytes(raw)
  (data/f'scores{i}.bin').write_bytes(f'paired observation {i}'.encode())
  vector=[0.0]*384;vector[i]=1.0
  features.append({'sha256':sha,'raw_scores':f'scores{i}.bin','models':{'tag_sha256':'1'*64,'dino_sha256':'2'*64},'tags':[{'index':names[t],'tag':t,'category':0} for t in tags],'dino':vector})
  # Opaque prior references: no source binding content is dereferenced by this builder.
  receipts.append({'sha256':sha,'binding':{'block':'3'*64,'offset':80,'bytes':100,'digest':'4'*64},'head':{'block':'3'*64,'offset':180,'bytes':100,'digest':'5'*64}})
 (data/'features.jsonl').write_text(''.join(json.dumps(x)+'\n' for x in features))
 (binding/'receipts.jsonl').write_text(''.join(json.dumps(x)+'\n' for x in receipts))
 def build(name,previous,seed):
  out=root/name;out.mkdir();subprocess.run([str(exe),str(data),str(out),str(binding),str(previous),str(seed)],check=True)
  rows=list(csv.DictReader((out/'links.csv').open()));return out,{(int(r['left']),int(r['right'])):r for r in rows},rows
 first,rows,order=build('first','-',17)
 assert len(rows)==6 and set(rows)=={(a,b) for a in range(4) for b in range(a+1,4)}
 for pair,row in rows.items():
  expected=pair[1]<3
  assert row['status']==('1' if expected else '0')
  assert float(row['strength'])==(1.01 if expected else 1.0)
  assert row['support']==('3' if expected else '0') # Includes the third experience.
  assert row['refute']=='0'
 tags=[json.loads(x) for x in (first/'tags.jsonl').read_text().splitlines()]
 assert next(x for x in tags if x['tag']=='red')['witnesses']==[0,1,2]
 second,again,order2=build('second',first,71)
 for pair,row in again.items():
  assert float(row['previous_strength'])==float(rows[pair]['strength'])
  assert abs(float(row['strength'])-(1.0201 if pair[1]<3 else 1.0))<1e-14
 assert [(r['left'],r['right']) for r in order]!=[(r['left'],r['right']) for r in order2]
 query=root/'query';query.mkdir();raw=b'unseen synthetic cue';(query/'input.bin').write_bytes(raw)
 (query/'cue.json').write_text(json.dumps({'image_sha256':hashlib.sha256(raw).hexdigest(),'dino_sha256':'2'*64,'dino':features[0]['dino']}))
 subprocess.run([str(recall_exe),str(data),str(first),str(query/'cue.json'),str(query/'result.json')],check=True)
 answer=json.loads((query/'result.json').read_text())
 assert answer['seed_source']==0
 assert {x['tag'] for x in answer['associated_tags']}=={'circle','triangle','blue','fish'}
 assert {x['via_experience'] for x in answer['associated_tags']}=={1,2,3}
 if len(sys.argv)>3:
  # Exercise GPU observation consumption with a deterministic fixture. Physical
  # GPU use is tested separately on the full real dataset, not claimed here.
  tag_exe=str(pathlib.Path(sys.argv[3]).resolve());tag_out=root/'tag-image';tag_out.mkdir()
  subprocess.run([tag_exe,'prepare',str(data),str(first),str(tag_out)],check=True)
  meta=json.loads((tag_out/'gpu-input.json').read_text())
  for gpu in range(2):
   lo=len(tags)*gpu//2;hi=len(tags)*(gpu+1)//2
   payload=bytes(int(image in tags[tag]['sources']) for tag in range(lo,hi) for image in range(4))
   (tag_out/f'gpu{gpu}.observations.u8').write_bytes(payload)
   (tag_out/f'gpu{gpu}.json').write_text(json.dumps({'gpu':gpu,'tag_begin':lo,'tag_end':hi,'images':4,'members_sha256':meta['members_sha256'],'output_sha256':hashlib.sha256(payload).hexdigest()}))
  subprocess.run([tag_exe,'apply',str(data),str(first),str(tag_out)],check=True)
  concepts=[json.loads(x) for x in (tag_out/'concepts.jsonl').read_text().splitlines()]
  red=next(x for x in concepts if x['name']=='red')
  assert red['role']=='common_concept' and red['experiences']==[0,1,2] and red['distinct_images']==3
  restored=json.loads(subprocess.check_output([tag_exe,'inspect',str(tag_out),str(red['tag'])],text=True))
  assert restored==red
  assert all(x['role']=='single_observation' for x in concepts if x['name']!='red')
  report=json.loads((tag_out/'summary.json').read_text())
  assert report['verification_mode']=='binary'
  assert report['accept']==8 and report['comparisons']==len(tags)*4 and report['reject']==len(tags)*4-8 and report['abstain']==0
  import shutil
  repeated=root/'repeated-tag-image';repeated.mkdir()
  for item in ('members.i32','gpu-input.json','gpu0.json','gpu1.json','gpu0.observations.u8','gpu1.observations.u8'):
   shutil.copyfile(tag_out/item,repeated/item)
  subprocess.run([tag_exe,'apply',str(data),str(first),str(repeated),str(tag_out)],check=True)
  repeat_report=json.loads((repeated/'summary.json').read_text())
  assert repeat_report['duplicate_observations']==24 and repeat_report['reapplied_observations']==0
  # Native payloads must be byte-identical for an identical-input replay.
  import struct
  def matrices(folder):
   blocks={p.read_bytes()[8:40].hex():p for p in folder.glob('*.block')};result={}
   for entry in map(json.loads,(folder/'tag-records.jsonl').open()):
    if 'tag' not in entry:continue
    a=entry['record']
    with blocks[a['block']].open('rb') as f:f.seek(a['offset']);raw=f.read(a['bytes'])
    ss,so,sm,sc=struct.unpack_from('<QQQQ',raw,32);result[entry['tag']]=raw[64+ss+so+sm:64+ss+so+sm+sc]
   return result
  assert matrices(repeated)==matrices(tag_out)
  # Exercise persisted threshold consumption independently of latest verdict.
  threshold=root/'threshold-previous';shutil.copytree(tag_out,threshold)
  entries=list(map(json.loads,(threshold/'tag-records.jsonl').open()))
  entry=next(x for x in entries if x.get('tag')==red['tag']);a=entry['record']
  blocks={p.read_bytes()[8:40].hex():p for p in threshold.glob('*.block')}
  path=blocks[a['block']]
  with path.open('r+b') as f:
   f.seek(a['offset']);raw=bytearray(f.read(a['bytes']))
   ss,so,sm,sc=struct.unpack_from('<QQQQ',raw,32);start=64+ss+so+sm
   for image,value in [(0,.995),(1,1.0),(3,.995*1.01)]:
    struct.pack_into('<d',raw,start+24+image*16+8,value)
   digest=hashlib.sha256(raw[:-48]).digest();raw[-48:-16]=digest;a['digest']=digest.hex()
   f.seek(a['offset']);f.write(raw)
  (threshold/'tag-records.jsonl').write_text(''.join(json.dumps(x)+'\n' for x in entries))
  threshold_out=root/'threshold-out';threshold_out.mkdir()
  for item in ('members.i32','gpu-input.json','gpu0.json','gpu1.json','gpu0.observations.u8','gpu1.observations.u8'):
   shutil.copyfile(tag_out/item,threshold_out/item)
  subprocess.run([tag_exe,'apply',str(data),str(first),str(threshold_out),str(threshold)],check=True)
  node=next(x for x in map(json.loads,(threshold_out/'concepts.jsonl').open()) if x['tag']==red['tag'])
  assert node['experiences']==[1,2,3] # weak accepted excluded; >=1 rejected included
  assert matrices(threshold_out)==matrices(threshold) # no weak numeric state deleted

  invalid=root/'invalid-tag-image';invalid.mkdir()
  for item in ('members.i32','gpu-input.json','gpu0.json','gpu1.json','gpu0.observations.u8','gpu1.observations.u8'):
   shutil.copyfile(tag_out/item,invalid/item)
  image_path=data/'images'/(features[0]['sha256']+'.bin');original_bytes=image_path.read_bytes()
  try:
   image_path.write_bytes(b'changed original')
   failed=subprocess.run([tag_exe,'apply',str(data),str(first),str(invalid)],capture_output=True,text=True)
   assert failed.returncode!=0 and 'not ready for binary verification' in failed.stderr
   assert not (invalid/'summary.json').exists()
  finally:image_path.write_bytes(original_bytes)
  print('PASS: binary tag-image core decisions, native common-concept read, invalid original fails rather than abstaining')
 # Repeated input with another delivery receipt is still one experience.
 duplicate_receipt=json.loads(json.dumps(receipts[0]));duplicate_receipt['head']['offset']=999
 (data/'features.jsonl').write_text(''.join(json.dumps(x)+'\n' for x in features+[features[0]]))
 (binding/'receipts.jsonl').write_text(''.join(json.dumps(x)+'\n' for x in receipts+[duplicate_receipt]))
 dedup,duplicate_rows,_=build('duplicate','-',17)
 assert duplicate_rows==rows
 summary=json.loads((dedup/'summary.json').read_text())
 assert summary['images']==4 and summary['input_occurrences']==5 and summary['duplicate_occurrences']==1
 aliases=[json.loads(x) for x in (dedup/'aliases.jsonl').read_text().splitlines()]
 assert len(aliases)==5 and aliases[-1]['source']==0 and aliases[-1]['duplicate']
 assert aliases[-1]['receipt']['head']['offset']==999
 subprocess.run([str(recall_exe),str(data),str(dedup),str(query/'cue.json'),str(query/'dedup.json')],check=True)
 duplicate_answer=json.loads((query/'dedup.json').read_text())
 assert duplicate_answer['source_count']==4
 assert {x['tag'] for x in duplicate_answer['associated_tags']}=={'circle','triangle','blue','fish'}
 # Same image but changed experience payload must NOT be merged.
 variant=json.loads(json.dumps(features[0]));variant['tags'].append({'index':999,'tag':'new_context','category':0})
 (data/'features.jsonl').write_text(''.join(json.dumps(x)+'\n' for x in features+[variant]))
 changed,_,_=build('changed-payload','-',17)
 summary=json.loads((changed/'summary.json').read_text())
 assert summary['images']==5 and summary['pairs']==10 and summary['duplicate_occurrences']==0
 print('PASS: six independent pairs; third-input evidence; no-common abstention; per-pair persistence across shuffle; native recall; exact-duplicate invariance and provenance; same-image different-payload retained')
