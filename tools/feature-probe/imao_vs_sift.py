"""Compare IMAO's shipped SURF feature set against SIFT on the same raster.

IMao builds its packs from the Kuro official map tiles (1024 px, laid out by
tile index) and stores every detected SURF keypoint in an uncompressed
``IMAOFT01`` binary. This script rebuilds that exact raster from
``map-regions/tiles``, reads the pack's shipped descriptors straight out of
``features.imf``, and then answers two questions with one measurement:

  * how many keypoints would SIFT produce on the same raster (so the size
    claim can be checked), and
  * how well do synthetic minimap-sized crops of that raster still localize
    against the sparse SIFT set versus the shipped dense SURF set.

The crops are taken from the same raster at known positions, so a verified
localization has a measurable centre error in raster pixels.
"""

import argparse
import json
import struct
import time
import zlib
from pathlib import Path

import cv2
import numpy as np

IMAO_MAGIC = b"IMAOFT01"
IMAO_HEADER_BYTES = 116
IMAO_KEYPOINT_BYTES = 28

# wuwa-map's shipped parameter set:
#   siftgz_ct30_et10_ol2_s16_g150_mpc200_r75_ds1_ts7000_to512_bt5_em8_md40
FRIEND_SIFT = dict(nfeatures=0, nOctaveLayers=2, contrastThreshold=0.03,
                   edgeThreshold=10, sigma=1.6)
GRID_CELLS = 150
MAX_PER_CELL = 200

# Fair-query gate for the synthetic-crop protocol, used by --gate texture.
# The 2026-10-08 protocol screened candidate crops on brightness spread
# (std >= 18), which a smooth water gradient satisfies while carrying no
# features at all - that is where fabricatorium's 4/12 came from. Structure is
# what a detector can actually use, so this gate measures mean |Laplacian|
# instead. Calibrated on 2026-10-10 over 400-candidate samples from three packs;
# its residual (long smooth edges still score as structure) is recorded in
# Docs/OpenWork.md rather than tuned away here.
TEXTURE_GATE = 6.0


def load_imao_features(path):
    """Read an IMAOFT01 binary into cv2 keypoints plus a descriptor matrix.

    The header is written field by field (magic, seven uint32 fields, two
    uint64 payload lengths, then two 32-byte hashes), so it is read with
    explicit offsets: in-process struct padding does not match the file.

    Both on-disk versions are supported. v1 stores 128 float32 descriptors per
    keypoint, uncompressed. v2 stores 128 uint8 codes against one per-file
    scale/offset and deflates both payloads; restoring a descriptor is
    ``(code + descriptorOffset) * descriptorScale``, exactly as
    FeatureBinaryCodec::Load does. Keypoints are the same 28 bytes either way,
    so the two versions carry identical keypoints.
    """
    raw = Path(path).read_bytes()
    if raw[:8] != IMAO_MAGIC:
        raise ValueError("%s is not an IMAOFT01 binary" % path)
    version = struct.unpack_from("<I", raw, 8)[0]
    keypoint_count = struct.unpack_from("<I", raw, 20)[0]
    rows = struct.unpack_from("<I", raw, 24)[0]
    columns = struct.unpack_from("<I", raw, 28)[0]
    descriptor_type = struct.unpack_from("<I", raw, 32)[0]
    keypoint_payload = struct.unpack_from("<Q", raw, 36)[0]
    descriptor_payload = struct.unpack_from("<Q", raw, 44)[0]
    if rows != keypoint_count:
        raise ValueError("unsupported pack shape: rows=%d keypoints=%d" % (rows, keypoint_count))
    if version == 1:
        header_bytes = 116
        if descriptor_type != 1:
            raise ValueError("v1 pack has descriptor type %d" % descriptor_type)
        if keypoint_payload != keypoint_count * IMAO_KEYPOINT_BYTES:
            raise ValueError("unexpected keypoint payload length %d" % keypoint_payload)
        stored_keypoints = raw[header_bytes:header_bytes + keypoint_payload]
        descriptor_start = header_bytes + keypoint_payload
        stored = raw[descriptor_start:descriptor_start + descriptor_payload]
        descriptors = np.frombuffer(stored, dtype=np.float32).reshape(keypoint_count, columns)
    elif version == 2:
        header_bytes = struct.unpack_from("<I", raw, 12)[0]
        if descriptor_type != 2:
            raise ValueError("v2 pack has descriptor type %d" % descriptor_type)
        scale = struct.unpack_from("<f", raw, 52)[0]
        code_offset = struct.unpack_from("<i", raw, 64)[0]
        stored_keypoints = zlib.decompress(raw[header_bytes:header_bytes + keypoint_payload])
        codes = np.frombuffer(
            zlib.decompress(raw[header_bytes + keypoint_payload:
                                header_bytes + keypoint_payload + descriptor_payload]),
            dtype=np.uint8).reshape(keypoint_count, columns)
        descriptors = (codes.astype(np.float32) + code_offset) * scale
    else:
        raise ValueError("unsupported IMAOFT01 version %d" % version)
    if len(stored_keypoints) != keypoint_count * IMAO_KEYPOINT_BYTES:
        raise ValueError("keypoint payload is %d bytes, expected %d" % (
            len(stored_keypoints), keypoint_count * IMAO_KEYPOINT_BYTES))
    keypoints = []
    for index in range(keypoint_count):
        offset = index * IMAO_KEYPOINT_BYTES
        x, y, size, angle, response = struct.unpack_from("<5f", stored_keypoints, offset)
        octave, class_id = struct.unpack_from("<2i", stored_keypoints, offset + 20)
        keypoints.append(cv2.KeyPoint(x, y, size, angle, response, octave, class_id))
    return keypoints, descriptors, {"version": version, "columns": columns}


