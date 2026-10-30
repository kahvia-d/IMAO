"""Disposable offline experiment; does not modify runtime resources."""
import json
from pathlib import Path
import cv2
import numpy as np

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / 'out/detectability-spike-blackshores'
ARCHIVE = ROOT / 'map-regions/tiles/B50F4135DCCC4D8DA87ED33CE95EA31D'

def mosaic(state, xmin, xmax, ymin, ymax):
    canvas = np.zeros(((ymax-ymin+1)*1024,(xmax-xmin+1)*1024,3), np.uint8)
    for x in range(xmin,xmax+1):
        for y in range(ymin,ymax+1):
            f=ARCHIVE/str(state)/f'{state}_{x}_{y}.png'
            if f.exists():
                im=cv2.imread(str(f));canvas[(ymax-y)*1024:(ymax-y+1)*1024,(x-xmin)*1024:(x-xmin+1)*1024]=im[:,:,:3]
    return canvas

results=[]
for state, bounds, times in [(8,(3,5,-2,0),[9,27]),(900,(-1,1,-2,0),[30,33,69])]:
    xmin,xmax,ymin,ymax=bounds
    big=mosaic(state,*bounds)
    # Map image matching, independent of any selected marker ID or guide text.
    ref=cv2.resize(big,None,fx=.5,fy=.5)
    cv2.imwrite(str(OUT/f'map-{state}.jpg'),ref)
    sift=cv2.SIFT_create(nfeatures=18000,contrastThreshold=.015)
    kr,dr=sift.detectAndCompute(cv2.cvtColor(ref,cv2.COLOR_BGR2GRAY),None)
    for t in times:
        frame=cv2.imread(str(OUT/f'frame-{t:03d}.jpg'))
        mask=np.zeros(frame.shape[:2],np.uint8);mask[35:430,210:770]=255
        kq,dq=sift.detectAndCompute(cv2.cvtColor(frame,cv2.COLOR_BGR2GRAY),mask)
        pairs=cv2.BFMatcher().knnMatch(dq,dr,k=2)
        good=[a for a,b in pairs if a.distance < .7*b.distance]
        src=np.float32([kq[m.queryIdx].pt for m in good]);dst=np.float32([kr[m.trainIdx].pt for m in good])
        A,inliers=cv2.estimateAffinePartial2D(src,dst,method=cv2.RANSAC,ransacReprojThreshold=3,maxIters=5000)
        if A is None: print(state,t,'failed');continue
        errors=np.linalg.norm(src@A[:,:2].T+A[:,2]-dst,axis=1)
        result={'stateId':state,'time':t,'goodMatches':len(good),'inliers':int(inliers.sum()),'medianErrorPixels':float(np.median(errors[inliers.ravel()>0])),'affineFrameToHalfMosaic':A.tolist(),'bounds':bounds}
        results.append(result); print(json.dumps(result))
        overlay=frame.copy()
        inv=cv2.invertAffineTransform(A)
        for group in json.loads((ROOT/f'Assets/KuroMap/states/state-{state}.json').read_text('utf-8-sig')):
            if not group['id'].startswith('qzx'):continue
            for p in group['location']:
                # Kuro raw point units /100 -> game -> tile pixel frame.
                px=(p['x']/100/850+1)*1024
                py=(-p['y']/100/850)*1024
                local=np.array([(px-xmin*1024)*.5,(ymax*1024-py)*.5,1.])
                x,y=inv@local
                if 0<=x<852 and 0<=y<430:
                    xy=(round(x),round(y));cv2.circle(overlay,xy,5,(0,0,255),1)
                    cv2.putText(overlay,p['id'][-6:],xy,cv2.FONT_HERSHEY_SIMPLEX,.3,(0,255,255),1)
        cv2.imwrite(str(OUT/f'overlay-{t:03d}.jpg'),overlay)
        drawn=cv2.drawMatches(frame,kq,ref,kr,good,None,matchesMask=inliers.ravel().tolist(),flags=cv2.DrawMatchesFlags_NOT_DRAW_SINGLE_POINTS)
        cv2.imwrite(str(OUT/f'matches-{t:03d}.jpg'),drawn)
(OUT/'map-matches.json').write_text(json.dumps(results,indent=2),encoding='utf-8')
