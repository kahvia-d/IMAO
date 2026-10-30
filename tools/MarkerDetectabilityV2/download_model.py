"""Resumable public model download with byte-range checks and upstream SHA256."""
from concurrent.futures import ThreadPoolExecutor,as_completed
import time
import requests
from prepare_media import OUT,save,digest

URL='https://huggingface.co/Systran/faster-whisper-large-v3/resolve/main/model.bin'
DEST=OUT/'models/large-v3/model.bin'

def main():
    with requests.get(URL,headers={'Range':'bytes=0-0'},stream=True,timeout=30) as r:
        r.raise_for_status();total=int(r.headers['content-range'].split('/')[-1])
        sha=next(h.headers['x-linked-etag'].strip('"') for h in r.history if h.headers.get('x-linked-etag'))
    if DEST.exists() and DEST.stat().st_size==total and digest(DEST)==sha:
        print('Model cached and SHA256 verified',flush=True);return
    folder=DEST.parent/'chunks';folder.mkdir(parents=True,exist_ok=True)
    block=16*1024*1024
    ranges=[(i,a,min(a+block,total)-1) for i,a in enumerate(range(0,total,block))]
    save(DEST.parent/'download-manifest.json',{'repository':'Systran/faster-whisper-large-v3','bytes':total,'sha256':sha,'chunkBytes':block})
    def chunk(spec):
        i,a,b=spec;path=folder/f'{i:04d}.bin'
        if path.exists() and path.stat().st_size==b-a+1:return i
        error=''
        for attempt in range(4):
            try:
                with requests.get(URL,headers={'Range':f'bytes={a}-{b}'},stream=True,timeout=(20,45)) as r:
                    if r.status_code!=206 or r.headers.get('content-range')!=f'bytes {a}-{b}/{total}':
                        raise ValueError('range response differs from requested bytes')
                    temp=path.with_suffix('.partial')
                    with temp.open('wb') as f:
                        for piece in r.iter_content(256*1024):f.write(piece)
                    if temp.stat().st_size!=b-a+1:raise ValueError('incomplete range')
                    temp.replace(path);return i
            except Exception as e:
                error=type(e).__name__;time.sleep(1)
        raise RuntimeError(f'chunk {i} failed: {error}')
    done=0
    with ThreadPoolExecutor(max_workers=4) as pool:
        futures=[pool.submit(chunk,spec) for spec in ranges]
        for future in as_completed(futures):
            future.result();done+=1
            if done%8==0 or done==len(ranges):print(f'Model chunks {done}/{len(ranges)}',flush=True)
    temp=DEST.with_suffix('.assembling')
    with temp.open('wb') as output:
        for i,a,b in ranges:
            with (folder/f'{i:04d}.bin').open('rb') as source:
                while piece:=source.read(1024*1024):output.write(piece)
    if digest(temp)!=sha:raise RuntimeError('model SHA256 mismatch')
    temp.replace(DEST);print('Model assembled; upstream SHA256 verified',flush=True)

if __name__=='__main__':main()
