"""Keypoint-count and size estimate for every pack, at a given extraction tuning.

Rebuilds each pack's raster from the tiles its manifest names (see
``imao_vs_sift.load_tiles``), runs one detector configuration over it, and
reports what the result would cost in each storage format. The estimate is an
extrapolation from measured per-pack counts, not a rebuild: nothing is written
to the shipped packs.
"""

import argparse
import json
import sys
import time
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).parent))
import imao_vs_sift as probe  # noqa: E402


def current_pack_keypoints(pack_dir):
    """Keypoint count and bytes of the pack as it ships today."""
    manifest = json.loads((pack_dir / "manifest.json").read_text(encoding="utf-8"))
    count = int(manifest["features"]["keypointCount"])
    path = pack_dir / "features.imf"
    return count, path.stat().st_size if path.exists() else 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pack-root", default="Assets/FeaturesDatas/KuroTilePacks")
    parser.add_argument("--detector", default="sift-friend")
    parser.add_argument("--pack", action="append", default=None)
    parser.add_argument("--downscale", type=int, default=1,
                        help="resize the raster by 1/N before extraction (memory guard for huge maps)")
    parser.add_argument("--json", default=None)
    arguments = parser.parse_args()

    root = Path(arguments.pack_root)
    packs = arguments.pack or sorted(
        entry.name for entry in root.iterdir() if (entry / "manifest.json").exists())

    rows = []
    for pack in packs:
        pack_dir = root / pack
        manifest = json.loads((pack_dir / "manifest.json").read_text(encoding="utf-8"))
        names = [Path(t["file"]).name.split("_")[0]
                 for t in manifest["tiles"] if not t.get("absent", False)]
        base_map_id = max(set(names), key=names.count) if names else None
        tiles, _ = probe.load_tiles(pack_dir, base_map_id)
        raster, layout = probe.stitch(tiles)
        shipped_count, shipped_bytes = current_pack_keypoints(pack_dir)
        if raster is None:
            print("%-18s skipped: no tiles for map id %s" % (pack, base_map_id))
            continue
        if arguments.downscale > 1:
            import cv2
            factor = 1.0 / arguments.downscale
            raster = cv2.resize(raster, None, fx=factor, fy=factor,
                                interpolation=cv2.INTER_AREA)
            layout = dict(layout, size=[raster.shape[1], raster.shape[0]],
                          downscale=arguments.downscale)
        started = time.time()
        keypoints, descriptors = probe.extract(arguments.detector, raster)
        elapsed = time.time() - started
        if descriptors is None or not len(keypoints):
            print("%-18s no descriptors" % pack)
            continue
        columns = int(descriptors.shape[1])
        imf_bytes = probe.imf_bytes_for(keypoints, columns)
        restored, _, _ = probe.quantize_descriptors(descriptors)
        import zlib
        stored = np.clip(np.round(
            np.asarray(descriptors, dtype=np.float32)
            * (255.0 / max(float(np.asarray(descriptors).max()), 1e-9))),
            0, 255).astype(np.uint8)
        keypoint_block = np.asarray(
            [[kp.pt[0], kp.pt[1], kp.size, kp.angle, kp.response,
              float(kp.octave), float(kp.class_id)] for kp in keypoints],
            dtype=np.float32).tobytes()
        quantized = len(keypoints) * (columns + 28)
        quantized_zlib = len(zlib.compress(stored.tobytes(), 6)) + len(zlib.compress(keypoint_block, 6))
        row = {
            "pack": pack,
            "baseMapId": base_map_id,
            "raster": layout["size"],
            "tiles": layout["tiles"],
            "shippedKeypoints": shipped_count,
            "shippedBytes": shipped_bytes,
            "detectorKeypoints": int(len(keypoints)),
            "ratio": round(len(keypoints) / max(shipped_count, 1), 3),
            "rawImfBytes": int(imf_bytes),
            "quantizedImfBytes": int(quantized),
            "quantizedZlibBytes": int(quantized_zlib),
            "detectSeconds": round(elapsed, 1),
        }
        rows.append(row)
        print("%-18s raster=%-11s tiles=%-4d shipped=%-7d %s=%-7d ratio=%.2f  "
              "u8+zlib=%7.2f MB (was %7.2f MB)  %4.1fs" % (
                  pack, "x".join(str(v) for v in layout["size"]), layout["tiles"],
                  shipped_count, arguments.detector, len(keypoints), row["ratio"],
                  quantized_zlib / 1048576.0, shipped_bytes / 1048576.0, elapsed))

    if rows:
        shipped_total = sum(r["shippedKeypoints"] for r in rows)
        shipped_bytes = sum(r["shippedBytes"] for r in rows)
        detector_total = sum(r["detectorKeypoints"] for r in rows)
        zlib_total = sum(r["quantizedZlibBytes"] for r in rows)
        raw_total = sum(r["rawImfBytes"] for r in rows)
        print("-" * 108)
        print("%d packs: shipped=%d keypoints / %.1f MB   %s=%d keypoints (%.2fx)   "
              "raw .imf=%.1f MB   uint8+zlib=%.1f MB" % (
                  len(rows), shipped_total, shipped_bytes / 1048576.0,
                  arguments.detector, detector_total,
                  shipped_total / max(detector_total, 1),
                  raw_total / 1048576.0, zlib_total / 1048576.0))
    if arguments.json:
        Path(arguments.json).write_text(json.dumps(rows, indent=2), encoding="utf-8")
        print("wrote %s" % arguments.json)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
