"""Repeat a bounded local ASR passage without overwriting the original transcript."""
import argparse
import os
from pathlib import Path
from prepare_media import OUT, save, digest
from transcribe import decode_mono


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('bvid'); p.add_argument('--part', type=int, default=1)
    p.add_argument('--start', type=float, required=True); p.add_argument('--end', type=float, required=True)
    a = p.parse_args()
    if not 0 <= a.start < a.end:
        p.error('invalid audio interval')
    folder = OUT / a.bvid / f'p{a.part:02d}'
    dest = folder / f'transcript.repeat-{a.start:g}-{a.end:g}.json'
    if dest.exists():
        print(dest.read_text('utf-8')); return
    cuda = Path('C:/Program Files/NVIDIA GPU Computing Toolkit/CUDA/v12.3/bin')
    handle = None
    if cuda.exists():
        os.environ['PATH'] = str(cuda) + os.pathsep + os.environ['PATH']
        handle = os.add_dll_directory(str(cuda))
    from faster_whisper import WhisperModel
    device = 'cuda' if handle else 'cpu'
    config = dict(language='zh', beam_size=10, vad_filter=False, condition_on_previous_text=False)
    model = WhisperModel(str(OUT/'models/large-v3'), device=device,
                         compute_type='int8_float16' if handle else 'int8')
    audio = decode_mono(folder/'audio.m4a')
    if a.end > len(audio)/16000 + .05:
        raise ValueError('interval exceeds audio')
    segments, _ = model.transcribe(audio[round(a.start*16000):round(a.end*16000)], **config)
    result = dict(bvid=a.bvid, part=a.part, start=a.start, end=a.end,
                  sourceAudioSha256=digest(folder/'audio.m4a'), config=config,
                  status='machine-repeat-not-ground-truth',
                  segments=[dict(start=s.start+a.start, end=s.end+a.start, text=s.text) for s in segments])
    save(dest, result)
    import json
    print(json.dumps(result, ensure_ascii=False))


if __name__ == '__main__':
    main()
