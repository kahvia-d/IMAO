"""Reuse video-only assets and fetch separate audio. Never read v1 labels or candidates."""
import concurrent.futures
import hashlib
import json
import time
from pathlib import Path
import av
import requests

ROOT=Path(__file__).resolve().parents[2]
OUT=ROOT/'out/detectability-v2'
HEADERS={'User-Agent':'Mozilla/5.0','Referer':'https://www.bilibili.com/'}

def save(path,data):
    path.parent.mkdir(parents=True,exist_ok=True)
    temp=path.with_suffix(path.suffix+'.tmp')
    temp.write_text(json.dumps(data,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    for retry in range(10):
        try:
            temp.replace(path);break
        except PermissionError:
            if retry==9:raise
            time.sleep(0.2)

def digest(path):
    with path.open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()

def inspect(path):
    with av.open(str(path)) as c:
        return {'duration':c.duration/av.time_base if c.duration is not None else None,
                'streams':[{'type':s.type,'codec':s.codec_context.name,'start':float(s.start_time*s.time_base) if s.start_time is not None else None,
                            'duration':float(s.duration*s.time_base) if s.duration is not None else None} for s in c.streams]}

def prepare(item,attempt_no=0):
    bv,p=item['bvid'],item['part']
    folder=OUT/bv/f'p{p:02d}'
    folder.mkdir(parents=True,exist_ok=True)
    audio=folder/'audio.m4a'
    metadata=folder/'media.json'
    old=json.loads(metadata.read_text('utf-8')) if metadata.exists() else None
    if old and audio.exists() and digest(audio)==old['audio']['sha256']:
        return {'bvid':bv,'part':p,'status':'cached'}
    video=ROOT/item['videoPath']
    vm=inspect(video)
    # All current cached videos are DASH video-only; preserve originals.
    if any(s['type']=='audio' for s in vm['streams']):
        audio=video
    else:
        response=requests.get('https://api.bilibili.com/x/player/playurl',params={'bvid':bv,'cid':item['cid'],'qn':64,'fnval':16,'fourk':0},headers=HEADERS,timeout=40)
        response.raise_for_status();payload=response.json()
        if payload.get('code')!=0:raise RuntimeError(f'playback code {payload.get("code")}')
        streams=payload.get('data',{}).get('dash',{}).get('audio',[])
        if not streams:raise RuntimeError('no audio stream')
        selected=max(streams,key=lambda s:s.get('bandwidth',0))
        urls=[selected.get('baseUrl',selected.get('base_url'))]+selected.get('backupUrl',selected.get('backup_url',[]))
        url=urls[attempt_no%len(urls)]
        temp=folder/'audio.partial';transfer=folder/'audio-transfer.json'
        previous=json.loads(transfer.read_text('utf-8')) if transfer.exists() else {}
        offset=temp.stat().st_size if temp.exists() and previous.get('streamId')==selected['id'] else 0
        headers=dict(HEADERS)
        if offset:
            headers['Range']=f'bytes={offset}-'
            if previous.get('etag'):headers['If-Range']=previous['etag']
        with requests.get(url,headers=headers,stream=True,timeout=(20,45)) as r:
            r.raise_for_status()
            if r.status_code==206:
                content_range=r.headers.get('content-range','')
                if not content_range.startswith(f'bytes {offset}-'):raise RuntimeError('audio range offset mismatch')
                expected=int(content_range.split('/')[-1])
            else:
                offset=0;expected=int(r.headers.get('content-length',0))
            save(transfer,{'streamId':selected['id'],'etag':r.headers.get('etag'),'expectedBytes':expected})
            n=offset
            with temp.open('ab' if offset else 'wb') as f:
                for block in r.iter_content(256*1024):f.write(block);n+=len(block)
            if expected and n!=expected:raise RuntimeError('audio length mismatch')
        am=inspect(temp)
        if not any(s['type']=='audio' for s in am['streams']):raise RuntimeError('download has no audio')
        temp.replace(audio)
    am=inspect(audio)
    if am['duration'] is None or abs(am['duration']-item['duration'])>3:raise RuntimeError('audio duration mismatch')
    save(metadata,dict(item,video=dict(vm,path=item['videoPath'],sha256=digest(video),bytes=video.stat().st_size),
                       audio=dict(am,path=audio.relative_to(ROOT).as_posix(),sha256=digest(audio),bytes=audio.stat().st_size),
                       alignmentStatus='container-timestamps-only; perceptual-review-pending'))
    return {'bvid':bv,'part':p,'status':'ready','audioBytes':audio.stat().st_size}

def attempt(item):
    for attempt_no in range(3):
        try:return prepare(item,attempt_no)
        except Exception as e:
            # No signed CDN URLs or query strings in diagnostics.
            failure={'bvid':item['bvid'],'part':item['part'],'status':'failed','errorType':type(e).__name__}
            if isinstance(e,RuntimeError):failure['reason']=str(e)
            if attempt_no<2:time.sleep(2)
    return failure

def main():
    inventory=json.loads((ROOT/'out/detectability-collection/inventory.json').read_text('utf-8'))
    manifest=[]
    for v in inventory:
        for p in v['pages']:
            bv,part=v['bvid'],p['page']
            path='out/detectability-spike-blackshores/video-480p.mp4' if bv=='BV1evKmzxE34' else f'out/detectability-collection/{bv}/p{part:02d}/video.mp4'
            manifest.append({'bvid':bv,'part':part,'cid':str(p['cid']),'duration':p['duration'],
                             'title':v['title'],'partTitle':p['part'],'videoPath':path})
    save(OUT/'manifest.json',manifest)
    results=[]
    with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
        for result in pool.map(attempt,manifest):
            results.append(result);save(OUT/'media-preparation.json',results)
            print(json.dumps(result,ensure_ascii=False),flush=True)
    if any(r['status']=='failed' for r in results):raise SystemExit(1)

if __name__=='__main__':main()
