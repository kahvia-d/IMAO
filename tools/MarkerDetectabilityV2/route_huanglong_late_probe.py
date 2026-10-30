"""Independent P12 route evidence; does not modify timelines."""
import json, sys
from pathlib import Path
import cv2,numpy as np
from localize import load_reference,project,save_image,match_reference
ROOT=Path(__file__).resolve().parents[2]
OUT=ROOT/'out/detectability-v2/route-huanglong-late';OUT.mkdir(exist_ok=True)
ref=load_reference(dict(manifest='Assets/FeaturesDatas/KuroTilePacks/jinzhou/manifest.json',bounds=[1,2,-4,-3],scale=.5))
cap=cv2.VideoCapture(str(ROOT/'out/detectability-collection/BV1iMTXzVE8J/p12/video.mp4'))
def frame(t):
 cap.set(cv2.CAP_PROP_POS_MSEC,t*1000);ok,f=cap.read();assert ok
 return cv2.resize(f,(852,480))
f=frame(510)
save_image(OUT/'p12-510.jpg',f)
mask=np.zeros(f.shape[:2],np.uint8);mask[40:440,180:820]=255
roi=dict(center=[426,240],radius=300,player=[426,241])
r,d=match_reference(f,roi,mask,ref)
(OUT/'p12-510-fit.json').write_text(json.dumps(r,ensure_ascii=False,indent=2),encoding='utf-8')
if d is not None:save_image(OUT/'p12-510-fit.jpg',d)
print({k:v for k,v in r.items() if k in ['acceptedGeometry','affineFrameToReference','inliers','gamePosition','rawPosition']})
save_image(OUT/'p12-local-mosaic.jpg',ref['image'])
if '--terrain' in sys.argv or '--early' in sys.argv or '--third' in sys.argv:
 from route_follow import minimap
 hp=lambda x:x.astype(np.float32)-cv2.GaussianBlur(x.astype(np.float32),(0,0),3.5)
 target=hp(cv2.cvtColor(ref['image'],cv2.COLOR_BGR2GRAY))
 rows=[];panels=[]
 times=range(10,86,4) if '--early' in sys.argv else range(385,510,5)
 prefix='p12-e1' if '--early' in sys.argv else 'p12'
 if '--third' in sys.argv:times=range(183,291,6);prefix='p12-e3'
 for t in times:
  try:patch,mask,meta=minimap(frame(t))
  except ValueError as ex:rows.append(dict(time=t,error=str(ex)));continue
  small=cv2.resize(patch,None,fx=.5,fy=.5);ms=cv2.resize(mask,None,fx=.5,fy=.5,interpolation=cv2.INTER_NEAREST)
  corr=cv2.matchTemplate(target,hp(cv2.cvtColor(small,cv2.COLOR_BGR2GRAY)),cv2.TM_CCORR_NORMED,mask=ms)
  corr[~np.isfinite(corr)]=-1
  peaks=[]
  for _ in range(4):
   _,val,_,xy=cv2.minMaxLoc(corr);px=np.array(xy)+55
   peaks.append(dict(score=val,pixel=px.tolist(),rawPosition=(project(ref['pixelToGame'],px)*100).tolist()))
   cv2.circle(corr,xy,35,-1,-1)
  rows.append(dict(time=t,roi=meta,peaks=peaks))
  px,py=peaks[0]['pixel'];crop=ref['image'][py-55:py+55,px-55:px+55]
  pair=np.hstack([small,crop]);pair=cv2.resize(pair,(440,220));cv2.putText(pair,f'{t} {peaks[0]["score"]:.2f}',(4,20),0,.6,(0,0,255),2);panels.append(pair)
 for i in range(0,len(panels),9):
  pp=panels[i:i+9];pp+=[np.zeros_like(pp[0])]*(9-len(pp));save_image(OUT/f'{prefix}-terrain-{i}.jpg',np.vstack([np.hstack(pp[j:j+3]) for j in (0,3,6)]))
 (OUT/f'{prefix}-terrain.json').write_text(json.dumps(rows,ensure_ascii=False,indent=2),encoding='utf-8')
if '--track' in sys.argv:
 results=[]
 for t in range(385,510,2):
  f=frame(t);mask=np.zeros(f.shape[:2],np.uint8)
  cv2.circle(mask,(106,104),88,255,-1);cv2.circle(mask,(106,104),23,0,-1)
  rr,dd=match_reference(f,dict(center=[106,104],radius=101,player=[106,104]),mask,ref)
  results.append(dict(time=t,**rr))
  print(t,rr.get('acceptedGeometry'),rr.get('inliers'),rr.get('rawPosition'),flush=True)
 (OUT/'p12-minimap-probes.json').write_text(json.dumps(results,ensure_ascii=False,indent=2),encoding='utf-8')
