"""Detector and budget comparison for map localization features.

The script rebuilds the same stitched raster IMAO's feature packs are built
from (Kuro official map tiles, 1024 px each, laid out by tile index), then
measures four extraction configurations on that identical input:

  surf-current   IMAO's shipped tuning      SURF(40, 8, 4, extended, upright)
  surf-grid      the same SURF plus a per-cell cap
  sift-friend    the tuning wuwa-map ships  SIFT(ct 0.03, et 10, layers 2)
  sift-grid      that SIFT plus the per-cell cap

For each configuration it reports the keypoint count, what the pack would
cost on disk in IMAO's uncompressed IMAOF T01 format, and how well synthetic
minimap-sized crops of the same raster still localize against that feature
set (ratio test, homography, inlier count, centre error in pixels).
"""

import argparse
import json
import time
from pathlib import Path

import cv2
import numpy as np

# wuwa-map's shipped parameter set:
#   siftgz_ct30_et10_ol2_s16_g150_mpc200_r75_ds1_ts7000_to512_bt5_em8_md40
FRIEND_SIFT = dict(nfeatures=0, nOctaveLayers=2, contrastThreshold=0.03,
                   edgeThreshold=10, sigma=1.6)
GRID_CELLS = 150          # rows = cols = 150 cells over the raster
MAX_PER_CELL = 200        # wuwa-map's max_per_cell

# IMAO's IMAOFT01 record: 128 float32 descriptors plus 28 bytes of keypoint.
IMAO_DESCRIPTOR_COLUMNS = 128
IMAO_BYTES_PER_KEYPOINT = 540


def load_tiles(pack_dir, base_map_id=None, tile_root_override=None):
    """Return {(x, y): Mat} for one map id, using the pack manifest as the index.

    A pack does not ship its own tiles. `manifest.json` lists them as
    ``tiles/<x>_<y>.png`` under ``map-regions/tiles/<resourceVersion>/<mapId>/``,
    and `resourceVersion` is the tile-bundle directory that holds the PNGs.
    """
    manifest = json.loads((pack_dir / "manifest.json").read_text(encoding="utf-8"))
    if tile_root_override is not None:
        tile_root = Path(tile_root_override)
    else:
        tile_root = Path("map-regions/tiles") / manifest["resourceVersion"]
    tiles = {}
    if not tile_root.is_dir():
        return tiles
    for entry in manifest.get("tiles", []):
        # `absent: true` means the pack does not ship this tile (no map art there),
        # so only the present ones take part in the raster.
        if entry.get("absent", False):
            continue
        relative = entry["file"]                      # tiles/[<mapId>/]<mapId>_<x>_<y>.png
        name = Path(relative).name
        map_id = name.split("_")[0]
        if base_map_id and map_id != str(base_map_id):
            continue
        # The same manifest path is served by two different on-disk layouts:
        # tiles/900/0_1.png (a per-map subdirectory) and tiles/8_-3_1.png (flat).
        candidates = [
            tile_root / relative,
            tile_root / name,
            tile_root / map_id / name,
            tile_root / "tiles" / name,
        ]
        image = None
        for candidate in candidates:
            image = cv2.imread(str(candidate), cv2.IMREAD_GRAYSCALE)
            if image is not None:
                break
        if image is None:
            continue
        tiles[(int(entry["x"]), int(entry["y"]))] = image
    return tiles


def stitch(parsed):
    """Lay tiles out on a grid keyed by their (x, y) index and merge them."""
    if not parsed:
        return None, None
    xs = sorted({key[0] for key in parsed})
    ys = sorted({key[1] for key in parsed})
    tile_h, tile_w = next(iter(parsed.values())).shape[:2]
    canvas = np.zeros((len(ys) * tile_h, len(xs) * tile_w), dtype=np.uint8)
    x_index = {value: index for index, value in enumerate(xs)}
    y_index = {value: index for index, value in enumerate(ys)}
    present = 0
    for (x, y), image in parsed.items():
        top = y_index[y] * tile_h
        left = x_index[x] * tile_w
        canvas[top:top + image.shape[0], left:left + image.shape[1]] = image
        present += 1
    return canvas, {"tiles": present, "grid": [len(xs), len(ys)],
                    "size": [canvas.shape[1], canvas.shape[0]]}


