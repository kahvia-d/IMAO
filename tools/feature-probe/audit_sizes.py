"""Size audit for map feature files.

Two formats are read without third-party dependencies:

  * the wuwa-map convention: ``np.savez_compressed`` archives holding
    ``keypoints`` (N x 7 float32) and ``descriptors`` (N x D float32),
  * IMAO's ``IMAOFT01`` binaries, whose 116-byte header is parsed directly.

For every file it reports the stored keypoint count, the descriptor width,
the on-disk bytes, the logical payload bytes and the resulting bytes per
keypoint, so a size claim can be checked instead of estimated.
"""

import argparse
import hashlib
import json
import struct
import sys
import zipfile
from pathlib import Path

IMPORTANT_NPZ_KEYS = ("keypoints.npy", "descriptors.npy", "map_size.npy")


def read_npy_header(raw):
    """Return (dtype, shape) for a .npy member."""
    if len(raw) < 10 or raw[:6] != b"\x93NUMPY":
        raise ValueError("not a .npy payload")
    major = raw[6]
    header_len = struct.unpack("<H", raw[8:10])[0] if major == 1 else struct.unpack("<I", raw[8:12])[0]
    start = 10 if major == 1 else 12
    header = raw[start:start + header_len].decode("latin1")
    info = ast_literal_dict(header)
    return info["descr"], tuple(info["shape"])


def ast_literal_dict(text):
    """Parse the {'descr': ..., 'shape': ...} literal without eval()."""
    result = {}
    for key in ("descr", "shape", "fortran_order"):
        marker = "'%s'" % key
        at = text.find(marker)
        if at < 0:
            continue
        colon = text.find(":", at)
        if key == "descr":
            first = text.find("'", colon + 1)
            second = text.find("'", first + 1)
            result[key] = text[first + 1:second]
        elif key == "shape":
            open_paren = text.find("(", colon)
            close_paren = text.find(")", open_paren)
            body = text[open_paren + 1:close_paren].strip().rstrip(",")
            result[key] = tuple(int(part) for part in body.split(",") if part.strip())
        else:
            result[key] = "True" in text[colon:colon + 20]
    return result


def size_of_dtype(descr):
    table = {"f4": 4, "f8": 8, "i4": 4, "i8": 8, "u1": 1, "i2": 2, "u2": 2}
    return table.get(descr, 4)


def audit_npz(path):
    record = {"path": str(path), "format": "npz", "diskBytes": path.stat().st_size}
    with zipfile.ZipFile(path) as archive:
        names = archive.namelist()
        record["members"] = names
        payload = 0
        for name in names:
            info = archive.getinfo(name)
            raw = archive.read(name) if name in IMPORTANT_NPZ_KEYS else b""
            if raw:
                try:
                    dtype, shape = read_npy_header(raw)
                except ValueError:
                    continue
                count = 1
                for dim in shape:
                    count *= dim
                logical = count * size_of_dtype(dtype)
                payload += logical
                if name.startswith("keypoints"):
                    record["keypoints"] = shape[0] if shape else 0
                if name.startswith("descriptors"):
                    record["descriptorColumns"] = shape[1] if len(shape) > 1 else 0
                    record["descriptorBytes"] = logical
                    record["descriptorDtype"] = dtype
            else:
                # Fall back to the zip directory entry for members we skip.
                payload += info.file_size
    record["logicalBytes"] = payload
    record["storedBytes"] = sum(archive.getinfo(n).compress_size for n in names) \
        if (archive := zipfile.ZipFile(path)) else record["diskBytes"]
    if record.get("keypoints"):
        record["bytesPerKeypoint"] = record["diskBytes"] / record["keypoints"]
    return record


def audit_imf(path):
    with open(path, "rb") as handle:
        header = handle.read(116)
    if header[:8] != b"IMAOFT01":
        raise ValueError("not an IMAOFT01 binary")
    fields = struct.unpack_from("<8I2Q", header, 8)
    (version, header_length, endian, keypoints, rows, columns,
     descriptor_type, _reserved, keypoint_bytes, descriptor_bytes) = fields
    disk = path.stat().st_size
    return {
        "path": str(path),
        "format": "IMAOFT01",
        "version": version,
        "keypoints": keypoints,
        "descriptorRows": rows,
        "descriptorColumns": columns,
        "descriptorType": descriptor_type,
        "keypointPayloadBytes": keypoint_bytes,
        "descriptorPayloadBytes": descriptor_bytes,
        "diskBytes": disk,
        "logicalBytes": header_length + keypoint_bytes + descriptor_bytes,
        "bytesPerKeypoint": disk / keypoints if keypoints else 0.0,
    }


def collect(root, kind):
    paths = sorted(Path(root).rglob("*.npz" if kind == "npz" else "*.imf"))
    records = []
    for path in paths:
        try:
            records.append(audit_npz(path) if kind == "npz" else audit_imf(path))
        except Exception as error:  # keep going: one bad file must not hide the rest
            records.append({"path": str(path), "error": str(error)})
    return records


def summarize(records):
    ok = [r for r in records if "error" not in r]
    return {
        "files": len(records),
        "failed": len(records) - len(ok),
        "keypoints": sum(r.get("keypoints", 0) for r in ok),
        "diskBytes": sum(r.get("diskBytes", 0) for r in ok),
        "logicalBytes": sum(r.get("logicalBytes", 0) for r in ok),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--npz-dir", action="append", default=[],
                        help="directory tree containing wuwa-map .npz feature archives")
    parser.add_argument("--imf-dir", action="append", default=[],
                        help="directory tree containing IMAO .imf feature binaries")
    parser.add_argument("--json", default=None, help="write the full audit to this file")
    arguments = parser.parse_args()

    report = {"npz": {}, "imf": {}}
    for directory in arguments.npz_dir:
        records = collect(directory, "npz")
        report["npz"][directory] = {"records": records, "summary": summarize(records)}
        print("== npz: %s" % directory)
        for record in records:
            if "error" in record:
                print("   ERROR %s: %s" % (record["path"], record["error"]))
                continue
            print("   %-46s kp=%-8d cols=%-4s disk=%9d  logical=%10d  B/kp=%6.2f" % (
                Path(record["path"]).name, record["keypoints"],
                record.get("descriptorColumns", "?"), record["diskBytes"],
                record["logicalBytes"], record.get("bytesPerKeypoint", 0.0)))
        summary = report["npz"][directory]["summary"]
        print("   TOTAL files=%d keypoints=%d disk=%.2f MB logical=%.2f MB" % (
            summary["files"], summary["keypoints"],
            summary["diskBytes"] / 1048576.0, summary["logicalBytes"] / 1048576.0))

    for directory in arguments.imf_dir:
        records = collect(directory, "imf")
        report["imf"][directory] = {"records": records, "summary": summarize(records)}
        print("== imf: %s" % directory)
        for record in records:
            if "error" in record:
                print("   ERROR %s: %s" % (record["path"], record["error"]))
                continue
            print("   %-24s kp=%-8d cols=%-4d disk=%9d  B/kp=%6.2f" % (
                Path(record["path"]).parent.name, record["keypoints"],
                record["descriptorColumns"], record["diskBytes"],
                record["bytesPerKeypoint"]))
        summary = report["imf"][directory]["summary"]
        print("   TOTAL files=%d keypoints=%d disk=%.2f MB" % (
            summary["files"], summary["keypoints"], summary["diskBytes"] / 1048576.0))

    if arguments.json:
        Path(arguments.json).write_text(json.dumps(report, indent=2), encoding="utf-8")
        print("wrote %s" % arguments.json)
    return 0


if __name__ == "__main__":
    sys.exit(main())
