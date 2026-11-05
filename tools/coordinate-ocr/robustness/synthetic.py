"""Render translucent glyphs with flight annotation geometry over real HUD backgrounds."""
import os
import copy
from functools import lru_cache
from pathlib import Path

import cv2
import numpy as np
from PIL import ImageFont


def coordinate_variant(row, rng):
    row = copy.deepcopy(row)
    objects = row["objects"]
    prefix = row["text"].split()[0]
    # The original flight rows have no positive-X / negative-Y examples.
    # Use the same positions and sign shapes that the detector must distinguish.
    if prefix.startswith("-") and ",-" in prefix and prefix.split(",")[1].startswith("-") and rng.random() < .65:
        advance = objects[1]["xyxy"][0] - objects[0]["xyxy"][0]
        objects.pop(0)
        prefix = prefix[1:]
        for obj in objects[:len(prefix)]:
            obj["xyxy"][0] -= advance
            obj["xyxy"][2] -= advance
    for obj in objects[:len(prefix)]:
        if obj["char"].isdigit():
            # Preserve digit count and box size. Avoid generating leading zeros.
            digit = int(rng.integers(1 if obj["char"] != "0" else 0, 10))
            obj.update({"class": digit, "char": str(digit)})
    row["text"] = "".join(obj["char"] for obj in objects[:len(prefix)]) + row["text"][len(row["text"].split()[0]):]
    assert row["text"].replace(" ", "") == "".join(obj["char"] for obj in objects)
    return row


@lru_cache
def glyph(character, bold):
    font_path = Path(os.environ["WINDIR"]) / "Fonts" / ("timesbd.ttf" if bold else "times.ttf")
    font = ImageFont.truetype(str(font_path), 128)
    bitmap = font.getmask(character, mode="L")
    image = np.asarray(bitmap, dtype=np.uint8).reshape(bitmap.size[::-1])
    y, x = np.nonzero(image)
    return image[y.min():y.max()+1, x.min():x.max()+1]


def render(row, strip, rng):
    # Sample only above the coordinate band. Synthesize the whole line before any crop.
    scene = cv2.resize(strip[:80], (640,128), interpolation=cv2.INTER_LINEAR)
    if rng.random() < .5:
        scene = cv2.flip(scene, 1)
    mask = np.zeros((256,1280), dtype=np.float32)
    bold = bool(rng.integers(2))
    for obj in row["objects"]:
        left, top, right, bottom = obj["xyxy"]
        x = round(((left - 4) * 608/884 + 32) * 2)
        y = round((top * 33/48 + 93) * 2)
        width = max(1, round((right-left) * 608/884 * 2))
        height = max(1, round((bottom-top) * 33/48 * 2))
        # Label rectangles include a margin around the ink.
        w, h = max(1,round(width*.86)), max(1,round(height*.84))
        x += (width-w)//2
        y += (height-h)//2
        bitmap = cv2.resize(glyph(obj["char"], bold), (w,h), interpolation=cv2.INTER_AREA)
        target = mask[y:y+h,x:x+w]
        np.maximum(target, bitmap.astype(np.float32)/255, out=target)
    alpha = cv2.resize(mask,(640,128),interpolation=cv2.INTER_AREA)
    outline = cv2.GaussianBlur(cv2.dilate(alpha,np.ones((3,3),np.uint8)),(3,3),.6)
    dark = scene.astype(np.float32) * (1-rng.uniform(.12,.45)*outline[...,None])
    blend = alpha[...,None] * rng.uniform(.18,.55)
    return np.clip(dark*(1-blend)+rng.uniform(220,255)*blend,0,255).astype(np.uint8)