def load_tiles(pack_dir, base_map_id=None):
    """Return {(x, y): Mat} for one map id, using the pack manifest as the index."""
    manifest = json.loads((pack_dir / "manifest.json").read_text(encoding="utf-8"))
    tile_root = Path("map-regions/tiles") / manifest["resourceVersion"]
    tiles = {}
    if not tile_root.is_dir():
        return tiles, manifest
    for entry in manifest.get("tiles", []):
        # `absent: true` means the pack ships no art for that tile.
        if entry.get("absent", False):
            continue
        name = Path(entry["file"]).name
        map_id = name.split("_")[0]
        if base_map_id and map_id != str(base_map_id):
            continue
        image = None
        for candidate in (tile_root / map_id / name, tile_root / name,
                          tile_root / entry["file"]):
            image = cv2.imread(str(candidate), cv2.IMREAD_GRAYSCALE)
            if image is not None:
                break
        if image is not None:
            tiles[(int(entry["x"]), int(entry["y"]))] = image
    return tiles, manifest


def stitch(parsed):
    """Lay tiles out on a grid keyed by their (x, y) index and merge them.

    Also returns where each tile landed, so a detector can run tile by tile and
    still report one coordinate frame - which is what production does.
    """
    if not parsed:
        return None, None, None
    xs = sorted({key[0] for key in parsed})
    ys = sorted({key[1] for key in parsed})
    tile_h, tile_w = next(iter(parsed.values())).shape[:2]
    canvas = np.zeros((len(ys) * tile_h, len(xs) * tile_w), dtype=np.uint8)
    x_index = {value: index for index, value in enumerate(xs)}
    y_index = {value: index for index, value in enumerate(ys)}
    origins = {}
    for (x, y), image in parsed.items():
        top = y_index[y] * tile_h
        left = x_index[x] * tile_w
        canvas[top:top + image.shape[0], left:left + image.shape[1]] = image
        origins[(x, y)] = (left, top)
    return canvas, {"tiles": len(parsed), "grid": [len(xs), len(ys)],
                    "size": [canvas.shape[1], canvas.shape[0]]}, origins


def make_detector(name):
    if name == "sift-friend":
        return cv2.SIFT_create(**FRIEND_SIFT)
    if name == "sift-default":
        return cv2.SIFT_create()
    if name == "sift-dense":
        return cv2.SIFT_create(nfeatures=0, nOctaveLayers=3, contrastThreshold=0.01,
                               edgeThreshold=10, sigma=1.6)
    raise ValueError("unknown detector: %s" % name)


