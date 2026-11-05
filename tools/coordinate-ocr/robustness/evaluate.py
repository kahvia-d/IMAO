"""Score final XY on raw crops and the existing 2,012-item regression corpus."""
import argparse
from collections import Counter, defaultdict
import hashlib
import json
import os
import shutil
import subprocess
from pathlib import Path

from train import FLIGHT, OUT, ROOT, read


def historical_manifest():
    prior = ROOT / "out/coordinate-targeted-retrain-20261016/evaluation-inputs.json"
    truth = read(ROOT / "out/coordinate-regression-recovery-20261016/truth-reviewed-v1.json")["frames"]
    items = {r["id"]: r for r in read(prior)["inputs"] if r["id"] in truth}
    for r in read(FLIGHT / "inference-frozen-v1.json")["rows"]:
        source = r["input"]
        images = {v: str(FLIGHT / "native" / f"{source['id']}.{v}.png") for v in ("raw", "contrast")}
        items[source["sourceId"]] = dict(images=images, context=r["context"])
    assert set(items) == set(truth) and len(items) == 2012
    rows = []
    for key, row in items.items():
        context = row["context"]
        calibrated = context["frameSize"] == [2560,1440] and context["roi"] == [32,1405,608,33]
        assert all(Path(p).is_file() for p in row["images"].values())
        target = truth[key]["expected"]
        rows.append(dict(id=key, images=row["images"], observations={}, calibrated=calibrated,
                         expected=[target["x"],target["y"]] if target else None, state=truth[key]["state"]))
    return rows


def score(rows, predictions):
    grades = {}
    stats = defaultdict(Counter)
    by_id = {r["id"]:r for r in predictions}
    assert len(by_id) == len(predictions) == len(rows)
    for r in rows:
        key = r["id"]
        selected = by_id[key]["selected"]
        xy = selected["xy"] if selected else None
        if r.get("state") == "absent":
            grade = "falsePositive" if xy else "trueNegative"
        elif r["expected"] is None:
            grade = "unknownReturned" if xy else "unknownRejected"
        else:
            grade = "correct" if xy == r["expected"] else "reject" if xy is None else "wrong"
        grades[key] = dict(grade=grade, xy=xy)
        stats[key.split("/")[-1] if "/" in key else "historical"][grade] += 1
    return dict(stats=stats, grades=grades)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("model")
    sets = parser.add_mutually_exclusive_group()
    sets.add_argument("--historical", action="store_true")
    sets.add_argument("--holdout", action="store_true")
    sets.add_argument("--scale", action="store_true")
    parser.add_argument("--boundary", action="store_true")
    parser.add_argument("--official", action="store_true")
    args = parser.parse_args()
    native = OUT / ("boundary-native" if args.boundary else "native") / "IMaoCoordinateYolo.exe"
    base = ROOT / "Assets/models/coordinate-yolo"
    folder = OUT / "native-evaluation"
    model = base
    if args.model == "baseline":
        model = OUT / "flight-baseline"
        model.mkdir(exist_ok=True)
        flight = FLIGHT / "export/flight.onnx"
        assert hashlib.sha256(flight.read_bytes()).hexdigest() == "f5abe76f9e9a982c502328a8e087525399c6a57074dd251a8835771686d561e4"
        shutil.copyfile(flight, model / "coordinate.onnx")
        shutil.copyfile(base / "onnxruntime.dll", model / "onnxruntime.dll")
    else:
        model = OUT / args.model / ("model-official" if args.official else "model")
        model.mkdir(exist_ok=True)
        weights = OUT / args.model / ("train-official" if args.official else "train") / "weights/last.onnx"
        shutil.copyfile(weights, model / "coordinate.onnx")
        shutil.copyfile(base / "onnxruntime.dll", model / "onnxruntime.dll")
    manifest = folder / "manifest.json"
    name = args.model + ("-official" if args.official else "") + ("-boundary" if args.boundary else "")
    if args.historical:
        name += "-historical"
        manifest = folder / "historical.json"
        if not manifest.exists():
            manifest.write_text(json.dumps(historical_manifest()))
    if args.holdout:
        name += "-holdout"
        manifest = folder / "holdout.json"
    if args.scale:
        name += "-scale"
        manifest = folder / "scale-left.json"
    predictions = folder / f"{name}.json"
    lock = folder / f"{name}-inputs.json"
    inputs = {str(path): hashlib.sha256(path.read_bytes()).hexdigest()
              for path in (manifest, native, model / "coordinate.onnx", model / "onnxruntime.dll")}
    if not predictions.exists() or not lock.exists() or read(lock) != inputs:
        env = os.environ.copy()
        env["PATH"] = str(ROOT / "third_party/build/opencv-4.11.0/bin") + os.pathsep + env["PATH"]
        env["IMAO_OCR_BACKEND"] = "cpu"
        pending = predictions.with_suffix(".partial")
        subprocess.run([native, "infer", model, manifest, pending], check=True, env=env)
        pending.replace(predictions)
        lock.write_text(json.dumps(inputs, indent=2))
    summary = score(read(manifest), read(predictions))
    (folder / f"{name}-score.json").write_text(json.dumps(summary, indent=2))
    print(json.dumps(summary["stats"]))


if __name__ == "__main__":
    main()
