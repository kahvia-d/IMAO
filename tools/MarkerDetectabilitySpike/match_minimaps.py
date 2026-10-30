"""Offline SIFT candidate retrieval, plus wrong-scene controls."""
import json,sys
from pathlib import Path
import cv2
import numpy as np
sys.stdout.reconfigure(encoding='utf-8')
ROOT=Path(__file__).resolve().parents[2]
OUT=ROOT/'out/detectability-spike-blackshores'
sift=cv2.SIFT_create(nfeatures=18000,contrastThreshold=.01)
refs={}
for state in [8,900]:
    ref=cv2.imread(str(OUT/f'map-{state}.jpg'))
    kp,desc=sift.detectAndCompute(cv2.cvtColor(ref,cv2.COLOR_BGR2GRAY),None)
    refs[state]=(kp,desc)
results=[]
for t,state in [(21,8),(24,8),(60,900),(63,900),(66,900),(105,900),(108,900),(111,900),(114,900),(24,900),(63,8),(114,8)]:
    frame=cv2.imread(str(OUT/f'frame-{t:03d}.jpg'))
    mask=np.zeros(frame.shape[:2],np.uint8)
    cv2.circle(mask,(105,110),91,255,-1);cv2.circle(mask,(105,110),27,0,-1)
    kp,desc=sift.detectAndCompute(cv2.cvtColor(frame,cv2.COLOR_BGR2GRAY),mask)
    kr,dr=refs[state]; pairs=cv2.BFMatcher().knnMatch(desc,dr,k=2)
    good=[a for a,b in pairs if a.distance<.7*b.distance]
    r={'time':t,'stateId':state,'goodMatches':len(good),'acceptedGeometry':False}
    if len(good)<3:results.append(r);print(json.dumps(r));continue
    src=np.float32([kp[m.queryIdx].pt for m in good]);dst=np.float32([kr[m.trainIdx].pt for m in good])
    A,mask=cv2.estimateAffinePartial2D(src,dst,method=cv2.RANSAC,ransacReprojThreshold=2,maxIters=5000)
    if A is None:results.append(r);print(json.dumps(r));continue
    inl=mask.ravel()>0;err=np.linalg.norm(src@A[:,:2].T+A[:,2]-dst,axis=1)
    center=A@np.array([105.,110.,1.]);xmin=3 if state==8 else -1
    rawx=(center[0]*2+xmin*1024-1024)*850/1024*100
    rawy=center[1]*2*850/1024*100
    r.update(inliers=int(inl.sum()),inlierRatio=float(inl.mean()),medianErrorHalfMosaicPixels=float(np.median(err[inl])),rawPosition=[rawx,rawy],affineFrameToHalfMosaic=A.tolist())
    r['acceptedGeometry']=r['inliers']>=12 and r['inlierRatio']>=.5 and r['medianErrorHalfMosaicPixels']<2
    pts=[]
    for g in json.loads((ROOT/f'Assets/KuroMap/states/state-{state}.json').read_text('utf-8-sig')):
        if not g['id'].startswith('qzx'):continue
        for p in g['location']:
            dist=np.hypot(p['x']-rawx,p['y']-rawy)/100
            pts.append({'pointId':p['id'],'typeId':g['id'],'distanceGameUnits':float(dist),'description':p.get('description','')})
    r['nearest']=sorted(pts,key=lambda p:p['distanceGameUnits'])[:4]
    results.append(r);print(json.dumps(r,ensure_ascii=False))
(OUT/'minimap-matches.json').write_text(json.dumps(results,ensure_ascii=False,indent=2),encoding='utf-8')
