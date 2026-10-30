"""Read-only terrain proposals and visual crosscheck, never auto-accept."""
import sys,json,cv2,numpy as np
from pathlib import Path
from localize import load_reference,project
from route_follow import minimap,terrain_candidates
part=int(sys.argv[1]);times=list(map(int,sys.argv[2:]));out=Path('out/detectability-v2/route-huanglong-late')
ref=load_reference(dict(manifest='Assets/FeaturesDatas/KuroTilePacks/jinzhou/manifest.json',bounds=[-1,1,-3,-1],scale=.5))
v=cv2.VideoCapture(f'out/detectability-collection/BV1iMTXzVE8J/p{part:02}/video.mp4');rows=[];panels=[]
for t in times:
 v.set(cv2.CAP_PROP_POS_MSEC,t*1000);ok,f=v.read()
 patch,mask,roi=minimap(f);peaks=terrain_candidates(patch,mask,ref['image'],scale=.5,count=3)
 for p in peaks:p['rawPosition']=(project(ref['pixelToGame'],p['centerPixel'])*100).tolist()
 rows.append(dict(time=t,roi=roi,peaks=peaks));print(t,[(round(p['score'],2),[round(x) for x in p['rawPosition']]) for p in peaks])
 x,y=map(int,peaks[0]['centerPixel']);crop=ref['image'][y-55:y+55,x-55:x+55];pair=np.hstack([cv2.resize(patch,(110,110)),cv2.resize(crop,(110,110))]);pair=cv2.resize(pair,(440,220));cv2.putText(pair,str(t),(4,22),0,.6,(0,0,255),2);panels.append(pair)
for i in range(0,len(panels),9):
 pp=panels[i:i+9];pp += [np.zeros_like(pp[0])]*(9-len(pp));cv2.imwrite(str(out/f'p{part}-probe-{times[0]}-{i}.jpg'),np.vstack([np.hstack(pp[j:j+3]) for j in (0,3,6)]))
(out/f'p{part}-probe-{times[0]}.json').write_text(json.dumps(rows,ensure_ascii=False,indent=2),encoding='utf-8')
