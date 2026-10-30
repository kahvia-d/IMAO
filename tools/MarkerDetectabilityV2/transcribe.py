"""Resumable local transcription. Outputs are machine evidence, never marked reviewed."""
import argparse
import hashlib
import json
import os
import time
from pathlib import Path
from prepare_media import ROOT,OUT,save,digest

def decode_mono(path):
    import av
    import numpy as np
    resampler=av.AudioResampler(format='fltp',layout='mono',rate=16000)
    chunks=[]
    with av.open(str(path)) as container:
        for frame in container.decode(audio=0):
            for converted in resampler.resample(frame):chunks.append(converted.to_ndarray().reshape(-1))
        for converted in resampler.resample(None):chunks.append(converted.to_ndarray().reshape(-1))
    if not chunks:raise ValueError('audio contains no decodable samples')
    return np.concatenate(chunks).astype(np.float32,copy=False)

def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--device',choices=['cuda','cpu'],default='cpu')
    parser.add_argument('--bvid');parser.add_argument('--part',type=int)
    args=parser.parse_args()
    dll_handles=[]
    if args.device=='cuda':
        cuda=Path('C:/Program Files/NVIDIA GPU Computing Toolkit/CUDA/v12.3/bin')
        if cuda.exists():
            os.environ['PATH']=str(cuda)+os.pathsep+os.environ['PATH']
            dll_handles.append(os.add_dll_directory(str(cuda)))
    from faster_whisper import WhisperModel
    config={'model':'large-v3','device':args.device,'computeType':'int8_float16' if args.device=='cuda' else 'int8',
            'language':'zh','beamSize':5,'wordTimestamps':True,'vadFilter':True,'conditionOnPreviousText':False}
    model_dir=OUT/'models/large-v3'
    model_hash=digest(model_dir/'model.bin')
    model=WhisperModel(str(model_dir),device=args.device,compute_type=config['computeType'],cpu_threads=8,num_workers=1)
    manifest=json.loads((OUT/'manifest.json').read_text('utf-8'))
    for item in manifest:
        bv,p=item['bvid'],item['part']
        if args.bvid and bv!=args.bvid:continue
        if args.part and p!=args.part:continue
        folder=OUT/bv/f'p{p:02d}'
        if not (folder/'media.json').exists():
            print(json.dumps({'bvid':bv,'part':p,'status':'audio-not-ready'}),flush=True);continue
        media=json.loads((folder/'media.json').read_text('utf-8'))
        audio_path=ROOT/media['audio']['path']
        audio_hash=digest(audio_path)
        destination=folder/'transcript.raw.json'
        if destination.exists():
            previous=json.loads(destination.read_text('utf-8'))
            if previous.get('audioSha256')==audio_hash and previous.get('modelSha256')==model_hash and previous.get('config')==config:
                print(json.dumps({'bvid':bv,'part':p,'status':'cached'}),flush=True);continue
        start=time.monotonic();print(json.dumps({'bvid':bv,'part':p,'status':'transcribing','config':config}),flush=True)
        try:
            segments,info=model.transcribe(decode_mono(audio_path),language='zh',beam_size=5,word_timestamps=True,
                                          vad_filter=True,condition_on_previous_text=False)
            records=[]
            with (folder/'transcript.partial.jsonl').open('w',encoding='utf-8') as f:
                for segment in segments:
                    row=segment._asdict()
                    row['words']=[w._asdict() for w in segment.words or []]
                    records.append(row);f.write(json.dumps(row,ensure_ascii=False)+'\n');f.flush()
                    print(json.dumps({'bvid':bv,'part':p,'end':round(segment.end,2),'text':segment.text},ensure_ascii=False),flush=True)
            result={'bvid':bv,'part':p,'status':'machine-transcript-only','audioSha256':audio_hash,
                    'modelSha256':model_hash,'config':config,'duration':info.duration,'durationAfterVad':info.duration_after_vad,
                    'segments':records,'elapsedSeconds':round(time.monotonic()-start,2)}
            save(destination,result)
            (folder/'transcript.txt').write_text('\n'.join(f'[{r["start"]:.2f}–{r["end"]:.2f}] {r["text"]}' for r in records)+'\n',encoding='utf-8')
            print(json.dumps({'bvid':bv,'part':p,'status':'transcribed','segments':len(records),'elapsedSeconds':result['elapsedSeconds']}),flush=True)
        except Exception as e:
            save(folder/'transcription-failure.json',{'bvid':bv,'part':p,'errorType':type(e).__name__,'message':str(e),'config':config})
            raise

if __name__=='__main__':main()