def grid_sample(keypoints, descriptors, shape, cells=GRID_CELLS, max_per_cell=MAX_PER_CELL,
                bounds=None):
    """Keep the strongest max_per_cell keypoints in each grid cell.

    `bounds` is (min_x, min_y, size_x, size_y) in the keypoints' own space and
    defaults to the image rectangle. The default is right for features detected
    on the raster, and wrong for a shipped pack: those keypoints are stored in
    app-map coordinates, so gridding them against the raster rectangle keeps
    nothing and reports a cap that never happened. KuroTilePointToAppMap is a
    uniform scale plus a translation, so a uniform grid over the keypoints' own
    bounding box is the same partition the raster grid would impose.
    """
    if len(keypoints) == 0:
        return keypoints, descriptors
    height, width = shape[:2]
    if bounds is None:
        min_x, min_y, size_x, size_y = 0.0, 0.0, float(width), float(height)
    else:
        min_x, min_y, size_x, size_y = bounds
    if size_x <= 0 or size_y <= 0:
        return keypoints, descriptors
    cell_h = size_y / float(cells)
    cell_w = size_x / float(cells)
    order = sorted(range(len(keypoints)), key=lambda i: -keypoints[i].response)
    buckets = {}
    for index in order:
        point = keypoints[index].pt
        column = int((point[0] - min_x) / cell_w)
        row = int((point[1] - min_y) / cell_h)
        if 0 <= column < cells and 0 <= row < cells:
            buckets.setdefault((row, column), []).append(index)
    keep = []
    for bucket in buckets.values():
        keep.extend(bucket[:max_per_cell])
    keep.sort()
    return [keypoints[i] for i in keep], descriptors[keep]


def keypoint_bounds(keypoints):
    """(min_x, min_y, size_x, size_y) of a keypoint set, in its own space."""
    xs = [point.pt[0] for point in keypoints]
    ys = [point.pt[1] for point in keypoints]
    return (min(xs), min(ys), max(xs) - min(xs), max(ys) - min(ys))


def crop_mean_laplacian(raster, top, left, size, margin=2):
    """Mean |Laplacian| over one crop, without materialising a whole-raster map.

    A Laplacian taken on the crop alone differs from the same box sliced out of
    a whole-raster Laplacian in its outermost rows, and the calibrated threshold
    was measured the second way. Padding the patch by the kernel radius removes
    the difference - and a float32 Laplacian of jinzhou's 415 Mpx raster would
    be 1.7 GB, so slicing one is not an option.
    """
    height, width = raster.shape[:2]
    top0, left0 = max(top - margin, 0), max(left - margin, 0)
    top1, left1 = min(top + size + margin, height), min(left + size + margin, width)
    laplacian = np.abs(cv2.Laplacian(raster[top0:top1, left0:left1].astype(np.float32), cv2.CV_32F))
    return float(laplacian[top - top0:top - top0 + size, left - left0:left - left0 + size].mean())


def gate_accepts(gate, raster, top, left, size, points=None, required=8):
    """Is this candidate crop a fair query? What "fair" means is the argument.

    Every gate starts from the same test - a mostly black crop is not map art -
    and then disagrees about what else a query needs:

    art         the 2026-10-08 protocol: brightness spread (std >= 18). A smooth
                water gradient passes it holding no features at all, which is
                how fabricatorium's 4/12 came about, so this gate over-admits.
    texture     structure instead of spread: mean |Laplacian| >= TEXTURE_GATE.
                Better than 'art', but a long smooth edge still counts as
                structure while a detector rejects it.
    answerable  the reference set must carry at least as many keypoints inside
                the crop as the pass criterion needs. This is the only gate that
                guarantees the question has an answer; three texture proxies
                (std, mean |Laplacian|, corner pixels) were calibrated on
                2026-10-10 and none of them separated cleanly.
    """
    crop = raster[top:top + size, left:left + size]
    if int(np.count_nonzero(crop > 8)) < 0.4 * crop.size:
        return False
    if gate == "art":
        return float(crop.std()) >= 18.0
    if gate == "texture":
        return crop_mean_laplacian(raster, top, left, size) >= TEXTURE_GATE
    if gate == "answerable":
        if points is None or len(points) == 0:
            return False
        inside = ((points[:, 0] >= left) & (points[:, 0] < left + size) &
                  (points[:, 1] >= top) & (points[:, 1] < top + size))
        return int(np.count_nonzero(inside)) >= required
    raise ValueError("unknown gate: %s" % gate)


def extract(name, raster, with_grid=False, grid=GRID_CELLS, max_per_cell=MAX_PER_CELL):
    detector = make_detector(name)
    keypoints, descriptors = detector.detectAndCompute(raster, None)
    if descriptors is None:
        return [], None
    if with_grid:
        keypoints, descriptors = grid_sample(keypoints, descriptors, raster.shape[:2],
                                             cells=grid, max_per_cell=max_per_cell)
    return keypoints, descriptors


