"""Evidence preparation only: does not assert review completion."""
import sys,json,cv2,numpy as np
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2];OUT=ROOT/'out/detectability-v2/route-huanglong-late'
part=int(sys.argv[1]);ordinals=list(map(int,sys.argv[2:]));d=json.loads((ROOT/f'tools/MarkerDetectabilityV2/timelines/BV1iMTXzVE8J-p{part:02}.json').read_text('utf-8-sig'))
v=cv2.VideoCapture(str(ROOT/f'out/detectability-collection/BV1iMTXzVE8J/p{part:02}/video.mp4'))
for e in d['events']:
 if e['ordinal'] not in ordinals:continue
 frames=[];times=list(np.arange(e['start'],e['end'],3))
 for t in times:
  v.set(cv2.CAP_PROP_POS_MSEC,float(t)*1000);ok,f=v.read()
  if not ok:continue
  f=cv2.resize(f,(480,270));cv2.putText(f,f'{t:.0f}',(215,24),0,.8,(0,0,255),2);frames.append(f)
 for i in range(0,len(frames),12):
  a=frames[i:i+12];a+=[np.zeros_like(a[0])]*(12-len(a));cv2.imwrite(str(OUT/f'p{part}-e{e["ordinal"]}-route-{i//12}.jpg'),np.vstack([np.hstack(a[j:j+3]) for j in (0,3,6,9)]))
 print(e['ordinal'],len(frames),(len(frames)+11)//12)
