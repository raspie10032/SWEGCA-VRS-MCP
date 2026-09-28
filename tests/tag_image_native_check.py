import pathlib,json,struct,hashlib,collections,sys
root=pathlib.Path(sys.argv[1]);graph=pathlib.Path(sys.argv[2])
tags={r['id']:r for r in map(json.loads,(graph/'tags.jsonl').open())};images={r['source']:r['image_sha256'] for r in map(json.loads,(graph/'sources.jsonl').open())}
blocks={}
for p in root.glob('tag-image-*.block'):
 f=p.open('rb');h=f.read(80);assert h[:8]==b'SWGCBLK1' and hashlib.sha256(h[:48]).digest()==h[48:80];blocks[h[8:40].hex()]=f
n=len(images);counts=collections.Counter();concept_count=0;matrices=0;eligible_count=0;ineligible_count=0
for row in map(json.loads,(root/'tag-records.jsonl').open()):
 a=row['record'];f=blocks[a['block']];f.seek(a['offset']);raw=f.read(a['bytes']);assert len(raw)==a['bytes']
 assert raw[:8]==b'SWGCEXP1' and hashlib.sha256(raw[:-48]).hexdigest()==a['digest']
 assert raw[-48:-16].hex()==a['digest'] and raw[-16:-8]==b'SWGCEND1'
 ss,so,sm,sc=struct.unpack_from('<QQQQ',raw,32);payload=raw[64+ss+so+sm:64+ss+so+sm+sc]
 if 'tag' in row:
  tag,rows,version=struct.unpack_from('<QQQ',payload);assert tag==row['tag'] and rows==n and version==2
  assert len(payload)==24+n*16;expected=set(tags[tag]['witnesses'])
  for source,(status,strength) in enumerate(struct.iter_unpack('<Qd',payload[24:])):
   assert status==(1 if source in expected else 2)
   assert strength==(1.01 if source in expected else 0.995);counts[status]+=1
   eligible_count+=strength>=1;ineligible_count+=strength<1
  matrices+=1
 else:
  node=json.loads(payload);assert node['tag']==row['concept'];expected=tags[node['tag']]['witnesses'];assert node['experiences']==expected
  distinct=len({images[i] for i in expected});assert node['distinct_images']==distinct
  assert node['role']==('common_concept' if distinct>1 else 'single_observation');concept_count+=distinct>1
s=json.loads((root/'summary.json').read_text());assert sum(counts.values())==s['comparisons'] and counts[1]==s['accept'] and counts[2]==s['reject'] and counts[0]==s['abstain']==0;assert matrices==len(tags) and concept_count==s['common_concepts']
if 'eligible_connections' in s:
 assert s['eligible_connections']==eligible_count and s['retained_ineligible_connections']==ineligible_count
 assert s['duplicate_observations']==s['comparisons'] and s['reapplied_observations']==0
if len(sys.argv)>3:
 previous=pathlib.Path(sys.argv[3]);old_blocks={}
 for p in previous.glob('*.block'):
  f=p.open('rb');h=f.read(80);old_blocks[h[8:40].hex()]=f
 old_rows={r['tag']:r['record'] for r in map(json.loads,(previous/'tag-records.jsonl').open()) if 'tag' in r}
 def content(files,a):
  f=files[a['block']];f.seek(a['offset']);raw=f.read(a['bytes'])
  assert hashlib.sha256(raw[:-48]).hexdigest()==a['digest']
  ss,so,sm,sc=struct.unpack_from('<QQQQ',raw,32)
  return raw[64+ss+so+sm:64+ss+so+sm+sc]
 for entry in map(json.loads,(root/'tag-records.jsonl').open()):
  if 'tag' in entry:assert content(blocks,entry['record'])==content(old_blocks,old_rows[entry['tag']])
a=json.loads((root/'gpu0.json').read_text());b=json.loads((root/'gpu1.json').read_text());overlap=min(a['finished_unix'],b['finished_unix'])-max(a['started_unix'],b['started_unix']);assert overlap>0
report={'passed':True,'native_matrices':matrices,'all_cell_statuses_and_strengths_checked':sum(counts.values()),'common_concepts':concept_count,'eligible_connections':eligible_count,'retained_ineligible_connections':ineligible_count,'previous_matrices_byte_identical':len(sys.argv)>3,'gpu_worker_overlap_seconds':overlap}
(root/'check.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report))