def extract_per_tile(name, parsed, origins, canvas_shape, with_grid=False,
                     grid=GRID_CELLS, max_per_cell=MAX_PER_CELL):
    """Detect inside every 1024px tile, then move the keypoints into canvas space.

    This is the production protocol: tools/KuroMapFeatureBuilder/main.cpp opens
    the pack's tiles one at a time and runs detectAndCompute on each 1024x1024
    image. Running a detector once over the stitched raster is a different
    experiment - both SIFT and SURF build their octaves from the image they are
    handed, and the border/octave behaviour of one 9216px canvas does not match
    the tiles it is made of. The packed keypoint count has to come from the
    per-tile path or it is not the number a rebuilt pack would ship.
    """
    detector = make_detector(name)
    all_keypoints = []
    descriptor_blocks = []
    for key in sorted(parsed):
        tile_keypoints, tile_descriptors = detector.detectAndCompute(parsed[key], None)
        if tile_descriptors is None or len(tile_keypoints) == 0:
            continue
        left, top = origins[key]
        for point in tile_keypoints:
            all_keypoints.append(cv2.KeyPoint(
                point.pt[0] + left, point.pt[1] + top, point.size, point.angle,
                point.response, point.octave, point.class_id))
        descriptor_blocks.append(tile_descriptors)
    if not all_keypoints:
        return [], None
    descriptors = np.vstack(descriptor_blocks)
    if with_grid:
        all_keypoints, descriptors = grid_sample(all_keypoints, descriptors, canvas_shape,
                                                 cells=grid, max_per_cell=max_per_cell)
    return all_keypoints, descriptors


CHUNK_ROWS = 8000      # cv::BFMatcher rejects a train set above IMGIDX_ONE rows


def ratio_test_chunked(query_descriptors, reference_descriptors, ratio):
    """knnMatch(k=2) over the whole reference set, in row chunks.

    OpenCV's BFMatcher caps the train descriptor count, and the biggest packs
    (lahai: 366,000 rows) exceed it, so the nearest neighbours are collected
    per chunk and merged by distance.
    """
    best = np.full(len(query_descriptors), np.inf, dtype=np.float32)
    second = np.full(len(query_descriptors), np.inf, dtype=np.float32)
    best_index = np.full(len(query_descriptors), -1, dtype=np.int64)
    matcher = cv2.BFMatcher(cv2.NORM_L2)
    for start in range(0, len(reference_descriptors), CHUNK_ROWS):
        chunk = reference_descriptors[start:start + CHUNK_ROWS]
        for query_index, pair in enumerate(matcher.knnMatch(query_descriptors, chunk, 2)):
            if len(pair) < 2:
                continue
            first, alternative = pair
            if first.distance < best[query_index]:
                second[query_index] = min(best[query_index], alternative.distance)
                best[query_index] = first.distance
                best_index[query_index] = start + first.trainIdx
            elif first.distance < second[query_index]:
                second[query_index] = first.distance
    keep = np.where(best < ratio * np.maximum(second, 1e-9))[0]
    return keep, best_index[keep]


def localize(query, reference_keypoints, reference_descriptors, detector,
             ratio=0.75, tolerance=4.0):
    """Localize one crop and score it translation-invariantly.

    A pack's keypoints do not have to share the raster's origin (IMao's packs
    live in the tile bundle's virtual canvas), so an absolute homography is not
    a fair test. What is fair is the consensus shift: a correct localization
    puts nearly every ratio-test match on the same (dx, dy), whatever the
    origin is, while a wrong one scatters them.
    """
    query_keypoints, query_descriptors = detector.detectAndCompute(query, None)
    result = {"queryKeypoints": 0 if query_keypoints is None else len(query_keypoints),
              "good": 0, "inliers": 0, "error": None, "shift": None}
    if query_descriptors is None or len(query_descriptors) < 2:
        return result
    keep, targets = ratio_test_chunked(np.ascontiguousarray(query_descriptors),
                                       reference_descriptors, ratio)
    deltas = [(reference_keypoints[targets[i]].pt[0] - query_keypoints[index].pt[0],
               reference_keypoints[targets[i]].pt[1] - query_keypoints[index].pt[1])
              for i, index in enumerate(keep)]
    result["good"] = len(deltas)
    if len(deltas) < 4:
        return result
    array = np.asarray(deltas, dtype=np.float64)
    shift = np.median(array, axis=0)
    residuals = np.hypot(array[:, 0] - shift[0], array[:, 1] - shift[1])
    result["inliers"] = int(np.count_nonzero(residuals <= tolerance))
    result["shift"] = [float(shift[0]), float(shift[1])]
    result["residualMedian"] = float(np.median(residuals))
    return result


