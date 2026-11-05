"""Rebuild the manually transcribed, fixed-frame local video validation set."""
import argparse
import csv
import json
from collections import Counter, defaultdict
from pathlib import Path

import cv2

from freeze import OFFSETS
from train import OUT


def continuous(path):
    folder = OUT / "video-continuous"
    folder.mkdir(exist_ok=True)
    capture = cv2.VideoCapture(str(path))
    fps = capture.get(cv2.CAP_PROP_FPS)
    rows = []
    try:
        if fps < 10:
            raise ValueError(f"Cannot sample {path} at 10 Hz (source FPS {fps})")
        index = 0
        while capture.grab():
            if index == round(len(rows)*fps/10):
                ok, frame = capture.retrieve()
                if not ok or frame.shape[:2] != (1440, 2560):
                    raise ValueError(f"Invalid 1440p frame {index}")
                image = folder / f"{index}.png"
                if not cv2.imwrite(str(image), frame[1405:1438, 32:640]):
                    raise OSError(image)
                rows.append(dict(id=str(index), crop=str(image), calibrated=True))
            index += 1
    finally:
        capture.release()
    if not rows:
        raise ValueError(f"No readable frames in {path}")
    (folder / "manifest.json").write_text(json.dumps(rows), encoding="utf-8")


def score(manifest, predictions):
    by_id = {row["id"]: row for row in predictions}
    if len(by_id) != len(predictions) or set(by_id) != {row["id"] for row in manifest}:
        raise ValueError("Prediction IDs differ from the frozen video inputs")
    conditions = defaultdict(Counter)
    for row in manifest:
        selected = by_id[row["id"]]["selected"]
        xy = selected["xy"] if selected is not None else None
        if row["label"] == "unknown":
            result = "unscored"
        elif row["label"] == "absent":
            result = "trueNegative" if xy is None else "falsePositive"
        elif xy is None:
            result = "reject"
        else:
            result = "correct" if xy == row["expected"] else "wrong"
        conditions[row["id"].split("/", 1)[1]][result] += 1
    return conditions


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("recordings", type=Path, nargs="?", help="Root containing the Desktop and Wuthering Waves recording directories")
    parser.add_argument("--score", type=Path, metavar="REPORT", help="Score a native recognize report against the generated manifest")
    parser.add_argument("--continuous", type=Path, metavar="VIDEO", help="Extract the 10 Hz sequence from a native 1440p game capture")
    args = parser.parse_args()
    if args.continuous:
        continuous(args.continuous)
        return
    folder = OUT / "video-holdout"
    if args.score:
        manifest = json.loads((folder / "manifest.json").read_text(encoding="utf-8"))
        predictions = json.loads(args.score.read_text(encoding="utf-8"))
        print(json.dumps(score(manifest, predictions), indent=2))
        return
    if args.recordings is None:
        parser.error("a recordings directory or --score REPORT is required")
    folder.mkdir(exist_ok=True)
    manifest = []
    source = Path(__file__).with_name("video-holdout.csv")
    with source.open(encoding="utf-8-sig") as stream:
        for row in csv.DictReader(stream):
            category = "Desktop" if row["video"].startswith("Desktop") else "Wuthering Waves"
            capture = cv2.VideoCapture(str(args.recordings / category / row["video"]))
            try:
                capture.set(cv2.CAP_PROP_POS_FRAMES, int(row["frame"]))
                ok, image = capture.read()
                if not ok or image.shape[:2] != (1440, 2560):
                    raise ValueError(f"Cannot decode expected 1440p source {row}")
            finally:
                capture.release()
            conditions = [(f"offset-{dx}-{dy}", 32+dx, 1405+dy, 608, 33) for dx, dy in OFFSETS]
            for factor in (.85, 1.15):
                width, height = round(608*factor), round(33*factor)
                top = 1405 + (33-height)//2
                conditions.append((f"scale-{factor}", 32, top, width, height))
                for dx, dy in ((-3, -2), (3, 2)):
                    conditions.append((f"combined-{factor}-{dx}-{dy}", 32+dx, top+dy, width, height))
            expected = [int(row["x"]), int(row["y"])] if row["label"] == "present" else None
            for name, left, top, width, height in conditions:
                crop = image[top:top+height, left:left+width]
                if crop.shape[:2] != (height, width):
                    continue
                path = folder / f'{row["id"]}-{name}.png'
                if not cv2.imwrite(str(path), crop):
                    raise OSError(path)
                manifest.append(dict(id=row["id"]+"/"+name, crop=str(path), expected=expected,
                                     label=row["label"], calibrated=name=="offset-0-0"))
    (folder / "manifest.json").write_text(json.dumps(manifest), encoding="utf-8")


if __name__ == "__main__":
    main()
