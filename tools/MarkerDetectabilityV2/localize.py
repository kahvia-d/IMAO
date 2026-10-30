"""Independent offline SIFT/RANSAC localization (no marker IDs or old candidates).

Usage: localize.py --inventory
       localize.py --config out/detectability-v2/localize-config.json
Config: {references:[{manifest:repoPath,bounds:[xmin,xmax,ymin,ymax],scale:.5}],
 frames:[{path:repoPath,time:seconds,segmentId:'continuous-shot',cutBefore:false,
          roi:{center:[x,y],radius:r,player:[x,y],excludeRects:[[x,y,w,h]]}}],
 output:'out/detectability-v2/localization'}.
ROI is in ORIGINAL frame pixels. Omit roi to try conservative circle/arrow detection.
Dedicated layer image references require image, pixelToGame (2x3), region, scene,
state, layer, provenance; tiled references are surface only. Never infer a floor.
Real frames are gated by ALL manifest timelines. No bypass CLI is provided.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path

import cv2
import numpy as np
from contract import can_start_matching

ROOT = Path(__file__).resolve().parents[2]
LIMITS = dict(ratio=.70, minMatches=16, minInliers=12, minInlierRatio=.5,
              maxMedianError=2., maxP90Error=3., minHullFraction=.08,
              minScale=.25, maxScale=16., ransac=3., ambiguityFactor=1.25)


def read(path):
    return json.loads(Path(path).read_text(encoding='utf-8-sig'))


def repo_path(value):
    path = (ROOT / value).resolve()
    path.relative_to(ROOT)
    return path


def image(path):
    im = cv2.imdecode(np.fromfile(path, np.uint8), cv2.IMREAD_COLOR)
    if im is None:
        raise ValueError(f'unreadable image: {path}')
    return im


def save_image(path, im):
    ok, encoded = cv2.imencode(Path(path).suffix, im)
    if not ok:
        raise ValueError('image encoding failed')
    encoded.tofile(path)


def tile_transform(bounds, scale, tile_size=1024, virtual_size=850):
    """Mosaic pixels -> game units; raw marker units are game *100."""
    xmin, _, _, ymax = bounds
    k = virtual_size / tile_size
    return np.array([[k/scale, 0, (xmin-1)*virtual_size],
                     [0, k/scale, -ymax*virtual_size]], np.float64)


def project(matrix, points):
    points = np.asarray(points, np.float64)
    return points @ np.asarray(matrix)[:, :2].T + np.asarray(matrix)[:, 2]


def load_reference(spec):
    if 'image' in spec:
        for key in ('pixelToGame', 'region', 'scene', 'state', 'layer', 'provenance'):
            if key not in spec:
                raise ValueError(f'dedicated image reference missing {key}')
        im = image(repo_path(spec['image']))
        transform = np.asarray(spec['pixelToGame'], np.float64)
        if transform.shape != (2, 3) or not np.isfinite(transform).all() or abs(np.linalg.det(transform[:, :2])) < 1e-10:
            raise ValueError('invalid pixelToGame')
        return dict(image=im, valid=np.full(im.shape[:2],255,np.uint8), pixelToGame=transform,
                    metadata={k:spec[k] for k in ('region','scene','state','layer','provenance')}, source=spec)
    path = repo_path(spec['manifest'])
    manifest = read(path)
    if spec.get('layer', 'surface') != 'surface':
        raise ValueError('surface tile mosaic cannot be relabeled as a different layer')
    bounds = spec.get('bounds')
    if not bounds or len(bounds) != 4:
        raise ValueError('explicit tile bounds required (avoid unbounded atlas allocation)')
    xmin,xmax,ymin,ymax = bounds
    scale = float(spec.get('scale', .5))
    size = manifest['source']['tileSize']
    if not 0 < scale <= 1 or xmax < xmin or ymax < ymin:
        raise ValueError('invalid mosaic bounds/scale')
    tile = round(size*scale)
    if abs(tile-size*scale)>1e-8:
        raise ValueError('scaled tile size must be integral')
    width,height = (xmax-xmin+1)*tile,(ymax-ymin+1)*tile
    if width*height>40_000_000:
        raise ValueError('mosaic exceeds 40 million pixels; split reference bounds')
    im=np.zeros((height,width,3),np.uint8);valid=np.zeros((height,width),np.uint8)
    used=[];missing=[]
    for t in manifest['tiles']:
        x,y=t['x'],t['y']
        # Manifests include several floors at the same XY. A missing floor must
        # never fall back to a surface tile, nor overwrite the surface mosaic.
        state=manifest['source']['state']
        if Path(t['file']).name != f'{state}_{x}_{y}.png':
            continue
        if not xmin<=x<=xmax or not ymin<=y<=ymax:
            continue
        file=path.parent/t['file']
        if not file.is_file():
            state=manifest['source']['state']
            file=ROOT/'map-regions/tiles'/manifest['resourceVersion']/str(state)/f'{state}_{x}_{y}.png'
        if not file.is_file():
            missing.append([x,y]);continue
        src=image(file)
        if src.shape[:2] != (size,size):
            raise ValueError(f'tile dimensions differ: {file}')
        # Reject stale/mixed-cache bytes rather than silently mixing resource versions.
        if t.get('sha256') and hashlib.sha256(file.read_bytes()).hexdigest()!=t['sha256']:
            raise ValueError(f'tile digest differs: {file}')
        xx,yy=(x-xmin)*tile,(ymax-y)*tile
        im[yy:yy+tile,xx:xx+tile]=cv2.resize(src,(tile,tile),interpolation=cv2.INTER_AREA)
        valid[yy:yy+tile,xx:xx+tile]=255;used.append([x,y])
    if not used:
        raise ValueError('no cached tiles in requested bounds')
    valid=cv2.erode(valid,np.ones((7,7),np.uint8))
    return dict(image=im,valid=valid,pixelToGame=tile_transform(bounds,scale,size,manifest['source']['virtualMapSize']),
                metadata=dict(region=manifest['packId'],scene=manifest['scene'],state=manifest['source']['state'],
                              layer='surface',resourceVersion=manifest['resourceVersion'],bounds=bounds,
                              mosaicScale=scale,missingTiles=missing,coordinateTransform=manifest.get('coordinateTransform')),
                source=spec)


def detect_roi(frame):
    h,w=frame.shape[:2]
    gray=cv2.cvtColor(frame[:round(h*.45),:round(w*.35)],cv2.COLOR_BGR2GRAY)
    circles=cv2.HoughCircles(gray,cv2.HOUGH_GRADIENT,1.2,max(20,h*.08),param1=100,param2=45,
                             minRadius=round(h*.06),maxRadius=round(h*.23))
    if circles is None:
        raise ValueError('minimap circle not detected; configure ROI')
    candidates=[c for c in circles[0] if c[0]-c[2]>=0 and c[1]-c[2]>=0]
    if len(candidates)!=1:
        raise ValueError(f'minimap circle ambiguous ({len(candidates)}); configure ROI')
    x,y,r=map(float,candidates[0])
    return dict(center=[x,y],radius=r,roiMethod='hough-circle')


def roi_mask(frame, configuration=None):
    roi=dict(configuration or detect_roi(frame));h,w=frame.shape[:2]
    center=np.asarray(roi['center'],np.float64);radius=float(roi['radius'])
    if radius<15 or not np.isfinite(center).all() or not np.isfinite(radius) or center[0]-radius<0 or center[1]-radius<0 or center[0]+radius>=w or center[1]+radius>=h:
        raise ValueError('ROI circle outside frame or invalid')
    mask=np.zeros((h,w),np.uint8);c=tuple(np.rint(center).astype(int))
    cv2.circle(mask,c,round(radius*.87),255,-1) # boundary + compass text
    hsv=cv2.cvtColor(frame,cv2.COLOR_BGR2HSV)
    if 'player' not in roi:
        yellow=cv2.inRange(hsv,np.array([18,110,150]),np.array([42,255,255]))
        near=np.zeros_like(mask);cv2.circle(near,c,round(radius*.25),255,-1)
        count,labels,stats,centroids=cv2.connectedComponentsWithStats(cv2.bitwise_and(yellow,near))
        candidates=[i for i in range(1,count) if 6<=stats[i,cv2.CC_STAT_AREA]<=radius*radius*.06]
        if len(candidates)!=1:
            raise ValueError(f'player arrow ambiguous ({len(candidates)}); configure player pixel')
        roi['player']=centroids[candidates[0]].tolist();roi['playerMethod']='yellow-component-centroid-approximate'
    else:
        roi['playerMethod']='configured-pixel'
    player=np.asarray(roi['player'],np.float64)
    if player.shape!=(2,) or not np.isfinite(player).all() or np.linalg.norm(player-center)>radius*.6:
        raise ValueError('player pixel outside central minimap area')
    cv2.circle(mask,tuple(np.rint(player).astype(int)),round(radius*.24),0,-1)
    # Bright compact symbols: conservative heuristic; configured rectangles remain authoritative.
    symbols=((hsv[:,:,2]>210)&((hsv[:,:,1]<50)|((hsv[:,:,1]>130)&((hsv[:,:,0]<45)|(hsv[:,:,0]>125))))).astype(np.uint8)*255
    count,labels,stats,_=cv2.connectedComponentsWithStats(symbols)
    for i in range(1,count):
        x,y,bw,bh,area=stats[i]
        if 3<=area<=180 and bw<=radius*.3 and bh<=radius*.3 and mask[min(h-1,y+bh//2),min(w-1,x+bw//2)]:
            cv2.rectangle(mask,(max(0,x-2),max(0,y-2)),(min(w-1,x+bw+2),min(h-1,y+bh+2)),0,-1)
    for x,y,bw,bh in roi.get('excludeRects',[]):
        cv2.rectangle(mask,(round(x),round(y)),(round(x+bw),round(y+bh)),0,-1)
    roi.setdefault('roiMethod','configured-circle')
    roi['maskMethod']='inner-87%-circle;player-24%-radius;compact-bright-symbols;explicit-rectangles'
    return roi,mask


def fit_geometry(src,dst,player,radius,limits=LIMITS):
    result=dict(acceptedGeometry=False,goodMatches=len(src),rejectionReasons=[])
    if len(src)<limits['minMatches']:
        result['rejectionReasons']=['insufficient-ratio-matches'];return result,None,None
    matrix,inliers=cv2.estimateAffinePartial2D(np.float32(src),np.float32(dst),method=cv2.RANSAC,
                      ransacReprojThreshold=limits['ransac'],maxIters=10000,confidence=.999,refineIters=10)
    if matrix is None or inliers is None or not np.isfinite(matrix).all():
        result['rejectionReasons']=['ransac-fit-failed'];return result,None,None
    good=inliers.ravel().astype(bool);n=int(good.sum())
    errors=np.linalg.norm(project(matrix,src)-dst,axis=1)
    scale=float(np.hypot(matrix[0,0],matrix[1,0]));angle=math.degrees(math.atan2(matrix[1,0],matrix[0,0]))
    hull=cv2.convexHull(np.float32(src)[good])
    fraction=float(cv2.contourArea(hull)/(math.pi*radius*radius))
    median=float(np.median(errors[good]));p90=float(np.percentile(errors[good],90))
    result.update(inliers=n,inlierRatio=n/len(src),medianErrorReferencePixels=median,p90ErrorReferencePixels=p90,
                  scale=scale,rotationDegrees=angle,hullFraction=fraction,affineFrameToReference=matrix.tolist(),
                  playerReferencePixel=project(matrix,player).tolist(),inlierFramePixels=np.asarray(src)[good].tolist(),
                  inlierReferencePixels=np.asarray(dst)[good].tolist())
    checks=[(n<limits['minInliers'],'insufficient-inliers'),(n/len(src)<limits['minInlierRatio'],'low-inlier-ratio'),
            (median>limits['maxMedianError'],'high-median-error'),(p90>limits['maxP90Error'],'high-p90-error'),
            (fraction<limits['minHullFraction'],'inliers-spatially-concentrated'),
            (not limits['minScale']<=scale<=limits['maxScale'],'scale-outside-fixed-bounds')]
    result['rejectionReasons']=[reason for bad,reason in checks if bad]
    result['acceptedGeometry']=not result['rejectionReasons']
    return result,matrix,good


def match_reference(frame,roi,mask,reference):
    sift=cv2.SIFT_create(nfeatures=24000,contrastThreshold=.015)
    kq,dq=sift.detectAndCompute(cv2.cvtColor(frame,cv2.COLOR_BGR2GRAY),mask)
    if 'features' not in reference:
        reference['features']=sift.detectAndCompute(cv2.cvtColor(reference['image'],cv2.COLOR_BGR2GRAY),reference['valid'])
    kr,dr=reference['features']
    if dq is None or dr is None or len(dr)<2:
        return dict(acceptedGeometry=False,rejectionReasons=['insufficient-descriptors'],reference=reference['metadata']),None
    pairs=cv2.BFMatcher().knnMatch(dq,dr,k=2)
    matches=[a for pair in pairs if len(pair)==2 for a,b in [pair] if a.distance<LIMITS['ratio']*b.distance]
    # Enforce one-to-one reference keypoints, so duplicate detections do not inflate support.
    unique={}
    for m in sorted(matches,key=lambda m:m.distance):unique.setdefault(m.trainIdx,m)
    matches=list(unique.values())
    src=np.float32([kq[m.queryIdx].pt for m in matches]);dst=np.float32([kr[m.trainIdx].pt for m in matches])
    result,matrix,inliers=fit_geometry(src,dst,roi['player'],roi['radius'])
    result['reference']=reference['metadata']
    if matrix is not None:
        px=np.asarray(result['playerReferencePixel']);x,y=np.rint(px).astype(int);h,w=reference['valid'].shape
        if not (0<=x<w and 0<=y<h and reference['valid'][y,x]):
            result['rejectionReasons'].append('player-outside-cached-map');result['acceptedGeometry']=False
        game=project(reference['pixelToGame'],px)
        result.update(gamePosition=game.tolist(),rawPosition=(game*100).tolist(),pixelToGame=reference['pixelToGame'].tolist())
        transform=reference['metadata'].get('coordinateTransform')
        if transform:result['appMapPosition']=(game*transform['scale']+np.array([transform['originX'],transform['originY']])).tolist()
    drawing=cv2.drawMatches(frame,kq,reference['image'],kr,matches,None,
                            matchesMask=inliers.astype(int).tolist() if inliers is not None else None,
                            flags=cv2.DrawMatchesFlags_NOT_DRAW_SINGLE_POINTS)
    return result,drawing


def continuity(previous,current,frame,max_speed=80,max_gap=10):
    if frame.get('cutBefore'):
        return dict(status='reset',reason='explicit-teleport-or-edit-cut',accepted=True)
    if not previous or not frame.get('segmentId'):
        return dict(status='not-checked',reason='no-continuous-predecessor',accepted=True)
    if frame['segmentId']!=previous['segmentId']:
        return dict(status='reset',reason='segment-changed',accepted=True)
    dt=float(frame['time'])-previous['time']
    if dt<=0:return dict(status='rejected',reason='non-increasing-time',accepted=False)
    if dt>max_gap:return dict(status='not-checked',reason='sampling-gap',accepted=True)
    for key in ('scene','state','layer'):
        if previous['result']['reference'][key]!=current['reference'][key]:
            return dict(status='rejected',reason='reference-domain-change-without-cut',accepted=False)
    distance=float(np.linalg.norm(np.array(current['gamePosition'])-previous['result']['gamePosition']))
    speed=distance/dt
    return dict(status='consistent' if speed<=max_speed else 'rejected',reason='speed-bound',
                distanceGameUnits=distance,speedGameUnitsPerSecond=speed,maxSpeed=max_speed,accepted=speed<=max_speed)


def gate():
    manifest=read(ROOT/'out/detectability-v2/manifest.json');timelines=[]
    for m in manifest:
        p=ROOT/'tools/MarkerDetectabilityV2/timelines'/f"{m['bvid']}-p{m['part']:02}.json"
        if p.exists():timelines.append(read(p))
    return can_start_matching([(m['bvid'],m['part']) for m in manifest],timelines)


def run(config):
    if not gate():raise ValueError('global-timeline-gate-closed; real localization refused')
    out=repo_path(config.get('output','out/detectability-v2/localization'))
    allowed=(ROOT/'out/detectability-v2').resolve()
    out.relative_to(allowed);out.mkdir(parents=True,exist_ok=True)
    references=[load_reference(s) for s in config['references']]
    results=[];previous=None
    for i,f in enumerate(config['frames']):
        entry=dict(frame=f,acceptedLocalization=False,candidates=[],rejectionReasons=[])
        try:
            frame=image(repo_path(f['path']));roi,mask=roi_mask(frame,f.get('roi'));entry['roi']=roi
            diagnostic=frame.copy();diagnostic[mask==0]//=4
            cv2.circle(diagnostic,tuple(np.rint(roi['player']).astype(int)),3,(0,0,255),-1)
            path=out/f'{i:05}-mask.jpg';save_image(path,diagnostic);entry['maskVisualization']=path.relative_to(ROOT).as_posix()
            for j,ref in enumerate(references):
                candidate,drawing=match_reference(frame,roi,mask,ref)
                if drawing is not None:
                    path=out/f'{i:05}-ref{j:02}.jpg';save_image(path,drawing);candidate['visualization']=path.relative_to(ROOT).as_posix()
                entry['candidates'].append(candidate)
            candidates=sorted([c for c in entry['candidates'] if c['acceptedGeometry']],key=lambda c:c['inliers'],reverse=True)
            if not candidates:entry['rejectionReasons'].append('no-reference-passed-geometry')
            elif len(candidates)>1 and candidates[0]['inliers']<candidates[1]['inliers']*LIMITS['ambiguityFactor']:
                entry['rejectionReasons'].append('ambiguous-reference-support')
            else:
                chosen=candidates[0];temporal=continuity(previous,chosen,f)
                entry.update(selected=chosen,temporal=temporal,acceptedLocalization=temporal['accepted'])
                if not temporal['accepted']:entry['rejectionReasons'].append(temporal['reason'])
                else:previous=dict(result=chosen,time=float(f['time']),segmentId=f.get('segmentId'))
        except (ValueError,KeyError,cv2.error) as exc:
            entry['rejectionReasons'].append(str(exc))
        if f.get('cutBefore') and not entry['acceptedLocalization']:previous=None
        results.append(entry)
    report=dict(method='SIFT-ratio + partial-affine-RANSAC',limits=LIMITS,globalTimelineGate=True,
                notes='Geometry only; no point identity or detectability inferred. Auto arrow centroid approximate; inspect mask/overlay. Temporal reset is not evidence of teleport.',results=results)
    (out/'localization.json').write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding='utf-8')
    return report


def inventory():
    rows=[]
    for path in sorted((ROOT/'Assets/FeaturesDatas/KuroTilePacks').glob('*/manifest.json')):
        m=read(path)
        rows.append(dict(manifest=path.relative_to(ROOT).as_posix(),region=m.get('packId'),scene=m.get('scene'),
                         state=m.get('source',{}).get('state'),bounds=m.get('tileBounds'),
                         coordinateTransform=m.get('coordinateTransform'),resourceVersion=m.get('resourceVersion'),tiles=len(m.get('tiles',[]))))
    return rows


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--inventory',action='store_true');p.add_argument('--config',type=Path);a=p.parse_args()
    try:
        if a.inventory:print(json.dumps(inventory(),ensure_ascii=False,indent=2))
        elif a.config:
            result=run(read(a.config));print(json.dumps({'frames':len(result['results']),'accepted':sum(r['acceptedLocalization'] for r in result['results'])}))
        else:p.error('choose --inventory or --config')
    except (ValueError,KeyError,FileNotFoundError) as exc:
        print(json.dumps(dict(status='refused',reason=str(exc)),ensure_ascii=False));raise SystemExit(2)