def imf_bytes_for(keypoints, columns):
    """Logical size in IMAO's IMAOFT01 layout (28-byte keypoint, float32 descriptors)."""
    return len(keypoints) * (columns * 4 + 28)


def npz_bytes_for(keypoints, descriptors, tmp_dir="measurements"):
    """Real on-disk size of a wuwa-map style np.savez_compressed archive."""
    import os
    import tempfile
    keypoint_array = np.array(
        [[kp.pt[0], kp.pt[1], kp.size, kp.angle, kp.response,
          float(kp.octave), float(kp.class_id)] for kp in keypoints],
        dtype=np.float32)
    handle, path = tempfile.mkstemp(suffix=".npz", dir=tmp_dir)
    os.close(handle)
    try:
        np.savez_compressed(path, keypoints=keypoint_array,
                            descriptors=np.asarray(descriptors, dtype=np.float32),
                            map_size=np.array([0, 0], dtype=np.int32),
                            num_keypoints=np.array(len(keypoints), dtype=np.int32))
        return os.path.getsize(path)
    finally:
        os.remove(path)


def log_size(name, raster, keypoints, descriptors):
    """Report keypoint count and every storage cost for one configuration."""
    import zlib
    columns = int(descriptors.shape[1])
    imf_bytes = imf_bytes_for(keypoints, columns)
    npz_bytes = npz_bytes_for(keypoints, descriptors)
    restored, quantized_columns, _ = quantize_descriptors(descriptors)
    stored = np.clip(np.round(
        np.asarray(descriptors, dtype=np.float32)
        * ((256 - 1) / max(float(np.asarray(descriptors).max()), 1e-9))),
        0, 255).astype(np.uint8)
    keypoint_block = np.asarray(
        [[kp.pt[0], kp.pt[1], kp.size, kp.angle, kp.response,
          float(kp.octave), float(kp.class_id)] for kp in keypoints],
        dtype=np.float32).tobytes()
    quantized_raw = len(keypoints) * (quantized_columns + 28)
    quantized_zlib = len(zlib.compress(stored.tobytes(), 6)) + len(zlib.compress(keypoint_block, 6)) \
        + len(keypoints) * 0  # both blocks compressed separately, as a codec would
    print("   %-22s kp=%-7d raw-imf=%7.2f MB  npz=%6.2f MB  uint8=%6.2f MB  "
          "uint8+zlib=%6.2f MB  B/kp: %.0f raw, %.0f npz, %.0f u8+zlib" % (
              name, len(keypoints), imf_bytes / 1048576.0, npz_bytes / 1048576.0,
              quantized_raw / 1048576.0, quantized_zlib / 1048576.0,
              imf_bytes / max(len(keypoints), 1), npz_bytes / max(len(keypoints), 1),
              quantized_zlib / max(len(keypoints), 1)))
    return {"keypoints": int(len(keypoints)), "descriptorColumns": columns,
            "imfBytes": int(imf_bytes), "npzBytes": int(npz_bytes),
            "quantizedImfBytes": int(quantized_raw),
            "quantizedZlibBytes": int(quantized_zlib)}


