"""Timeline discovery views from source video only; no overview/point metadata inputs."""
import argparse
import json
from pathlib import Path
import cv2
from PIL import Image,ImageDraw,ImageFont
from prepare_media import ROOT,OUT,save

FONT=ImageFont.truetype('C:/Windows/Fonts/msyh.ttc',15)

def main():
    parser=argparse.ArgumentParser();parser.add_argument('bvid');parser.add_argument('--part',type=int,default=1)
    parser.add_argument('--start',type=float,default=0);parser.add_argument('--end',type=float)
    parser.add_argument('--step',type=float,default=5)
    args=parser.parse_args()
    manifest=json.loads((OUT/'manifest.json').read_text('utf-8'))
    item=next(r for r in manifest if r['bvid']==args.bvid and r['part']==args.part)
    folder=OUT/args.bvid/f'p{args.part:02d}'/'visual';folder.mkdir(parents=True,exist_ok=True)
    cap=cv2.VideoCapture(str(ROOT/item['videoPath']))
    duration=cap.get(cv2.CAP_PROP_FRAME_COUNT)/cap.get(cv2.CAP_PROP_FPS)
    end=min(args.end if args.end is not None else duration,duration)
    frames=[];t=args.start
    while t<end:
        name=folder/f'frame-{t:08.2f}.jpg'
        if not name.exists():
            cap.set(cv2.CAP_PROP_POS_MSEC,t*1000);ok,im=cap.read()
            if not ok:break
            cv2.imwrite(str(name),im)
        frames.append((t,name));t+=args.step
    cap.release();sheets=[]
    for offset in range(0,len(frames),12):
        batch=frames[offset:offset+12]
        sheet=Image.new('RGB',(1280,4*265),'#10151e');draw=ImageDraw.Draw(sheet)
        for i,(second,path) in enumerate(batch):
            with Image.open(path) as im:
                im=im.convert('RGB');im.thumbnail((420,237))
                x=(i%3)*426;y=(i//3)*265
                sheet.paste(im,(x,y));draw.text((x+5,y+239),f'{int(second)//60:02d}:{second%60:05.2f}  ({second:g}s)',font=FONT,fill='white')
        path=folder/f'scenes-{args.step:g}s-{batch[0][0]:08.2f}.jpg'
        sheet.save(path,quality=90);sheets.append(path.relative_to(ROOT).as_posix())
    # Keep the embedded progress bar at native width to preserve small labels.
    for offset in range(0,len(frames),20):
        batch=frames[offset:offset+20]
        with Image.open(batch[0][1]) as first:w,h=first.size
        strip_height=round(h*.13)
        sheet=Image.new('RGB',(w+110,len(batch)*(strip_height+5)),'#10151e');draw=ImageDraw.Draw(sheet)
        for i,(second,path) in enumerate(batch):
            with Image.open(path) as im:strip=im.crop((0,h-strip_height,w,h))
            y=i*(strip_height+5);sheet.paste(strip,(110,y));draw.text((3,y+4),f'{second:.1f}s',font=FONT,fill='white')
        path=folder/f'progress-{args.step:g}s-{batch[0][0]:08.2f}.jpg';sheet.save(path,quality=95)
        sheets.append(path.relative_to(ROOT).as_posix())
    save(folder/f'scan-{args.start:g}-{end:g}-{args.step:g}.json',{'sourceVideo':item['videoPath'],'stepSeconds':args.step,
        'start':args.start,'end':end,'frames':[p.relative_to(ROOT).as_posix() for _,p in frames],
        'sheets':sheets,'reviewStatus':'not-reviewed','warning':'Discovery views only; not proof of complete event review.'})
    print(json.dumps({'bvid':args.bvid,'part':args.part,'frames':len(frames),'sheets':sheets},ensure_ascii=False))

if __name__=='__main__':main()
