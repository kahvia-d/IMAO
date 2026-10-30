import json
from pathlib import Path
import cv2,numpy as np
from localize import load_reference,match_reference,project,save_image
from route_follow import minimap
ROOT=Path(__file__).resolve().parents[2];OUT=ROOT/'out/detectability-v2/route-huanglong-late'
ref=load_reference(dict(manifest='Assets/FeaturesDatas/KuroTilePacks/jinzhou/manifest.json',bounds=[-1,1,-2,0],scale=.5))
cap=cv2.VideoCapture(str(ROOT/'out/detectability-collection/BV1iMTXzVE8J/p07/video.mp4'))
def frame(t):
 cap.set(cv2.CAP_PROP_POS_MSEC,t*1000);ok,f=cap.read();assert ok;return cv2.resize(f,(852,480))
f=frame(216);mask=np.zeros(f.shape[:2],np.uint8);mask[15:440,200:560]=255
r,d=match_reference(f,dict(center=[426,240],radius=300,player=[426,240]),mask,ref)
(OUT/'p7-216-facility-fit.json').write_text(json.dumps(r,ensure_ascii=False,indent=2),encoding='utf-8');print(r.get('rawPosition'))
hp=lambda x:x.astype(np.float32)-cv2.GaussianBlur(x.astype(np.float32),(0,0),3.5)
target=hp(cv2.cvtColor(ref['image'],cv2.COLOR_BGR2GRAY));rows=[];panels=[]
for t in range(222,271,4):
 try:patch,mask,meta=minimap(frame(t))
 except ValueError as ex:rows.append(dict(time=t,error=str(ex)));continue
 # This part's radar decoration sits southeast, not centered at the south edge.
 cv2.rectangle(mask,(142,130),(220,220),0,-1)
 hsv=cv2.cvtColor(patch,cv2.COLOR_BGR2HSV);white=((hsv[:,:,1]<70)&(hsv[:,:,2]>145)).astype(np.uint8)*255
 mask[cv2.dilate(white,np.ones((7,7),np.uint8))>0]=0
 small=cv2.resize(patch,None,fx=.5,fy=.5);ms=cv2.resize(mask,None,fx=.5,fy=.5,interpolation=cv2.INTER_NEAREST)
 corr=cv2.matchTemplate(target,hp(cv2.cvtColor(small,cv2.COLOR_BGR2GRAY)),cv2.TM_CCORR_NORMED,mask=ms);corr[~np.isfinite(corr)]=-1
 # Independent 216s facility anchor plus uninterrupted short travel bounds region.
 yy,xx=np.indices(corr.shape);rawx=(xx+55)*ref['pixelToGame'][0,0]*100+ref['pixelToGame'][0,2]*100;rawy=(yy+55)*ref['pixelToGame'][1,1]*100+ref['pixelToGame'][1,2]*100
 corr[(rawx<-100000)|(rawx>-70000)|(rawy<100000)|(rawy>135000)]=-1
 peaks=[]
 for _ in range(4):
  _,val,_,xy=cv2.minMaxLoc(corr);px=np.array(xy)+55;peaks.append(dict(score=val,pixel=px.tolist(),rawPosition=(project(ref['pixelToGame'],px)*100).tolist()));cv2.circle(corr,xy,35,-1,-1)
 rows.append(dict(time=t,roi=meta,peaks=peaks));px,py=peaks[0]['pixel'];crop=ref['image'][py-55:py+55,px-55:px+55];pair=cv2.resize(np.hstack([small,crop]),(440,220));cv2.putText(pair,f'{t} {peaks[0]["score"]:.2f}',(4,20),0,.6,(0,0,255),2);panels.append(pair)
for i in range(0,len(panels),9):
 pp=panels[i:i+9];pp+=[np.zeros_like(pp[0])]*(9-len(pp));save_image(OUT/f'p7-e5e6-terrain-{i}.jpg',np.vstack([np.hstack(pp[j:j+3]) for j in (0,3,6)]))
(OUT/'p7-e5e6-terrain.json').write_text(json.dumps(rows,ensure_ascii=False,indent=2),encoding='utf-8')
print([(r['time'],r.get('peaks',[{}])[0].get('rawPosition')) for r in rows])
