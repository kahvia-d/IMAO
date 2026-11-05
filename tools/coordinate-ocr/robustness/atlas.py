"""Extract game glyph appearance from annotated flight crops (training sources only)."""
from collections import defaultdict
import cv2
import numpy as np


def estimate_glyphs(rows, images, minimum_brightness=0):
    samples = defaultdict(list)
    for row in rows:
        if not row["objects"]:
            continue
        image = cv2.imread(str(images / (row["id"] + ".raw.png")), cv2.IMREAD_GRAYSCALE).squeeze().astype(np.float32)
        detail = image - cv2.GaussianBlur(image, (0,0), 3)
        for obj in row["objects"]:
            x1,y1,x2,y2 = np.rint(obj["xyxy"]).astype(int)
            if image[y1:y2,x1:x2].mean() < minimum_brightness:
                continue
            patch = cv2.resize(detail[y1:y2,x1:x2], (32,48))
            amplitude = np.percentile(np.abs(patch), 95)
            if amplitude >= 2:
                samples[obj["char"]].append(patch/amplitude)
    assert set(samples) == set("0123456789-,+:")
    return {char: np.median(values, axis=0) for char,values in samples.items()}


def composite(row, strip, atlas, rng):
    scene = cv2.resize(strip[:80], (640,128)).astype(np.float32)
    if rng.random()<.5:
        scene = cv2.flip(scene,1)
    white = np.zeros((256,1280), np.float32)
    black = np.zeros_like(white)
    for obj in row["objects"]:
        a,t,b,d = obj["xyxy"]
        left, right = np.rint((np.array([a,b])-4) * (2*608/884) + 64).astype(int)
        top, bottom = np.rint(np.array([t,d]) * (2*33/48) + 186).astype(int)
        stamp = cv2.resize(atlas[obj["char"]], (right-left, bottom-top))
        light, shadow = white[top:bottom,left:right], black[top:bottom,left:right]
        np.maximum(light, stamp, out=light)
        np.maximum(shadow, -stamp, out=shadow)
    # Positive strokes reveal the light text; negative strokes darken the scene.
    ink = cv2.resize(white, (640,128), interpolation=cv2.INTER_AREA)[...,None]
    edge = cv2.resize(black, (640,128), interpolation=cv2.INTER_AREA)[...,None]
    opacity = rng.uniform(.17,.68)
    scene *= 1-np.clip(edge*opacity*rng.uniform(.5,1.3),0,1)
    ink = np.clip(ink*opacity, 0, 1)
    return np.uint8(np.clip(scene*(1-ink)+245*ink,0,255))
