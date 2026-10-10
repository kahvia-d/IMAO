"""How much of each map can a query-sized crop even be answered on?

The synthetic-crop protocol draws 184px crops and asks whether they localize.
A crop whose box holds fewer reference keypoints than the pass criterion needs
(8 inliers) cannot be scored by any detector, so the pass rate over uniformly
drawn crops is dominated by how much of the map carries content. This measures
that directly, for both the candidate detector (per-tile SIFT) and the one that
ships today (SURF, mapped out of the pack's app-map coordinates).

    python coverage_windows.py [--crop 184] [--json measurements/coverage-windows.json]
"""
import argparse
import json
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import imao_vs_sift as H  # noqa: E402

TILE_SIZE = 1024.0
VIRTUAL_MAP_SIZE = 850.0
PASS_INLIERS = 8


def app_to_raster(origin_x, origin_y, scale, px, py):
    """Inverse of KuroTilePointToAppMap (tools/KuroMapFeatureBuilder/main.cpp:138)."""
    world_x = (px - origin_x) / scale
    world_y = (py - origin_y) / scale
    kuro_x = world_x * TILE_SIZE / VIRTUAL_MAP_SIZE + TILE_SIZE
    kuro_y = -world_y * TILE_SIZE / VIRTUAL_MAP_SIZE
    tile_x = int(np.floor(kuro_x / TILE_SIZE))
    tile_y = int(np.ceil(kuro_y / TILE_SIZE))
    return tile_x, tile_y, kuro_x - tile_x * TILE_SIZE, tile_y * TILE_SIZE - kuro_y


def map_pack_points(pack_dir, manifest, origins):
    """Shipped keypoints in raster pixels, plus how many failed to land."""
    transform = manifest["coordinateTransform"]
    keypoints, _, _ = H.load_imao_features(pack_dir / "features.imf")
    mapped = []
    dropped = 0
    for keypoint in keypoints:
        tile_x, tile_y, pixel_x, pixel_y = app_to_raster(
            transform["originX"], transform["originY"], transform["scale"],
            keypoint.pt[0], keypoint.pt[1])
        corner = origins.get((tile_x, tile_y))
        if corner is None:
            dropped += 1
            continue
        mapped.append((corner[0] + pixel_x, corner[1] + pixel_y))
    return np.array(mapped, dtype=np.float64) if mapped else np.empty((0, 2)), dropped


def window_fraction(points, height, width, crop, minimum):
    """Fraction of a non-overlapping lattice whose windows hold >= minimum points.

    The sweep draws crops at arbitrary positions, not on this lattice, so each
    point is assigned to the lattice cell it falls in. That quantises window
    membership by at most one crop width, which shifts both detectors the same
    way - the comparison between them is what this is for, not the third digit.
    A crop of `crop` px starting at a multiple of `crop` fits while its start is
    <= size - crop, so there are (size - 1) // crop windows per axis.
    """
    if len(points) == 0:
        return 0.0, 0, 0
    columns = (width - 1) // crop
    rows = (height - 1) // crop
    if columns <= 0 or rows <= 0:
        return 0.0, 0, 0
    column = np.floor(points[:, 0] / crop).astype(np.int64)
    row = np.floor(points[:, 1] / crop).astype(np.int64)
    keep = (column >= 0) & (column < columns) & (row >= 0) & (row < rows)
    flat = row[keep] * columns + column[keep]
    counts = np.bincount(flat, minlength=rows * columns)
    return float((counts >= minimum).mean()), int((counts >= minimum).sum()), rows * columns


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--crop", type=int, default=184)
    parser.add_argument("--minimum", type=int, default=PASS_INLIERS)
    parser.add_argument("--pack-root", default="Assets/FeaturesDatas/KuroTilePacks")
    parser.add_argument("--registry", default="Assets/FeaturesDatas/kuro-tile-packs.json")
    parser.add_argument("--json", default="measurements/coverage-windows.json")
    arguments = parser.parse_args()

    registry = json.loads(Path(arguments.registry).read_text(encoding="utf-8"))
    rows = []
    for name in registry["packs"]:
        pack_dir = Path(arguments.pack_root) / name
        manifest = json.loads((pack_dir / "manifest.json").read_text(encoding="utf-8"))
        names = [Path(t["file"]).name.split("_")[0]
                 for t in manifest["tiles"] if not t.get("absent", False)]
        surface = [item for item in names if item.isdigit()]
        map_id = max(set(surface), key=surface.count) if surface else None
        tiles, _ = H.load_tiles(pack_dir, map_id)
        raster, layout, origins = H.stitch(tiles)
        if raster is None:
            print("%-16s no tiles for map id %s" % (name, map_id))
            continue
        height, width = raster.shape[:2]

        sift_keypoints, _ = H.extract_per_tile("sift-friend", tiles, origins, raster.shape[:2])
        sift_xy = np.array([kp.pt for kp in sift_keypoints], dtype=np.float64)
        surf_xy, dropped = map_pack_points(pack_dir, manifest, origins)

        sift_fraction, sift_windows, total = window_fraction(
            sift_xy, height, width, arguments.crop, arguments.minimum)
        surf_fraction, surf_windows, _ = window_fraction(
            surf_xy, height, width, arguments.crop, arguments.minimum)

        rows.append(dict(
            pack=name, raster=[width, height], mpx=round(width * height / 1e6, 1),
            crop=arguments.crop, minimum=arguments.minimum, windows=total,
            siftKeypoints=len(sift_xy), surfKeypoints=len(surf_xy), surfDropped=dropped,
            siftAnswerableWindows=sift_windows, surfAnswerableWindows=surf_windows,
            siftAnswerableFraction=round(sift_fraction, 4),
            surfAnswerableFraction=round(surf_fraction, 4)))
        print("%-16s %6d windows  answerable: SIFT %5.1f%%  SURF %5.1f%%   (kp/Mpx SIFT %5.1f SURF %5.1f)" % (
            name, total, 100 * sift_fraction, 100 * surf_fraction,
            len(sift_xy) / (width * height / 1e6), len(surf_xy) / (width * height / 1e6)))

    Path(arguments.json).write_text(json.dumps(
        dict(generatedBy="tools/feature-probe/coverage_windows.py",
             crop=arguments.crop, minimum=arguments.minimum, packs=rows), indent=2),
        encoding="utf-8")
    print("wrote %s" % arguments.json)


if __name__ == "__main__":
    main()
