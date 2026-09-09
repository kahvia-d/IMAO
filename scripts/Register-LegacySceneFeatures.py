"""Propose hash-bound legacy exclusions from replacement feature geometry.

Requires numpy/scipy. Writes a candidate report only: replay independent captures
before copying legacyBaseExclusions into a verified replacement manifest.
"""
import argparse
import hashlib
import json
import struct
from pathlib import Path

import numpy as np
from scipy.spatial import cKDTree
from scipy.spatial.distance import cdist


def read_features(path):
    data = path.read_bytes()
    if data[:8] != b"IMAOFT01":
        raise ValueError("Invalid feature binary")
    version, header, endian, count, rows, columns, kind = struct.unpack_from("<7I", data, 8)
    if (version, header, endian, rows, columns, kind) != (1, 116, 0x01020304, count, 128, 1):
        raise ValueError("Unsupported feature binary layout")
    if len(data) != 116 + count * (28 + 512) or hashlib.sha256(data[116:]).digest() != data[84:116]:
        raise ValueError("Feature binary size or payload hash mismatch")
    points = np.frombuffer(data, "<f4", count * 7, 116).reshape(count, 7)[:, :2]
    descriptors = np.frombuffer(data, "<f4", count * 128, 116 + count * 28).reshape(count, 128)
    return points, descriptors, hashlib.sha256(data).hexdigest(), data[52:84].hex()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("baseline", type=Path)
    parser.add_argument("pack", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--spatial-radius", type=float, default=None,
                        help="Limit descriptor candidates in the existing atlas frame; final residual gates are unchanged")
    args = parser.parse_args()
    bx, bd, baseline_hash, _ = read_features(args.baseline)
    px, pd, _, source_hash = read_features(args.pack / "features.imf")
    manifest = json.loads((args.pack / "manifest.json").read_text(encoding="utf8"))
    if source_hash != manifest["features"]["sha256"]:
        raise ValueError("Replacement feature provenance mismatch")
    # This is an atlas registration search window, not a runtime scene rule.
    ids = np.flatnonzero(((bx >= px.min(0) - 500) & (bx <= px.max(0) + 500)).all(1))
    matches = []
    if args.spatial_radius is not None:
        if not 0 < args.spatial_radius <= 500:
            raise ValueError("Spatial radius must be in (0, 500] atlas pixels")
        tree = cKDTree(px)
        for offset, row in enumerate(ids):
            candidates = tree.query_ball_point(bx[row], args.spatial_radius)
            if len(candidates) < 2:
                continue
            distances = cdist(bd[row:row + 1], pd[candidates], "sqeuclidean")[0]
            nearest = np.argpartition(distances, 1)[:2]
            nearest = nearest[np.argsort(distances[nearest])]
            if distances[nearest[0]] < .62 ** 2 * distances[nearest[1]] and distances[nearest[0]] < .5 ** 2:
                matches.append((int(row), candidates[nearest[0]]))
            if offset % 5000 == 0:
                print(f"Registered {offset}/{len(ids)} atlas rows", flush=True)
    for start in range(0, len(ids), 256):
        if args.spatial_radius is not None:
            break
        selected = ids[start:start + 256]
        distances = cdist(bd[selected], pd, "sqeuclidean")
        nearest = np.argpartition(distances, 1, axis=1)[:, :2]
        values = np.take_along_axis(distances, nearest, axis=1)
        order = np.argsort(values, axis=1)
        nearest = np.take_along_axis(nearest, order, axis=1)
        values = np.take_along_axis(values, order, axis=1)
        good = (values[:, 0] < .62 ** 2 * values[:, 1]) & (values[:, 0] < .5 ** 2)
        matches.extend(zip(selected[good].tolist(), nearest[good, 0].tolist()))
    if len(matches) < 12:
        raise ValueError("Insufficient descriptor registration support")
    matches = np.asarray(matches)
    delta = px[matches[:, 1]] - bx[matches[:, 0]]
    translation = np.median(delta, axis=0)
    inliers = np.linalg.norm(delta - translation, axis=1) < 2
    support = set(matches[inliers, 0])
    parent = np.arange(len(ids))

    def root(index):
        while parent[index] != index:
            parent[index] = parent[parent[index]]
            index = parent[index]
        return index

    for first, second in cKDTree(bx[ids]).query_pairs(100, output_type="ndarray"):
        parent[root(second)] = root(first)
    labels = np.array([root(index) for index in range(len(ids))])
    excluded, components = [], []
    for label in np.unique(labels):
        rows = ids[labels == label]
        hits = sum(row in support for row in rows)
        if hits < 12 or hits / len(rows) < .35:
            continue
        excluded.extend(rows.tolist())
        components.append({"rows": len(rows), "matched": hits,
                           "min": bx[rows].min(0).tolist(), "max": bx[rows].max(0).tolist()})
    if not excluded:
        raise ValueError("No supported atlas component")
    report = {"diagnosticOnly": True, "sceneId": manifest["sceneId"], "spatialRadius": args.spatial_radius,
              "translation": translation.tolist(), "components": components,
              "legacyBaseExclusions": {"baseFeatureSha256": baseline_hash,
                  "replacementFeatureSha256": source_hash,
                  "method": "descriptor registration; 100px components; >=12 matches; >=35% support; <2px residual",
                  "matchedFeatures": int(inliers.sum()), "rows": sorted(excluded)}}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf8")
    print(f"Candidate only: {len(excluded)} rows, {inliers.sum()} registration inliers")


if __name__ == "__main__":
    main()
