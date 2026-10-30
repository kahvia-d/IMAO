"""North-up terrain odometry. Relative tracks are NOT absolute point identities.

Each failed edge/cut starts a new component. No bridge over a gap is invented.
Moving editor minimap insets are normalized before matching. Independent map
anchors and reviewed route/guide correspondence are required for confirmation.
"""
import argparse
import json
import math
from pathlib import Path
import cv2
import numpy as np
import localize as L


def terrain_candidates(query, mask, atlas, scale=1., count=3):
    """North-up proposals only; score is not confidence or identity proof.

    The atlas must include a margin around the expected position. Preserve
    alternate spatial peaks for route checks instead of selecting nearest ID.
    """
    def highpass(im):
        if im.ndim==3:im=cv2.cvtColor(im,cv2.COLOR_BGR2GRAY)
        gray=im.astype(np.float32)
        return gray-cv2.GaussianBlur(gray,(0,0),7)
    q=highpass(query);a=highpass(atlas)
    if scale!=1:
        q=cv2.resize(q,None,fx=scale,fy=scale)
        mask=cv2.resize(mask,q.shape[::-1],interpolation=cv2.INTER_NEAREST)
    if a.shape[0]<q.shape[0] or a.shape[1]<q.shape[1] or np.count_nonzero(mask)<100:return []
    scores=cv2.matchTemplate(a,q,cv2.TM_CCORR_NORMED,mask=mask)
    scores[~np.isfinite(scores)]=-1
    results=[]
    for _ in range(count):
        _,value,_,xy=cv2.minMaxLoc(scores)
        if value<=-1:break
        center=np.asarray(xy)+np.array([q.shape[1],q.shape[0]])/2
        results.append({'score':value,'centerPixel':center.tolist(),'scale':scale,'status':'proposal-requires-route-and-visual-review'})
        cv2.circle(scores,xy,round(min(q.shape)*.32),-1,-1)
    return results


def displacement(a, b, ma, mb):
    sift=cv2.SIFT_create(nfeatures=1800,contrastThreshold=.012)
    ka,da=sift.detectAndCompute(a,ma);kb,db=sift.detectAndCompute(b,mb)
    result={'accepted':False,'reason':'insufficient-terrain-features'}
    if da is None or db is None or len(db)<2:return result
    pairs=cv2.BFMatcher().knnMatch(da,db,k=2)
    unique={}
    for pair in pairs:
        if len(pair)==2 and pair[0].distance<.72*pair[1].distance:
            m=pair[0]
            if m.trainIdx not in unique or m.distance<unique[m.trainIdx].distance:unique[m.trainIdx]=m
    matches=list(unique.values());result['matches']=len(matches)
    if len(matches)<10:return result
    src=np.float32([ka[m.queryIdx].pt for m in matches]);dst=np.float32([kb[m.trainIdx].pt for m in matches])
    matrix,mask=cv2.estimateAffinePartial2D(src,dst,method=cv2.RANSAC,ransacReprojThreshold=1.5,maxIters=3000,confidence=.999)
    if matrix is None:return result
    good=mask.ravel().astype(bool);n=int(good.sum())
    scale=float(np.hypot(matrix[0,0],matrix[1,0]));angle=math.degrees(math.atan2(matrix[1,0],matrix[0,0]))
    delta=np.median(dst[good]-src[good],axis=0)
    errors=np.linalg.norm(dst[good]-src[good]-delta,axis=1)
    hull=float(cv2.contourArea(cv2.convexHull(src[good]))/(a.shape[0]*a.shape[1]))
    result.update(inliers=n,inlierRatio=n/len(matches),scale=scale,rotation=angle,hullFraction=hull,
                  p90Error=float(np.percentile(errors,90)),playerDelta=(-delta).tolist())
    checks=[(n<10,'few-inliers'),(n/len(matches)<.65,'low-consensus'),(abs(scale-1)>.025,'zoom-change'),
            (abs(angle)>1.,'orientation-change'),(result['p90Error']>1.8,'nontranslation'),
            (hull<.035,'concentrated-features'),(np.linalg.norm(delta)>85,'large-discontinuity')]
    reasons=[r for bad,r in checks if bad];result.update(accepted=not reasons,reason=';'.join(reasons) or 'terrain-translation')
    return result


def integrate(rows):
    segment=-1;position=np.zeros(2);out=[]
    for r in rows:
        if segment<0 or r.get('cutBefore') or not r.get('accepted'):
            segment+=1;position=np.zeros(2)
        else:position+=np.asarray(r['playerDelta'])
        out.append(dict(r,segment=segment,relativePixel=position.tolist()))
    return out


