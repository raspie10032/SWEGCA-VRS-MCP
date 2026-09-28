"""Existing local model adapter only; no VRS decisions or new dependencies.
Writes the DINO cue separately from WD14 comparison labels.
"""
import argparse, hashlib, json, io, sys
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('image',type=Path);p.add_argument('output',type=Path);a=p.parse_args()
import numpy as np
import torch
from PIL import Image
from transformers import AutoImageProcessor, AutoModel
base=Path('/var/home/raspie/Documents/Codex/tinylm-slicer-sanabi-bazzite')
dino=Path('/run/media/raspie/4TB SATA/Rozephine_Observations/p3_g2_prerequisites_20260818/dinov2-small-ed25f3a')
torch.set_num_threads(2);torch.set_num_interop_threads(1)
raw=a.image.read_bytes();sha=hashlib.sha256(raw).hexdigest()
with Image.open(io.BytesIO(raw)) as im:
 rgba=im.convert('RGBA');canvas=Image.new('RGBA',rgba.size,(255,255,255,255));canvas.alpha_composite(rgba);rgb=canvas.convert('RGB')
processor=AutoImageProcessor.from_pretrained(dino,local_files_only=True)
model=AutoModel.from_pretrained(dino,local_files_only=True).eval().to('cuda:1')
inputs={k:v.to('cuda:1') for k,v in processor(images=[rgb],return_tensors='pt').items()}
with torch.inference_mode(): vector=torch.nn.functional.normalize(model(**inputs).last_hidden_state[:,0].float(),dim=-1).cpu().numpy()[0]
if not np.isfinite(vector).all():raise ValueError('nonfinite observation')
a.output.mkdir(parents=True,exist_ok=True)
(a.output/'input.bin').write_bytes(raw)
(a.output/'cue.json').write_text(json.dumps({'image_sha256':sha,'image':str(a.image),'dino_sha256':hashlib.sha256((dino/'model.safetensors').read_bytes()).hexdigest(),'dino':vector.tolist()}))
print('DINO cue written; no query tags supplied to recall',flush=True)
# Independent comparison only, never included in cue.json.
del model;torch.cuda.empty_cache()
sys.path.append(str(base/'.runtime-rozephine-aiart-wd14-prompt-filter-20260904-run001/venv/lib/python3.14/site-packages'))
import onnxruntime as ort
import csv
lib=base/'.runtime-rozephine-python314-torch214-cu132-20260903/venv/lib/python3.14/site-packages/nvidia'
ort.preload_dlls(cuda=True,cudnn=False,directory=str(lib/'cu13/lib'));ort.preload_dlls(cuda=False,cudnn=True,directory=str(lib/'cudnn/lib'))
tag=base/'.runtime-rozephine-aiart-wd14-prompt-filter-20260904-run001/model'
options=ort.SessionOptions();options.intra_op_num_threads=2;options.inter_op_num_threads=1
session=ort.InferenceSession(str(tag/'model.onnx'),sess_options=options,providers=[('CUDAExecutionProvider',{'device_id':1}),'CPUExecutionProvider'])
size=session.get_inputs()[0].shape[1];side=max(rgb.size);square=Image.new('RGB',(side,side),'white');square.paste(rgb,((side-rgb.width)//2,(side-rgb.height)//2))
array=np.asarray(square.resize((size,size),Image.Resampling.BICUBIC),dtype=np.float32)[:,:,::-1].copy()
probs=session.run(None,{session.get_inputs()[0].name:array[None]})[0][0]
with (tag/'selected_tags.csv').open() as f:labels=list(csv.DictReader(f))
(a.output/'comparison-only.json').write_text(json.dumps({'not_ground_truth':True,'not_used_for_recall':True,'tags':[{'tag':labels[i]['name'],'score':float(v)} for i,v in enumerate(probs) if v>=0.2614]}))
print('Comparison labels written separately',flush=True)
