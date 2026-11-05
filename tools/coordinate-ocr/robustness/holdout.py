"""Generate only the crops backed by captured pixels from the reserved session."""
import csv
import hashlib
import json
from pathlib import Path

import cv2

from freeze import OFFSETS
from train import ROOT, OUT, read


def main():
    folder = OUT / "native-evaluation"
    lock = read(folder / "source-lock.json")
    capture = next((ROOT / "out/coordinate-independent-20261016/capture").glob("session-448*"))
    rows = []
    with Path(__file__).with_name("holdout-448C5F2E.csv").open(encoding="utf-8-sig") as f:
        for item in csv.DictReader(f):
            index = int(item["frame"])
            path = capture / f"frame-{index:06}.png"
            assert hashlib.sha256(path.read_bytes()).hexdigest() == lock[str(path)]
            strip = cv2.imread(str(path))
            expected = [int(item["x"]), int(item["y"])] if item["x"] else None
            variants = [(f"offset-{dx}-{dy}", 32+dx, 93+dy, 608, 33) for dx,dy in OFFSETS]
            variants.append(("scale-0.9",62,94,547,30))
            for name,x,y,w,h in variants:
                if x+w > strip.shape[1] or y+h > strip.shape[0]:
                    continue
                crop = strip[y:y+h,x:x+w]
                destination = folder / f"holdout-{index}-{name}.png"
                assert cv2.imwrite(str(destination),crop)
                rows.append(dict(id=f"holdout-{index}/{name}",crop=str(destination),expected=expected,
                                 calibrated=(x,y,w,h)==(32,93,608,33)))
    (folder / "holdout.json").write_text(json.dumps(rows))


if __name__ == "__main__":
    main()
