"""Offline curation helpers; media downloads explicitly authorized by user.

No runtime integration. Separate per-video evidence directories permit parallel review.
"""
from pathlib import Path
import json,hashlib
import requests,cv2
from PIL import Image,ImageDraw,ImageFont

ROOT=Path(__file__).resolve().parents[2]
OUT=ROOT/'out/detectability-collection'
HEADERS={'User-Agent':'Mozilla/5.0','Referer':'https://www.bilibili.com/'}

def inventory():return json.loads((OUT/'inventory.json').read_text('utf-8'))

def download(bvid,part):
    item=next(x for x in inventory() if x['bvid']==bvid)
    page=next(x for x in item['pages'] if x['page']==part)
    folder=OUT/bvid/f'p{part:02d}';folder.mkdir(parents=True,exist_ok=True)
    target=folder/'video.mp4'
    if target.exists() and (folder/'media.json').exists():return folder
    r=requests.get('https://api.bilibili.com/x/player/playurl',params={'bvid':bvid,'cid':page['cid'],'qn':64,'fnval':16,'fourk':0},headers=HEADERS,timeout=30).json()
    if r.get('code')!=0:raise RuntimeError(f'{bvid} playback code {r.get("code")}')
    streams=r.get('data',{}).get('dash',{}).get('video',[])
    choices=[v for v in streams if v['codecs'].startswith('avc') and v['height']<=720]
    if not choices:raise RuntimeError('No supported public video stream')
    v=max(choices,key=lambda x:x['height'])
    with requests.get(v.get('baseUrl',v.get('base_url')),headers=HEADERS,stream=True,timeout=40) as response:
        response.raise_for_status();n=0;h=hashlib.sha256()
        with target.with_suffix('.partial').open('wb') as f:
            for block in response.iter_content(256*1024):n+=len(block);h.update(block);f.write(block)
        declared=int(response.headers.get('content-length',0))
        if declared and declared!=n:raise RuntimeError('Media length mismatch')
    target.with_suffix('.partial').replace(target)
    meta={'bvid':bvid,'part':part,'cid':str(page['cid']),'title':item['title'],'partTitle':page['part'],'durationSeconds':page['duration'],'bytes':n,'sha256':h.hexdigest(),'width':v['width'],'height':v['height']}
    (folder/'media.json').write_text(json.dumps(meta,ensure_ascii=False,indent=2),encoding='utf-8')
    return folder

def extract(folder,times):
    folder=Path(folder);cap=cv2.VideoCapture(str(folder/'video.mp4'));result=[]
    for t in times:
        f=folder/f'frame-{t:07.2f}.jpg'
        if not f.exists():
            cap.set(cv2.CAP_PROP_POS_MSEC,t*1000);ok,im=cap.read()
            if not ok:continue
            cv2.imwrite(str(f),im)
        result.append((t,f))
    cap.release();return result

def contact(folder,step=10,start=0,end=None):
    folder=Path(folder);meta=json.loads((folder/'media.json').read_text('utf-8'))
    end=meta['durationSeconds'] if end is None else end
    frames=extract(folder,range(start,end,step));font=ImageFont.truetype('C:/Windows/Fonts/msyh.ttc',16)
    outputs=[]
    for begin in range(0,len(frames),20):
        chunk=frames[begin:begin+20];canvas=Image.new('RGB',(4*426,5*264),'#141a24');draw=ImageDraw.Draw(canvas)
        for i,(t,f) in enumerate(chunk):
            im=Image.open(f).convert('RGB');im.thumbnail((426,240));x=i%4*426;y=i//4*264;canvas.paste(im,(x,y));draw.text((x+5,y+241),f'{t//60:02d}:{t%60:02d} ({t}s)',font=font,fill='white')
        name=folder/f'contact-{step}s-{chunk[0][0]:04d}.jpg';canvas.save(name,quality=88);outputs.append(str(name))
    return outputs

def guide(state,pid,folder,images=True):
    folder=Path(folder);folder.mkdir(parents=True,exist_ok=True);f=folder/f'guide-{state}-{pid}.json'
    if f.exists():d=json.loads(f.read_text('utf-8'))
    else:
        r=requests.post('https://api.kurobbs.com/map/core/position/getDetailOnline',headers={'source':'h5','wiki_type':'10','state_id':str(state)},data={'id':str(pid)},timeout=30);r.raise_for_status();d=r.json()
        if d.get('code')!=200:raise RuntimeError(f'Guide response code {d.get("code")}')
        f.write_text(json.dumps(d,ensure_ascii=False,indent=2),encoding='utf-8')
    p=d['data'];assert str(p['id'])==str(pid) and p['stateId']==state
    paths=[]
    if images:
        for i,url in enumerate(p.get('content',{}).get('picturesUrl',[])):
            dst=folder/f'guide-{state}-{pid}-{i+1}.png'
            if not dst.exists():
                response=requests.get(url,timeout=30);response.raise_for_status();dst.write_bytes(response.content)
            paths.append(str(dst))
    return p,paths

def points(states):
    result=[]
    for state in states:
        data=json.loads((ROOT/f'Assets/KuroMap/states/state-{state}.json').read_text('utf-8-sig'))
        for g in data:
            for p in g.get('location',[]):result.append(dict(p,stateId=state,typeId=g['id'],name=g['name']))
    return result

if __name__=='__main__':
    import argparse
    parser=argparse.ArgumentParser();parser.add_argument('bvid');parser.add_argument('--part',type=int,default=1);parser.add_argument('--step',type=int,default=10)
    a=parser.parse_args();folder=download(a.bvid,a.part)
    print(json.dumps({'folder':str(folder),'contacts':contact(folder,a.step)},ensure_ascii=False))