def grid_sample(keypoints, descriptors, shape, cells=GRID_CELLS, max_per_cell=MAX_PER_CELL):
    """Keep the strongest max_per_cell keypoints in each grid cell."""
    if len(keypoints) == 0:
        return keypoints, descriptors
    height, width = shape[:2]
    cell_h = height / float(cells)
    cell_w = width / float(cells)
    order = sorted(range(len(keypoints)), key=lambda i: -keypoints[i].response)
    buckets = {}
    for index in order:
        point = keypoints[index].pt
        column = int(point[0] / cell_w)
        row = int(point[1] / cell_h)
        if 0 <= column < cells and 0 <= row < cells:
            buckets.setdefault((row, column), []).append(index)
    keep = []
    for bucket in buckets.values():
        keep.extend(bucket[:max_per_cell])
    keep.sort()
    return [keypoints[i] for i in keep], descriptors[keep]


def extract(name, raster):
    if name == "surf-current":
        detector = cv2.xfeatures2d.SURF_create(40, 8, 4, True, True)
        keypoints, descriptors = detector.detectAndCompute(raster, None)
        return keypoints, descriptors
    if name == "surf-grid":
        detector = cv2.xfeatures2d.SURF_create(40, 8, 4, True, True)
        keypoints, descriptors = detector.detectAndCompute(raster, None)
        return grid_sample(keypoints, descriptors, raster.shape)
    if name == "sift-friend":
        detector = cv2.SIFT_create(**FRIEND_SIFT)
        keypoints, descriptors = detector.detectAndCompute(raster, None)
        return keypoints, descriptors
    if name == "sift-grid":
        detector = cv2.SIFT_create(**FRIEND_SIFT)
        keypoints, descriptors = detector.detectAndCompute(raster, None)
        return grid_sample(keypoints, descriptors, raster.shape)
    if name == "sift-default":
        detector = cv2.SIFT_create()
        keypoints, descriptors = detector.detectAndCompute(raster, None)
        return keypoints, descriptors
    raise ValueError("unknown detector configuration: %s" % name)


