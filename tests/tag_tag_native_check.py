"""Independent all-pairs oracle from original tag membership references."""
import json,struct,hashlib,pathlib,sys,collections
root=pathlib.Path(sys.argv[1]);graph=pathlib.Path(sys.argv[2])
tags=list(map(json.loads,(graph/'tags.jsonl').open()));tags.sort(key=lambda t:t['id'])
bits=[sum(1<<i for i in set(t['witnesses'])) for t in tags]
blocks={}
for p in root.glob('tag-tag-*.block'):
 f=p.open('rb');h=f.read(80);assert h[:8]==b'SWGCBLK1' and hashlib.sha256(h[:48]).digest()==h[48:];blocks[h[8:40].hex()]=f
counts=collections.Counter();expected_left=0;expected_right=1;total=0;examples={1:[],2:[]}
for item in map(json.loads,(root/'pairs.jsonl').open()):
 a=item['record'];f=blocks[a['block']];f.seek(a['offset']);raw=f.read(a['bytes'])
 assert len(raw)==a['bytes'] and hashlib.sha256(raw[:-48]).hexdigest()==a['digest']
 assert raw[-48:-16].hex()==a['digest'] and raw[-16:-8]==b'SWGCEND1'
 ss,so,sm,sc=struct.unpack_from('<QQQQ',raw,32);payload=raw[64+ss+so+sm:64+ss+so+sm+sc]
 assert len(payload)==item['pairs']*40
 for left,right,witnesses,status,strength in struct.iter_unpack('<QQQQd',payload):
  assert (left,right)==(expected_left,expected_right)
  expected=(bits[left]&bits[right]).bit_count();assert witnesses==expected
  assert status==(1 if expected else 2) and strength==(1.01 if expected else .995)
  if len(examples[status])<4:examples[status].append({'left':tags[left]['tag'],'right':tags[right]['tag'],'witnesses':witnesses,'strength':strength})
  counts[status]+=1;total+=1;expected_right+=1
  if expected_right==len(tags):expected_left+=1;expected_right=expected_left+1
s=json.loads((root/'summary.json').read_text())
assert total==len(tags)*(len(tags)-1)//2==s['pairs']
assert counts[1]==s['accept']==s['eligible'] and counts[2]==s['reject']==s['retained_ineligible'] and s['abstain']==0
report={'passed':True,'every_pair_checked':total,'accept':counts[1],'reject':counts[2],'abstain':0,'examples':examples}
(root/'check.json').write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n');print(json.dumps(report,ensure_ascii=False))