def quantize_descriptors(descriptors, levels=256):
    """Scalar-quantize descriptors to uint8, as a compressed .imf would store them.

    The realistic format change does not need a new detector: it stores the same
    keypoints with 128 uint8 values instead of 128 float32 values. Matching after
    dequantization then says whether that format keeps enough precision.
    """
    array = np.asarray(descriptors, dtype=np.float32)
    top = float(array.max())
    if top <= 0:
        return array.copy(), 128, 0
    scale = (levels - 1) / top
    stored = np.clip(np.round(array * scale), 0, levels - 1).astype(np.uint8)
    restored = stored.astype(np.float32) / scale
    return restored, int(stored.shape[1]), int(stored.nbytes)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pack-root", default="Assets/FeaturesDatas/KuroTilePacks")
    parser.add_argument("--pack", required=True)
    parser.add_argument("--base-map-id", default=None)
    parser.add_argument("--config", action="append", default=None,
                        help="sift-friend | sift-default | sift-dense; -grid suffix adds the cell cap")
    parser.add_argument("--queries", type=int, default=12)
    parser.add_argument("--crop", type=int, default=184)
    parser.add_argument("--scale", type=float, action="append", default=None,
                        help="query scale(s); default 1.0 (native pixels)")
    parser.add_argument("--grid", action="append", default=[],
                        help="budget sweep entry GRID:MAX_PER_CELL applied to each config")
    parser.add_argument("--reference-mode", choices=("per-tile", "whole-raster"),
                        default="per-tile",
                        help="where the reference features are detected. 'per-tile' is "
                             "production (KuroMapFeatureBuilder); 'whole-raster' is the older "
                             "protocol and is not the keypoint count a rebuilt pack would ship")
    parser.add_argument("--gate", choices=("art", "texture", "answerable"), default="art",
                        help="which candidate crops count as fair queries. 'art' is the "
                             "2026-10-08 protocol (not black, std >= 18) and is the default so "
                             "the original run stays reproducible; 'texture' demands structure; "
                             "'answerable' demands that the reference set itself carries enough "
                             "keypoints for the pass criterion to be reachable at all")
    parser.add_argument("--gate-reference", default="sift-friend",
                        help="config whose reference features define the 'answerable' gate")
    parser.add_argument("--quantize", action="store_true",
                        help="also score uint8-quantized descriptors (the cheap format change)")
    parser.add_argument("--json", default=None)
    arguments = parser.parse_args()

    pack_dir = Path(arguments.pack_root) / arguments.pack
    manifest = json.loads((pack_dir / "manifest.json").read_text(encoding="utf-8"))
    base_map_id = arguments.base_map_id
    if base_map_id is None:
        # A layered composite (layered_L14_F-1-14_902_3_0.png) is an extra
        # appearance stacked onto the surface frame, not the frame itself, and a
        # pack like lowervault lists far more of them than surface tiles. Taking
        # the most common prefix outright therefore selects "layered" and then
        # finds no readable image at all. The surface frame is the numeric
        # <mapId>_<x>_<y> family.
        names = [Path(t["file"]).name.split("_")[0]
                 for t in manifest["tiles"] if not t.get("absent", False)]
        surface = [name for name in names if name.isdigit()]
        base_map_id = max(set(surface), key=surface.count) if surface else None

    tiles, _ = load_tiles(pack_dir, base_map_id)
    raster, layout, origins = stitch(tiles)
    if raster is None:
        print("%s: no tiles for map id %s" % (arguments.pack, base_map_id))
        return 1
    print("== %s (map id %s): %d tiles -> %dx%d raster" % (
        arguments.pack, base_map_id, layout["tiles"], layout["size"][0], layout["size"][1]))

    imf_path = pack_dir / "features.imf"
    imao_keypoints, imao_descriptors, imao_info = load_imao_features(imf_path)
    imao_bytes = imf_path.stat().st_size
    print("   IMAO shipped SURF : kp=%d cols=%d disk=%.2f MB  B/kp=%.1f" % (
        len(imao_keypoints), imao_info["columns"], imao_bytes / 1048576.0,
        imao_bytes / max(len(imao_keypoints), 1)))

    # One reference build per config: the 'answerable' gate and the measurement
    # want the same features, and for a pack like jinzhou that detection pass is
    # the expensive part of the run.
    reference_cache = {}

    def reference_for(name, with_grid=None):
        """Reference features for one config, detected once and reused.

        `with_grid` must stay in the cache key: the callers reach the capped
        variant two different ways - a "-grid" config name, and measure() being
        handed the already-stripped base name plus with_grid=True. Keying on the
        name alone silently drops the cap for the second one.
        """
        if with_grid is None:
            with_grid = name.endswith("-grid")
            if with_grid:
                name = name[:-5]
        key = (arguments.reference_mode, name, bool(with_grid))
        if key not in reference_cache:
            if arguments.reference_mode == "whole-raster":
                reference_cache[key] = extract(name, raster, with_grid=with_grid)
            else:
                reference_cache[key] = extract_per_tile(name, tiles, origins,
                                                        raster.shape[:2], with_grid=with_grid)
        return reference_cache[key]

    # Synthetic queries: crops of the same raster at known positions, optionally
    # rescaled to imitate a minimap that is much smaller than the big map.
    scales = arguments.scale or [1.0]
    rng = np.random.default_rng(20261002)
    queries = []
    margin = arguments.crop
    attempts = 0
    gate_points = None
    if arguments.gate == "answerable":
        gate_points = np.array([kp.pt for kp in reference_for(arguments.gate_reference)[0]],
                               dtype=np.float64)
        print("   gate reference %s: %d keypoints" % (
            arguments.gate_reference, len(gate_points)))
    while len(queries) < arguments.queries * len(scales) and attempts < arguments.queries * 200:
        attempts += 1
        if raster.shape[0] < 3 * margin or raster.shape[1] < 3 * margin:
            break
        top = int(rng.integers(margin, raster.shape[0] - margin))
        left = int(rng.integers(margin, raster.shape[1] - margin))
        if not gate_accepts(arguments.gate, raster, top, left, arguments.crop,
                            points=gate_points):
            continue
        crop = raster[top:top + arguments.crop, left:left + arguments.crop]
        centre = (left + arguments.crop / 2.0, top + arguments.crop / 2.0)
        for scale in scales:
            if scale == 1.0:
                queries.append((crop, centre, scale))
            else:
                resized = cv2.resize(crop, None, fx=scale, fy=scale,
                                     interpolation=cv2.INTER_AREA)
                queries.append((resized, centre, scale))
    print("   queries: %d (crop %d px at scales %s, from %d candidate positions, gate=%s)" % (
        len(queries), arguments.crop, scales, attempts, arguments.gate))

    # What each crop landed on. This is what separates "the method failed" from
    # "the question had no answer", and that distinction is the whole point of
    # the 2026-10-10 correction, so it belongs in the report rather than in a
    # side script.
    diagnostic_points = np.array([kp.pt for kp in reference_for(arguments.gate_reference)[0]],
                                 dtype=np.float64)
    query_diagnostics = []
    for crop, centre, scale in queries:
        left = int(round(centre[0] - arguments.crop / 2.0))
        top = int(round(centre[1] - arguments.crop / 2.0))
        box = raster[top:top + arguments.crop, left:left + arguments.crop]
        inside = ((diagnostic_points[:, 0] >= left) &
                  (diagnostic_points[:, 0] < left + arguments.crop) &
                  (diagnostic_points[:, 1] >= top) &
                  (diagnostic_points[:, 1] < top + arguments.crop))
        query_diagnostics.append({
            "left": left, "top": top, "scale": scale,
            "ink": round(int(np.count_nonzero(box > 8)) / float(box.size), 4),
            "std": round(float(box.std()), 2),
            "meanLaplacian": round(crop_mean_laplacian(raster, top, left, arguments.crop), 3),
            "referenceKeypoints": int(np.count_nonzero(inside)),
        })

    report = {"pack": arguments.pack, "baseMapId": base_map_id, "layout": layout,
              "scales": scales, "crop": arguments.crop,
              "referenceMode": arguments.reference_mode,
              "gate": arguments.gate,
              "queries": query_diagnostics,
              "imao": {"keypoints": len(imao_keypoints), "diskBytes": imao_bytes},
              "configs": {}}

    def measure(name, with_grid=False):
        """Build the reference feature set the way the shipping pack would."""
        return reference_for(name, with_grid=with_grid)

    def evaluate(label, reference_keypoints, reference_descriptors, detector):
        trials = []
        for crop, centre, scale in queries:
            outcome = localize(crop, reference_keypoints, reference_descriptors, detector)
            shift = outcome.get("shift")
            outcome["scale"] = scale
            if shift is not None:
                dx = shift[0] + crop.shape[1] / 2.0 - centre[0]
                dy = shift[1] + crop.shape[0] / 2.0 - centre[1]
                outcome["error"] = float(np.hypot(dx, dy))
            trials.append(outcome)
        verified = [t for t in trials
                    if t.get("inliers", 0) >= 8 and t.get("error") is not None and t["error"] <= 4.0]
        inliers = [t.get("inliers", 0) for t in trials]
        errors = [t["error"] for t in trials if t.get("error") is not None]
        good = [t["good"] for t in trials]
        per_scale = {}
        for scale in scales:
            subset = [t for t in trials if t["scale"] == scale]
            per_scale[str(scale)] = {
                "verified": len([t for t in subset
                                 if t.get("inliers", 0) >= 8 and t.get("error") is not None
                                 and t["error"] <= 4.0]),
                "queries": len(subset),
                "inliers": [t.get("inliers", 0) for t in subset],
            }
        print("   %-18s verified=%2d/%-2d  inliers median=%3d  good median=%3d  "
              "shift err median=%s   per-scale %s" % (
                  label, len(verified), len(trials),
                  int(np.median(inliers)) if inliers else 0,
                  int(np.median(good)) if good else 0,
                  ("%.2f px" % np.median(errors)) if errors else "n/a",
                  " ".join("%s:%d/%d" % (key, value["verified"], value["queries"])
                           for key, value in per_scale.items())))
        # Merge into the entry the caller already stored instead of replacing it.
        # log_size() runs first and records keypoints and every byte count;
        # overwriting the entry here is how earlier reports ended up carrying
        # accuracy with no size next to it.
        entry = report["configs"].setdefault(label, {})
        entry.update({
            "verified": len(verified), "queries": len(trials),
            "inliers": inliers, "errors": errors, "good": good,
            "perScale": per_scale,
        })
        return len(verified)

    # IMAO's own column: the keypoint count and the size are facts read off the
    # shipped pack. Its accuracy cannot be measured here - the shipped
    # descriptors are SURF's L2-normalized floats, and a SIFT query descriptor
    # cannot be compared against them (every cross-space L2 distance saturates).
    imao_report = report["imao"]
    imao_report["columns"] = imao_info["columns"]
    imao_report["imfBytes"] = int(imao_bytes)
    cap_keypoints, cap_descriptors = grid_sample(
        imao_keypoints, imao_descriptors, raster.shape[:2], cells=50, max_per_cell=20,
        bounds=keypoint_bounds(imao_keypoints))
    imao_report["cap50x20"] = {
        "keypoints": int(len(cap_keypoints)),
        "keptFraction": round(len(cap_keypoints) / max(len(imao_keypoints), 1), 4),
    }
    print("   IMAO cap 50:20 would keep %d of %d keypoints (%.1f%%)" % (
        len(cap_keypoints), len(imao_keypoints),
        100.0 * len(cap_keypoints) / max(len(imao_keypoints), 1)))
    print("   (SURF accuracy is not measurable from the Python OpenCV wheels: "
          "cv2.xfeatures2d.SURF_create raises 'this algorithm is patented and is "
          "excluded in this configuration'. An earlier revision of this script "
          "recorded an 'imao-surf-13329' column of verified=0 because of that "
          "exception, not because SURF failed to localize. Do not read those "
          "zeros as a SURF measurement.)")

    def evaluate_quantized(label, keypoints, descriptors, base, budget_bytes):
        restored, columns, stored_bytes = quantize_descriptors(descriptors)
        print("   %-22s uint8 payload=%d B/descriptor -> imf-like %6.2f MB total" % (
            label, columns, (len(keypoints) * (columns + 28)) / 1048576.0))
        report["configs"][label + " quantized"] = {
            "keypoints": int(len(keypoints)), "bytesPerDescriptor": columns,
            "quantizedImfBytes": int(len(keypoints) * (columns + 28)),
        }
        evaluate(label + " quantized", keypoints, restored, make_detector(base))

    for name in (arguments.config or ["sift-friend", "sift-friend-grid",
                                      "sift-default", "sift-dense"]):
        with_grid = name.endswith("-grid")
        base = name[:-5] if with_grid else name
        started = time.time()
        keypoints, descriptors = measure(base, with_grid=with_grid)
        elapsed = time.time() - started
        if descriptors is None or len(keypoints) == 0:
            print("   %-18s no descriptors" % name)
            continue
        report["configs"][name] = log_size(name, raster, keypoints, descriptors)
        report["configs"][name]["detectSeconds"] = round(elapsed, 2)
        evaluate(name, keypoints, descriptors, make_detector(base))
        if arguments.quantize:
            evaluate_quantized(name, keypoints, descriptors, base, None)

    # How much the cell cap would actually bite, at the budgets worth testing.
    for option in arguments.grid:
        grid, max_per_cell = (int(part) for part in option.split(":"))
        base = "sift-friend"
        full_keypoints, full_descriptors = measure(base)
        capped_keypoints, capped_descriptors = grid_sample(
            full_keypoints, full_descriptors, raster.shape[:2],
            cells=grid, max_per_cell=max_per_cell)
        label = "cap %d:%d" % (grid, max_per_cell)
        report["configs"][label] = log_size(label, raster, capped_keypoints, capped_descriptors)
        report["configs"][label]["detected"] = int(len(full_keypoints))
        report["configs"][label]["keptFraction"] = round(
            len(capped_keypoints) / max(len(full_keypoints), 1), 4)
        evaluate(label, capped_keypoints, capped_descriptors, make_detector(base))

    if arguments.json:
        Path(arguments.json).write_text(json.dumps(report, indent=2), encoding="utf-8")
        print("wrote %s" % arguments.json)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
