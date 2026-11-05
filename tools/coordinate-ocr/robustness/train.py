"""Build reproducible flight-source ablations and export fixed-epoch candidates."""
import argparse
import hashlib
import json
import os
from pathlib import Path

import cv2
import numpy as np

ROOT = Path(__file__).resolve().parents[3]
OUT = Path(os.environ.get("IMAO_COORDINATE_RESEARCH_OUT", ROOT / "out/coordinate-robustness-v1")).resolve()
FLIGHT = ROOT / "out/coordinate-flight-intake-20261016"
PILOT = ROOT / "out/coordinate-yolo-selftrain-20261015"
SEED = 20261106
ROUTES = ("raw", "contrast")


def read(path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def prepare(crop):
    size = (round(crop.shape[1] * 48 / crop.shape[0]), 48)
    raw = cv2.resize(crop, size, interpolation=cv2.INTER_CUBIC)
    gray = cv2.resize(cv2.cvtColor(crop, cv2.COLOR_BGR2GRAY), size, interpolation=cv2.INTER_CUBIC)
    enhanced = cv2.createCLAHE(3, (4, 2)).apply(gray)
    contrast = cv2.cvtColor(cv2.normalize(enhanced, None, 0, 255, cv2.NORM_MINMAX), cv2.COLOR_GRAY2BGR)
    return [cv2.copyMakeBorder(image, 0, 0, 4, 4, cv2.BORDER_CONSTANT) for image in (raw, contrast)]


def geometry(row, strip, rng, allow_suffix_clip=False):
    # Annotated boxes refer to the 608x33 reference ROI, resized to 884x48 + border.
    boxes = np.array([obj["xyxy"] for obj in row["objects"]], dtype=float)
    boxes[:, [0, 2]] = (boxes[:, [0, 2]] - 4) * 608 / 884 + 32
    boxes[:, [1, 3]] = boxes[:, [1, 3]] * 33 / 48 + 93
    zoom = rng.uniform(.82, 1.22)
    height = round(33 * zoom)
    width = round(608 * zoom)
    left = round(32 + rng.uniform(-8, 8))
    top = round(109.5 - height / 2 + rng.uniform(-3, 3))
    # Keep all glyph centres in view; clip only annotation margins at the edges.
    if np.any(boxes[:, 0] < left + 1):
        left = int(boxes[:, 0].min()) - 1
    right = max(left + width, int(np.ceil(boxes[:, 2].max())) + 1)
    if allow_suffix_clip:
        right = left + width
    expanded = cv2.copyMakeBorder(strip, 0, 24, 0, max(0, right - strip.shape[1]), cv2.BORDER_REPLICATE)
    # Real capture detail changes with raster size, including subpixel resampling.
    scale = rng.choice([.68, .75, .875, 1., 1.125])
    crop = cv2.resize(expanded[top:top+height, left:right], None, fx=scale, fy=scale, interpolation=cv2.INTER_LINEAR)
    images = prepare(crop)
    out_width = images[0].shape[1]
    boxes[:, [0, 2]] = ((boxes[:, [0, 2]] - left) * (out_width-8)/(right-left) + 4) / out_width
    boxes[:, [1, 3]] = (boxes[:, [1, 3]] - top) / height
    visible = (boxes[:,2] > 0) & (boxes[:,0] < 1)
    boxes = np.clip(boxes, 0, 1)
    lines = []
    for obj, (x1, y1, x2, y2), keep in zip(row["objects"], boxes, visible):
        if not keep:
            continue
        assert x2 > x1 and y2 > y1
        lines.append(f"{obj['class']} {(x1+x2)/2:.8f} {(y1+y2)/2:.8f} {x2-x1:.8f} {y2-y1:.8f}")
    return images, "\n".join(lines) + "\n"


def dataset(kind, folder):
    folder.mkdir(parents=True, exist_ok=False)
    rng = np.random.default_rng(SEED)
    source = FLIGHT / "flight/dataset"
    manifest = read(FLIGHT / "training-protocol-v2.json")
    flight = next(trial for trial in manifest["trials"] if trial["trial"] == "flight")
    for patch in flight["patches"]:
        image = Path(patch["image"])
        label = image.parent.parent / "labels" / (image.stem + ".txt")
        assert hashlib.sha256(image.read_bytes()).hexdigest() == patch["imageSHA256"]
        assert hashlib.sha256(label.read_bytes()).hexdigest() == patch["labelSHA256"]
    paths = []
    rows = manifest["oldRows"] + manifest["newRows"]
    use_atlas = kind in ("atlas", "codec")
    views = 6 if use_atlas else 3
    if use_atlas:
        from atlas import estimate_glyphs
        glyphs = estimate_glyphs(rows, source / "images")
    for row in rows:
        key = row["id"]
        originals = [source / "images" / f"{key}.{route}.png" for route in ROUTES]
        assert all(path.is_file() for path in originals)
        paths.extend(originals)
        if kind == "control" or not row["objects"]:
            paths.extend(originals * views)
            continue
        session, number = key.rsplit("-", 1)
        root = ROOT / ("out/coordinate-samples" if session.startswith("session-1F") else "out/coordinate-independent-20261016/capture")
        strip = cv2.imread(str(root / session / f"frame-{number}.png"))
        assert strip.shape[:2] == (128, 640)
        ref = prepare(strip[93:126, 32:640])[0]
        assert cv2.norm(ref, cv2.imread(str(originals[0])), cv2.NORM_INF) <= 1, key
        for index in range(views):
            source_strip = strip
            target = row
            if kind == "mixed" and index == 2:
                from synthetic import coordinate_variant, render
                target = coordinate_variant(row, rng)
                source_strip = render(target, strip, rng)
            if use_atlas and index >= 3:
                from atlas import composite
                from synthetic import coordinate_variant
                target = coordinate_variant(row, rng)
                source_strip = composite(target, strip, glyphs, rng)
            if kind == "codec" and index in (1, 2, 4, 5):
                # Attenuation, sub-native rasterization and block compression occur
                # before the OCR crop; keep the original glyph coordinates.
                low = cv2.GaussianBlur(source_strip,(0,0),2.0)
                source_strip = cv2.addWeighted(source_strip, .68, low, .32, 0)
                factor = rng.uniform(.53,.90)
                small = cv2.resize(source_strip, None, fx=factor, fy=factor, interpolation=cv2.INTER_AREA)
                ok, payload = cv2.imencode(".jpg", small, [cv2.IMWRITE_JPEG_QUALITY, int(rng.integers(46,92))])
                assert ok
                source_strip = cv2.resize(cv2.imdecode(payload, cv2.IMREAD_COLOR), (640,128), interpolation=cv2.INTER_LINEAR)
            images, labels = geometry(target, source_strip, rng, allow_suffix_clip=use_atlas)
            for route, image in zip(ROUTES, images):
                path = folder / "images" / f"{key}-{index}.{route}.png"
                label = folder / "labels" / (path.stem + ".txt")
                path.parent.mkdir(exist_ok=True)
                label.parent.mkdir(exist_ok=True)
                assert cv2.imwrite(str(path), image)
                label.write_text(labels)
                paths.append(path)
    listing = folder / "train.txt"
    listing.write_text("\n".join(p.as_posix() for p in paths) + "\n")
    yaml = (FLIGHT / "flight/data.yaml").read_text().split("names:", 1)[1]
    config = folder / "data.yaml"
    config.write_text(f"path: {folder.as_posix()}\ntrain: train.txt\nval: train.txt\nnames:{yaml}")
    return config


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("kind", choices=["control", "geometry", "mixed", "atlas", "codec"])
    parser.add_argument("--initial", choices=["flight", "official"], default="flight")
    args = parser.parse_args()
    os.environ.update(YOLO_CONFIG_DIR=str(PILOT / "settings"), YOLO_AUTOINSTALL="false", CUBLAS_WORKSPACE_CONFIG=":4096:8")
    from ultralytics import YOLO
    import torch
    torch.set_num_threads(4)
    folder = OUT / args.kind
    config = folder / "data.yaml"
    if not config.exists():
        config = dataset(args.kind, folder)
    assert len((folder / "train.txt").read_text().splitlines()) == (2296 if args.kind in ("atlas","codec") else 1312)
    baseline = read(FLIGHT / "flight-training.json")
    initial = Path(baseline["weights"])
    assert hashlib.sha256(initial.read_bytes()).hexdigest() == baseline["weightsSHA256"]
    if args.initial == "official":
        initial = PILOT / "official/yolo11n.pt"
        assert hashlib.sha256(initial.read_bytes()).hexdigest() == "0ebbc80d4a7680d14987a577cd21342b65ecfd94632bd9a8da63ae6417644ee1"
    run = "train" if args.initial == "flight" else "train-official"
    assert not (folder / run).exists()
    train_args = baseline["args"]
    train_args.update(data=str(config), project=str(folder), name=run, epochs=16, seed=SEED, batch=16, lr0=.00025, val=False)
    if args.kind == "codec":
        initial = OUT / "geometry-blend70/train/weights/last.pt"
        train_args["lr0"] = .00008
    YOLO(str(initial)).train(**train_args)
    YOLO(str(folder / run / "weights/last.pt")).export(format="onnx", imgsz=(64, 896), opset=12, dynamic=False, simplify=False, half=False, batch=1, device="cpu")


if __name__ == "__main__":
    main()
