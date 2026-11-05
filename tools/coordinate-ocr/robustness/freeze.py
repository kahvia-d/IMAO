"""Freeze production-shaped replay inputs before looking at any candidate predictions."""
import hashlib
import itertools
import json
from pathlib import Path

import cv2
import numpy as np

from train import ROOT, OUT, read

EVIDENCE = ROOT / "out/evidence"
OFFSETS = list(itertools.product([-1, 0, 1], repeat=2))


def freeze():
    folder = OUT / "native-evaluation"
    folder.mkdir(exist_ok=False)
    manifest = []
    sources = []

    def add_frame(key, frame, rect, expected, calibrated_size):
        x, y, width, height = rect
        conditions = [(f"offset-{dx}-{dy}", x+dx, y+dy, width, height) for dx, dy in OFFSETS]
        for factor in (.9, 1.1):
            w, h = round(width*factor), round(height*factor)
            conditions.append((f"scale-{factor}", x+(width-w)//2, y+(height-h)//2, w, h))
        for name, left, top, w, h in conditions:
            if left < 0 or top < 0 or left+w > frame.shape[1] or top+h > frame.shape[0]:
                continue
            crop = frame[top:top+h, left:left+w]
            path = folder / f"{key}-{name}.png"
            assert cv2.imwrite(str(path), crop)
            manifest.append(dict(id=f"{key}/{name}", crop=str(path), expected=expected,
                                 calibrated=calibrated_size and (left,top,w,h)==rect))

    label_path = EVIDENCE / "ocr-sensitivity-20261024/visual-labels-v2.json"
    for item in read(label_path)["frames"]:
        i = item["frame"]
        path = EVIDENCE / f"client-crop-fix-20261024/frames/{i}.png"
        frame = cv2.imread(str(path))
        baseline = cv2.imread(str(EVIDENCE / f"ocr-sensitivity-20261024/crops/{i}-0-0.png"))
        assert np.array_equal(frame[1092:1117,25:481], baseline)
        sources.append((str(path), hashlib.sha256(path.read_bytes()).hexdigest()))
        add_frame(f"repro-{i}", frame, (25,1092,456,25), item.get("xy"), False)

    # Reserve the entire unused session, including as a source of synthetic backgrounds.
    holdout = next((ROOT / "out/coordinate-independent-20261016/capture").glob("session-448*"))
    for path in sorted(holdout.glob("frame-*.png")):
        if ".minimap." not in path.name:
            sources.append((str(path), hashlib.sha256(path.read_bytes()).hexdigest()))
    (folder / "manifest.json").write_text(json.dumps(manifest))
    (folder / "source-lock.json").write_text(json.dumps(dict(sources),indent=2))
    print(len(manifest), "replay inputs;", len(sources), "source hashes")


if __name__ == "__main__":
    freeze()
