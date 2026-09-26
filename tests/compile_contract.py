#!/usr/bin/env python3
"""Require forbidden arithmetic and fabricated judgments to fail at compile time."""
import os,pathlib,shlex,subprocess,tempfile
root=pathlib.Path(__file__).resolve().parents[1]
compiler=shlex.split(os.environ.get('CXX','c++'))
include='#include "swegca_architecture/memory_promotion_kernel.hpp"\nusing namespace swegca::architecture::kernel;\n'
cases=[
 ('fast-math',include+'int main() {}',['-ffast-math'],'SWEGCA core requires strict'),
 ('finite-only',include+'int main() {}',['-ffinite-math-only'],'SWEGCA core requires strict'),
 ('reciprocal',include+'int main() {}',['-freciprocal-math'],'SWEGCA core requires strict'),
 ('unsigned-zero',include+'int main() {}',['-fno-signed-zeros'],'SWEGCA core requires strict'),
 ('reassociation',include+'int main() {}',['-fassociative-math','-fno-signed-zeros','-fno-trapping-math'],'SWEGCA core requires strict'),
 ('private-field',include+'int main(){EvidenceJudgment j; j.status_=EvidenceStatus::accept;} ',[],'private'),
 ('result-rewrite',include+'int main(){EvidenceJudgment j; j.status()=EvidenceStatus::accept;}',[],None),
 ('aggregate-forge',include+'int main(){EvidenceJudgment j{EvidenceStatus::accept};}',[],None),
 ('raw-status-bypass',include+'int main(){MemoryPromotionDecision out; return decide_memory_promotion(MemoryTier::episodic,EvidenceStatus::accept,EvidenceReason::causal_lower_bound,true,true,out);}',[],None),
]
with tempfile.TemporaryDirectory(prefix='swegca-compile-contract-') as directory:
 path=pathlib.Path(directory)/'probe.cpp'
 for name,source,flags,diagnostic in [('strict-positive',include+'int main(){EvidenceJudgment j;return int(j.status());}',[],None)]+cases:
  path.write_text(source)
  r=subprocess.run(compiler+['-std=c++20','-ffp-contract=off','-I'+str(root/'cpp')]+flags+['-fsyntax-only',str(path)],capture_output=True,text=True)
  if name=='strict-positive':
   if r.returncode:raise RuntimeError(r.stderr)
  else:
   if r.returncode==0:raise RuntimeError(name+' unexpectedly compiled')
   if diagnostic and diagnostic not in r.stderr:raise RuntimeError(name+' failed for a different reason: '+r.stderr)
  print('PASS:',name)
