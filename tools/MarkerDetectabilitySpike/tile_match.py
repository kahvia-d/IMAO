"""Candidate generator only: accepted geometry is not a reviewed marker identity."""
import json
from pathlib import Path
import cv2,numpy as np
from collection_utils import ROOT,OUT,points

class TileMatcher:
    def __init__(self,state,bounds,generation='B50F4135DCCC4D8DA87ED33CE95EA31D'):
        self.state=state;self.bounds=bounds
        self.sift=cv2.SIFT_create(nfeatures=7000,contrastThreshold=.012)
        cache=OUT/('index-'+str(state)+'-'+'_'.join(map(str,bounds))+'-'+generation+'.npz')
        if cache.exists():
            d=np.load(cache);self.xy=d['xy'];desc=d['desc']
        else:
            coords=[];descs=[]
            xmin,xmax,ymin,ymax=bounds
            for x in range(xmin,xmax+1):
                for y in range(ymin,ymax+1):
                    f=ROOT/f'map-regions/tiles/{generation}/{state}/{state}_{x}_{y}.png'
                    if not f.exists():continue
                    im=cv2.imread(str(f),cv2.IMREAD_GRAYSCALE);im=cv2.resize(im,None,fx=.5,fy=.5)
                    kp,ds=self.sift.detectAndCompute(im,None)
                    if ds is None:continue
                    xy=np.float32([k.pt for k in kp])*2;xy[:,0]+=x*1024;xy[:,1]-=y*1024
                    coords.append(xy);descs.append(ds)
            if not descs:raise ValueError('No tile features')
            self.xy=np.concatenate(coords);desc=np.concatenate(descs)
            np.savez_compressed(cache,xy=self.xy,desc=desc)
        self.matcher=cv2.FlannBasedMatcher(dict(algorithm=1,trees=4),dict(checks=80));self.matcher.add([desc]);self.matcher.train()
        self.points=[p for p in points([state]) if p['typeId'].startswith('qzx') or any(t in p['name'] for t in ['挑战','解谜','玩法','宝箱','潮汐'])]

    def match(self,frame,kind='mini',center=(105,110)):
        mask=np.zeros(frame.shape[:2],np.uint8)
        if kind=='mini':cv2.circle(mask,center,91,255,-1);cv2.circle(mask,center,27,0,-1)
        else:mask[35:430,210:770]=255
        kp,ds=self.sift.detectAndCompute(cv2.cvtColor(frame,cv2.COLOR_BGR2GRAY),mask)
        if ds is None:return {'accepted':False,'good':0}
        pairs=self.matcher.knnMatch(ds,k=2);good=[a for a,b in pairs if a.distance<.7*b.distance]
        r={'stateId':self.state,'kind':kind,'good':len(good),'accepted':False}
        if len(good)<4:return r
        src=np.float32([kp[m.queryIdx].pt for m in good]);dst=self.xy[[m.trainIdx for m in good]]
        A,inliers=cv2.estimateAffinePartial2D(src,dst,method=cv2.RANSAC,ransacReprojThreshold=5,maxIters=5000)
        if A is None:return r
        inl=inliers.ravel()>0;err=np.linalg.norm(src@A[:,:2].T+A[:,2]-dst,axis=1)
        r.update(inliers=int(inl.sum()),ratio=float(inl.mean()),medianError=float(np.median(err[inl])),affine=A.tolist())
        scale=float(np.hypot(A[0,0],A[1,0]));rotation=float(np.degrees(np.arctan2(A[1,0],A[0,0])))
        r.update(scale=scale,rotation=rotation)
        r['accepted']=r['inliers']>=12 and r['ratio']>=.35 and r['medianError']<5 and .1<scale<8 and abs(rotation)<8
        loc=A@np.array([*center,1]);raw=[(loc[0]-1024)*850/1024*100,loc[1]*850/1024*100];r['raw']=raw
        r['nearest']=self.nearest(raw)
        return r

    def nearest(self,raw,n=5):
        ds=sorted(((float(np.hypot(p['x']-raw[0],p['y']-raw[1]))/100,p) for p in self.points),key=lambda x:x[0])
        return [dict(pointId=p['id'],typeId=p['typeId'],name=p['name'],description=p.get('description',''),distance=d) for d,p in ds[:n]]
