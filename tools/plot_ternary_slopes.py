"""Plot saved observations only; no judgments or thresholds are introduced."""
import pathlib,json,csv,sys
import numpy as np
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from matplotlib import font_manager
root=pathlib.Path(sys.argv[1]);font=pathlib.Path('/home/raspie/.local/share/fonts/NotoSansCJK-Regular.ttc')
if font.exists():font_manager.fontManager.addfont(str(font));plt.rcParams['font.family']=font_manager.FontProperties(fname=str(font)).get_name()
plt.rcParams['axes.unicode_minus']=False
rows=list(csv.DictReader((root/'experience-slopes.csv').open()));cols=['accept_per_round','reject_per_round','abstain_per_round','net_per_round'];labels=['승인 증가 기울기','반려 증가 기울기','기권 증가 기울기','순지지 기울기 (승인−반려)'];colors=['#167548','#b64838','#727c88','#375fc0']
fig,axes=plt.subplots(2,2,figsize=(13,8),layout='constrained');summary={}
for ax,key,title,color in zip(axes.flat,cols,labels,colors):
 a=np.array([float(r[key]) for r in rows]);ax.hist(a,bins=50 if a.min()!=a.max() else [a[0]-.5,a[0]+.5],color=color,alpha=.85);ax.set(title=title,xlabel='횟수 / VRS 회차',ylabel='원경험 수');ax.grid(alpha=.15)
 summary[key]={'min':float(a.min()),'p25':float(np.quantile(a,.25)),'median':float(np.median(a)),'p75':float(np.quantile(a,.75)),'max':float(a.max()),'mean':float(a.mean())}
fig.suptitle(f'원경험 {len(rows):,}개: 10회 정수 3상 누적의 평균 증가 기울기',fontsize=15)
fig.savefig(root/'experience-slopes.png',dpi=150);fig.savefig(root/'experience-slopes.svg');plt.close(fig)
h=json.loads((root/'connection-slope-histograms.json').read_text());fig,axes=plt.subplots(2,2,figsize=(13,8),layout='constrained')
for ax,axis,title in zip(axes.flat,range(4),labels):
 for kind,color,label in [('tag_image','#27845f','태그–이미지'),('tag_tag','#5b65bb','태그–태그')]:
  d=h['delta_histograms'][kind][axis];x=np.array([int(k)/h['rounds'] for k in d]);y=np.array(list(d.values()));order=np.argsort(x);ax.plot(x[order],y[order],'.',ms=3,color=color,label=label,alpha=.7)
 ax.set(xscale='symlog',yscale='log',title=title,xlabel='횟수 / 회차 (대칭 로그 축)',ylabel='해당 기울기의 연결 수 (로그 축)');ax.legend();ax.grid(alpha=.15)
fig.suptitle('모든 시냅스 연결의 증가 기울기 분포 — 0 증가 연결 포함',fontsize=15);fig.savefig(root/'connection-slopes.png',dpi=150);fig.savefig(root/'connection-slopes.svg');plt.close(fig)
(root/'slope-summary.json').write_text(json.dumps(summary,ensure_ascii=False,indent=2)+'\n');print(json.dumps(summary,ensure_ascii=False))