def localize(query, reference_keypoints, reference_descriptors, detector,
             ratio=0.75, min_inliers=8):
    """Match one query crop against a reference feature set."""
    if reference_descriptors is None or len(reference_descriptors) == 0:
        return None
    query_keypoints, query_descriptors = detector.detectAndCompute(query, None)
    if query_descriptors is None or len(query_descriptors) < 2:
        return {"queryKeypoints": 0, "good": 0, "inliers": 0, "error": None}
    matcher = cv2.BFMatcher(cv2.NORM_L2)
    knn = matcher.knnMatch(query_descriptors, reference_descriptors, 2)
    source, target = [], []
    for pair in knn:
        if len(pair) < 2:
            continue
        best, second = pair
        if best.distance < ratio * second.distance:
            source.append(query_keypoints[best.queryIdx].pt)
            target.append(reference_keypoints[best.trainIdx].pt)
    result = {"queryKeypoints": int(len(query_keypoints)),
              "good": len(source), "inliers": 0, "error": None}
    if len(source) < 4:
        return result
    homography, mask = cv2.findHomography(
        np.float32(source).reshape(-1, 1, 2),
        np.float32(target).reshape(-1, 1, 2),
        cv2.RANSAC, 3.0, maxIters=2000, confidence=0.995)
    if homography is None or mask is None:
        return result
    result["inliers"] = int(mask.sum())
    if result["inliers"] >= min_inliers:
        centre = np.float32([[[query.shape[1] / 2.0, query.shape[0] / 2.0]]])
        projected = cv2.perspectiveTransform(centre, homography)[0, 0]
        result["centre"] = [float(projected[0]), float(projected[1])]
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pack-root", default="Assets/FeaturesDatas/KuroTilePacks")
    parser.add_argument("--pack", action="append", required=True,
                        help="pack directory name, e.g. jinzhou")
    parser.add_argument("--base-map-id", default=None,
                        help="only stitch tiles whose name starts with this map id")
    parser.add_argument("--config", action="append", default=None)
    parser.add_argument("--queries", type=int, default=6, help="synthetic crops per pack")
    parser.add_argument("--crop", type=int, default=184, help="crop edge, like a minimap")
    parser.add_argument("--json", default=None)
    arguments = parser.parse_args()

    configs = arguments.config or ["surf-current", "surf-grid", "sift-friend", "sift-grid"]
    report = {}
    for pack in arguments.pack:
        pack_dir = Path(arguments.pack_root) / pack
        base_map_id = arguments.base_map_id
        if base_map_id is None and pack == "jinzhou":
            base_map_id = "8"
        if base_map_id is None:
            manifest = json.loads((pack_dir / "manifest.json").read_text(encoding="utf-8"))
            names = [t["file"].split("/")[-1].split("_")[0]
                     for t in manifest["tiles"] if not t.get("absent", True)]
            if names:
                base_map_id = max(set(names), key=names.count)
        tiles = load_tiles(pack_dir, base_map_id)
        raster, layout = stitch(tiles)
        if raster is None:
            print("%s: no tiles found for map id %s" % (pack, base_map_id))
            continue
        print("== %s (map id %s): %d tiles -> %dx%d raster" % (
            pack, base_map_id, layout["tiles"], layout["size"][0], layout["size"][1]))

        # Synthetic queries: crops taken away from the raster border.
        rng = np.random.default_rng(20261002)
        queries = []
        margin = arguments.crop
        for _ in range(arguments.queries * 3):
            if raster.shape[0] < 3 * margin or raster.shape[1] < 3 * margin:
                break
            top = int(rng.integers(margin, raster.shape[0] - margin))
            left = int(rng.integers(margin, raster.shape[1] - margin))
            queries.append((raster[top:top + arguments.crop, left:left + arguments.crop],
                            (left + arguments.crop / 2.0, top + arguments.crop / 2.0)))
            if len(queries) >= arguments.queries:
                break

        report[pack] = {"baseMapId": base_map_id, "layout": layout, "configs": {}}
        for name in configs:
            started = time.time()
            keypoints, descriptors = extract(name, raster)
            elapsed = time.time() - started
            if descriptors is None:
                print("   %-14s no descriptors" % name)
                continue
            descriptor_columns = int(descriptors.shape[1])
            imf_bytes = len(keypoints) * (descriptor_columns * 4 + 28)
            gz_bytes = len(keypoints) * 168  # measured wuwa-map bytes per keypoint
            detector = (cv2.xfeatures2d.SURF_create(40, 8, 4, True, True)
                        if name.startswith("surf") else cv2.SIFT_create(**FRIEND_SIFT))
            trials = []
            for crop, centre in queries:
                outcome = localize(crop, keypoints, descriptors, detector) or {}
                if outcome.get("centre"):
                    error = float(np.hypot(outcome["centre"][0] - centre[0],
                                           outcome["centre"][1] - centre[1]))
                    outcome["error"] = error
                trials.append(outcome)
            verified = [t for t in trials if t.get("inliers", 0) >= 8 and t.get("error") is not None
                        and t["error"] <= 4.0]
            inliers = [t.get("inliers", 0) for t in trials]
            print("   %-14s kp=%-7d cols=%-4d imf=%7.2f MB  gz=%6.2f MB  detect=%5.1fs  "
                  "verified=%d/%d  inliers median=%s" % (
                      name, len(keypoints), descriptor_columns, imf_bytes / 1048576.0,
                      gz_bytes / 1048576.0, elapsed, len(verified), len(trials),
                      int(np.median(inliers)) if inliers else 0))
            report[pack]["configs"][name] = {
                "keypoints": int(len(keypoints)),
                "descriptorColumns": descriptor_columns,
                "imfBytes": int(imf_bytes),
                "gzipEstimateBytes": int(gz_bytes),
                "detectSeconds": round(elapsed, 2),
                "queries": len(trials),
                "verified": len(verified),
                "inliers": inliers,
                "errors": [t.get("error") for t in trials],
            }

    if arguments.json:
        Path(arguments.json).write_text(json.dumps(report, indent=2), encoding="utf-8")
        print("wrote %s" % arguments.json)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
