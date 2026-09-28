"""Read-only real-data checks. Arguments: executable dataset graph query-directory."""
import json, pathlib, subprocess, sys
exe,data,graph,out=sys.argv[1:];out=pathlib.Path(out)
base=[exe,data,graph,str(out/'cue.json')]
a=json.loads((out/'recall.json').read_text())
assert a['query_sha256']!=a['seed_sha256'] and not a['query_tags_used']
assert a['associated_tags'] and a['traversed_paths']>0
assert not set(a['seed_tags'])&{v['tag'] for v in a['associated_tags']}
subprocess.run(base+[str(out/'disconnected.json'),'--disconnect'],check=True)
b=json.loads((out/'disconnected.json').read_text())
assert a['seed_sha256']==b['seed_sha256'] and a['seed_tags']==b['seed_tags']
assert b['associated_tags']==[] and b['traversed_paths']==0
cue=json.loads((out/'cue.json').read_text())
for kind in ('tag-leak','zero-cue','model-mismatch'):
 bad=dict(cue)
 if kind=='tag-leak':bad['tags']=['injected_tag']
 elif kind=='zero-cue':bad['dino']=[0]*384
 else:bad['dino_sha256']='0'*64
 path=out/(kind+'.json');path.write_text(json.dumps(bad))
 result=subprocess.run([exe,data,graph,str(path),str(out/(kind+'-result.json'))],capture_output=True,text=True)
 assert result.returncode!=0,(kind,result.stdout)
 expected={'tag-leak':'query tag leakage prohibited','zero-cue':'zero cue','model-mismatch':'DINO model mismatch'}[kind]
 assert expected in result.stderr,(kind,result.stderr)
 print(kind+': '+result.stderr.strip())
labels=json.loads((out/'comparison-only.json').read_text())
reference={r['tag'] for r in labels['tags']};extra={r['tag'] for r in a['associated_tags']}
report={'passed':True,'disconnect_removes_associations':True,'rejected_cases':['query_tag_leakage','zero_descriptor','model_mismatch'],'associated_count':len(extra),'reference_count':len(reference),'associated_reference_overlap':sorted(extra&reference),'seed_reference_overlap':sorted(set(a['seed_tags'])&reference),'top20_reference_overlap':sorted({x['tag'] for x in a['associated_tags'][:20]}&reference),'comparison_is_model_output_not_ground_truth':True}
(out/'checks.json').write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n');print(json.dumps(report,ensure_ascii=False))
