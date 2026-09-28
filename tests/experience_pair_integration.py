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
 assert {x['tag'] for x in answer['associated_tags']}=={'circle','triangle'}
 assert {x['via_experience'] for x in answer['associated_tags']}=={1,2}
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
 assert {x['tag'] for x in duplicate_answer['associated_tags']}=={'circle','triangle'}
 # Same image but changed experience payload must NOT be merged.
 variant=json.loads(json.dumps(features[0]));variant['tags'].append({'index':999,'tag':'new_context','category':0})
 (data/'features.jsonl').write_text(''.join(json.dumps(x)+'\n' for x in features+[variant]))
 changed,_,_=build('changed-payload','-',17)
 summary=json.loads((changed/'summary.json').read_text())
 assert summary['images']==5 and summary['pairs']==10 and summary['duplicate_occurrences']==0
 print('PASS: six independent pairs; third-input evidence; no-common abstention; per-pair persistence across shuffle; native recall; exact-duplicate invariance and provenance; same-image different-payload retained')
