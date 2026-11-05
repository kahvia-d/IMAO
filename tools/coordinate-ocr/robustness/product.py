"""Package experimental detector + alignment parameters and replay the production entry point."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess

import cv2
import numpy as np
from evaluate import historical_manifest, score
from atlas import estimate_glyphs
from train import FLIGHT,OUT,ROOT,read


def historical_crop(row):
    key=row["id"]
    if key.startswith("session-"):
        session, number=key.rsplit("-",1)
        parent=ROOT/("out/coordinate-samples" if session.startswith("session-1F") else "out/coordinate-independent-20261016/capture")
        return cv2.imread(str(parent/session/f"frame-{number}.png"))[93:126,32:640]
    im=cv2.imread(str(ROOT/"out/coordinate-sign-layout-20261015/historical-inputs"/f"{key}.png"))
    if im.shape[:2]==(1440,2560):
        return im[1405:1438,32:640]
    return im


def templates(minimum_brightness=0):
    protocol = read(FLIGHT / "training-protocol-v2.json")
    atlas = estimate_glyphs(protocol["oldRows"] + protocol["newRows"], FLIGHT / "flight/dataset/images", minimum_brightness)
    return {ch:atlas[ch].astype(np.float32) for ch in "0128"}


def package(name, directory):
    weights=OUT/name/"train/weights"
    baseline=ROOT/"Assets/models/coordinate-yolo"
    directory.mkdir(exist_ok=True)
    for src,dst in [(weights/"last.onnx","coordinate.onnx"),(baseline/"onnxruntime.dll","onnxruntime.dll")]:
        shutil.copyfile(src,directory/dst)
    metadata=read(baseline/"model.json")
    metadata.update(model="IMao own geometry-robust coordinate YOLO",trainingSource=("164 flight sources, real-crop geometry augmentation, 70% fixed-epoch fine-tuned weights interpolated with flight"
                                    if name == "geometry-blend70" else name),
                    sourceWeightsSHA256=hashlib.sha256((weights/"last.pt").read_bytes()).hexdigest())
    metadata["rowAlignment"]={"version":1,"templates":{k:im.flatten().tolist() for k,im in templates().items()},
                              "brightTemplates":{k:im.flatten().tolist() for k,im in templates(170).items()}}
    onnx=directory/"coordinate.onnx"
    metadata["files"]["coordinate.onnx"]={"sha256":hashlib.sha256(onnx.read_bytes()).hexdigest(),"bytes":onnx.stat().st_size}
    # Keep ordinary metadata and glyph names readable without one line per float.
    alignment = metadata.pop("rowAlignment")
    text = json.dumps(metadata, indent=2)[:-2] + ',\n  "rowAlignment": {\n    "version": 1'
    for bank in ("templates", "brightTemplates"):
        arrays = [f'      "{char}": ' + json.dumps(values, separators=(",",":")) for char,values in alignment[bank].items()]
        text += f',\n    "{bank}": {{\n' + ",\n".join(arrays) + "\n    }"
    (directory/"model.json").write_text(text + "\n  }\n}\n", encoding="utf-8")


def main():
    parser=argparse.ArgumentParser()
    parser.add_argument("model")
    parser.add_argument("--build", default="consensus")
    parser.add_argument("suites",nargs="+",choices=["grid","historical","holdout","scale"])
    args=parser.parse_args()
    folder=OUT/args.build
    folder.mkdir(exist_ok=True)
    model=folder/args.model
    package(args.model,model)
    ev=OUT/"native-evaluation"
    names={"grid":"manifest.json","scale":"scale-left.json","holdout":"holdout.json","historical":"historical.json"}
    env=os.environ.copy()
    env["PATH"]=str(ROOT/"third_party/build/opencv-4.11.0/bin")+os.pathsep+env["PATH"]
    env["IMAO_OCR_BACKEND"]="cpu"
    for suite in args.suites:
        manifest=ev/names[suite]
        if suite=="historical":
            if not manifest.is_file():
                manifest.write_text(json.dumps(historical_manifest()))
            raw_manifest=folder/"historical-inputs.json"
            if not raw_manifest.exists():
                rows=read(manifest)
                for r in rows:
                    crop=historical_crop(r)
                    path=folder/(r["id"]+".png")
                    assert cv2.imwrite(str(path),crop)
                    r["crop"]=str(path)
                raw_manifest.write_text(json.dumps(rows))
            manifest=raw_manifest
        dest=folder/f"{args.model}-{suite}.json"
        pending=dest.with_suffix(".partial")
        subprocess.run([OUT/(args.build+"-native")/"IMaoCoordinateYolo.exe","recognize",model,manifest,pending],env=env,check=True)
        pending.replace(dest)
        rows=read(dest)
        summary=score(read(manifest),rows)
        summary["p95Ms"]=float(np.percentile([r["recognitionMs"] for r in rows[5:]],95))
        (folder/f"{args.model}-{suite}-score.json").write_text(json.dumps(summary,indent=2))
        print(args.model,suite,summary["stats"],"P95 ms",summary["p95Ms"],flush=True)


if __name__=="__main__":
    main()