def minimap(frame):
    """Find enlarged inset; accept clipped circles, reject ambiguous/playerless UI."""
    h,w=frame.shape[:2];region=frame[:round(h*.57),:round(w*.34)]
    hsv=cv2.cvtColor(region,cv2.COLOR_BGR2HSV)
    yellow=cv2.inRange(hsv,np.array([18,100,160]),np.array([42,255,255]))
    n,labels,stats,centers=cv2.connectedComponentsWithStats(yellow)
    arrows=[i for i in range(1,n) if 150*(h/480)**2<stats[i,4]<1400*(h/480)**2]
    circles=cv2.HoughCircles(cv2.cvtColor(region,cv2.COLOR_BGR2GRAY),cv2.HOUGH_GRADIENT,1.2,25*h/480,
        param1=100,param2=28,minRadius=round(h*.14),maxRadius=round(h*.25))
    if circles is None or not arrows:raise ValueError('no-enlarged-map-and-arrow')
    options=[]
    for c in circles[0]:
        for i in arrows:
            distance=np.linalg.norm(c[:2]-centers[i])
            if distance<c[2]*.13:options.append((distance,c,centers[i]))
    if not options:raise ValueError('map-arrow-centre-mismatch')
    options.sort(key=lambda x:x[0]);_,circle,arrow=options[0];cx,cy,r=map(float,circle)
    # Keep native pixels (no radius-based rescaling; Hough radius jitter is not real zoom).
    size=round(h*220/480);half=size/2
    matrix=np.array([[1,0,half-cx],[0,1,half-cy]],np.float32)
    patch=cv2.warpAffine(frame,matrix,(size,size))
    valid=cv2.warpAffine(np.full(frame.shape[:2],255,np.uint8),matrix,(size,size))
    mask=np.zeros((size,size),np.uint8);cv2.circle(mask,(round(half),round(half)),round(r*.80),255,-1)
    mask=cv2.bitwise_and(mask,valid);cv2.circle(mask,(round(half),round(half)),round(r*.27),0,-1)
    # Editor-added compass characters and the lower radar ring are screen-fixed,
    # not terrain. They can otherwise outvote the actual moving map features.
    cv2.rectangle(mask,(round(half-r*.30),0),(round(half+r*.30),round(half-r*.60)),0,-1)
    cv2.rectangle(mask,(round(half-r*.55),round(half+r*.38)),(round(half+r*.55),size),0,-1)
    cv2.rectangle(mask,(0,round(half-r*.22)),(round(half-r*.62),round(half+r*.22)),0,-1)
    cv2.rectangle(mask,(round(half+r*.62),round(half-r*.22)),(size,round(half+r*.22)),0,-1)
    phsv=cv2.cvtColor(patch,cv2.COLOR_BGR2HSV)
    symbols=cv2.inRange(phsv,np.array([0,100,165]),np.array([179,255,255]))
    mask[cv2.dilate(symbols,np.ones((5,5),np.uint8))>0]=0
    return patch,mask,{'center':[cx,cy],'radius':r,'arrow':arrow.tolist(),'normalizationOffset':[half-cx,half-cy]}


def run(bvid,part,start,end,step=1):
    if not L.gate():raise ValueError('timeline-gate-closed')
    root=L.ROOT;out=root/f'out/detectability-v2/route-follow/{bvid}/p{part:02}'
    out.mkdir(parents=True,exist_ok=True)
    video=root/f'out/detectability-collection/{bvid}/p{part:02}/video.mp4'
    cap=cv2.VideoCapture(str(video));rows=[];previous=None
    for t in np.arange(start,end+.001,step):
        cap.set(cv2.CAP_PROP_POS_MSEC,float(t)*1000);ok,frame=cap.read()
        row={'time':round(float(t),3),'accepted':False}
        if not ok:row['reason']='decode-failed';previous=None
        else:
            # Source caches use 852x480 consistently; preserve aspect ratio for other layouts.
            frame=cv2.resize(frame,(round(frame.shape[1]*480/frame.shape[0]),480))
            try:
                patch,mask,roi=minimap(frame);row['roi']=roi
                gray=cv2.cvtColor(patch,cv2.COLOR_BGR2GRAY)
                if previous is not None:row.update(displacement(previous[0],gray,previous[1],mask))
                else:row['reason']='component-start'
                previous=(gray,mask)
                path=out/f'minimap-{t:08.2f}.jpg';L.save_image(path,patch);row['patch']=path.relative_to(root).as_posix()
                L.save_image(out/f'mask-{t:08.2f}.png',mask)
            except ValueError as ex:row['reason']=str(ex);previous=None
        rows.append(row)
    cap.release();rows=integrate(rows)
    result={'bvid':bvid,'part':part,'start':start,'end':end,'step':step,'method':'north-up-relative-terrain-SIFT',
            'limitation':'Relative components only; no absolute identity, no bridging failed/cut edges. Requires manual cut review.', 'frames':rows}
    (out/'track.json').write_text(json.dumps(result,ensure_ascii=False,indent=2),encoding='utf-8')
    print(json.dumps({'bvid':bvid,'part':part,'frames':len(rows),'acceptedEdges':sum(r['accepted'] for r in rows),'components':rows[-1]['segment']+1}))
    return result


if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('bvid');p.add_argument('--part',type=int,default=1);p.add_argument('--start',type=float,required=True);p.add_argument('--end',type=float,required=True);p.add_argument('--step',type=float,default=1)
    a=p.parse_args();run(a.bvid,a.part,a.start,a.end,a.step)
