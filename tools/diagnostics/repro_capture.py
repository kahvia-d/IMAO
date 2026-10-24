"""Inspect immutable short-repro evidence; export native CoordinateYolo replay inputs.

Usage: python tools/diagnostics/repro_capture.py SESSION --output OUTPUT
       [--reconstruct-client] [--model MODEL_DIRECTORY]
Requires local OpenCV (cv2) and numpy; never downloads models or runs inference.
Exit 0: complete, structurally valid evidence; 1: invalid/incomplete evidence;
2: invocation/output failure. Boundary gaps are reported separately from corruption.
"""

import argparse
import ctypes
import hashlib
import html
import json
import os
from pathlib import Path, PureWindowsPath
import struct
import sys
import zlib

import cv2
import numpy as np


SPARSE_NOTICE = ('ROI WGC images are sparse surfaces: only validRegions contain captured '
                 'pixels. Black pixels outside those regions are invalid, not a full game frame. '
                 'Reconstructed client images preserve that same sparse validity contract.')


def _inside(path, root):
    return path == root or root in path.parents


def _image_path(root, relative):
    if not isinstance(relative, str) or not relative or ':' in relative:
        raise ValueError('image path must be a relative ordinary file path')
    windows = PureWindowsPath(relative)
    parts = relative.replace('\\', '/').split('/')
    if windows.is_absolute() or windows.drive or relative.startswith(('/', '\\')) or '..' in parts:
        raise ValueError('absolute, traversal and drive paths are forbidden')
    path = root.joinpath(*parts).resolve()
    if not _inside(path, root) or path == root:
        raise ValueError('image path escapes the session (including symlinks)')
    return path


def _png(path, descriptor):
    """Check PNG structure/CRC and decoded 8-bit shape, without trusting the extension."""
    data = path.read_bytes()
    if data[:8] != b'\x89PNG\r\n\x1a\n':
        raise ValueError('not a PNG lossless image')
    pos, ihdr, ended = 8, None, False
    while pos < len(data):
        if pos + 12 > len(data):
            raise ValueError('truncated PNG chunk')
        length = struct.unpack_from('>I', data, pos)[0]
        end = pos + length + 12
        if end > len(data):
            raise ValueError('truncated PNG data')
        kind, body = data[pos + 4:pos + 8], data[pos + 8:pos + 8 + length]
        if zlib.crc32(kind + body) & 0xffffffff != struct.unpack_from('>I', data, end - 4)[0]:
            raise ValueError('PNG CRC mismatch')
        if pos == 8:
            if kind != b'IHDR' or length != 13:
                raise ValueError('missing PNG header')
            ihdr = struct.unpack('>IIBBBBB', body)
        pos = end
        if kind == b'IEND':
            ended = True
            break
    if not ended or pos != len(data) or ihdr[2] != 8 or ihdr[3] not in (0, 2, 6):
        raise ValueError('PNG is incomplete or not native 8-bit gray/BGR/BGRA')
    pixels = cv2.imdecode(np.frombuffer(data, dtype=np.uint8), cv2.IMREAD_UNCHANGED)
    if pixels is None:
        raise ValueError('PNG decode failed')
    height, width = pixels.shape[:2]
    channels = 1 if pixels.ndim == 2 else pixels.shape[2]
    expected = {'width': width, 'height': height, 'channels': channels, 'type': (channels - 1) * 8}
    if any(descriptor.get(k) != v for k, v in expected.items()) or ihdr[:2] != (width, height):
        raise ValueError(f'PNG shape/type differs from descriptor: decoded {expected}')
    return pixels


def _decode_image(path, descriptor):
    encoding = descriptor.get('encoding', 'png')
    if encoding == 'png':
        return _png(path, descriptor)
    if encoding != 'windows-xpress-chunks-v1':
        raise ValueError(f'unsupported image encoding: {encoding}')
    # Bound both the file and inflated surface before reading/allocating. A corrupt length
    # must never become a ctypes allocation or be passed to the Windows codec unchecked.
    limit = 512 * 1024 * 1024
    width, height, channels = (descriptor.get(k) for k in ('width', 'height', 'channels'))
    if any(type(v) is not int or v <= 0 for v in (width, height, channels)) or channels not in (1, 3, 4):
        raise ValueError('invalid XPRESS image dimensions/channels')
    row_bytes, raw_bytes = descriptor.get('rowBytes'), descriptor.get('rawBytes')
    if (type(row_bytes) is not int or type(raw_bytes) is not int or
            row_bytes != width * channels or raw_bytes != row_bytes * height or
            raw_bytes > limit or descriptor.get('type') != (channels - 1) * 8):
        raise ValueError('XPRESS descriptor must describe bounded contiguous 8-bit pixels (maximum 512 MiB)')
    if path.stat().st_size > limit:
        raise ValueError('XPRESS container exceeds the 512 MiB inspection limit')
    data = path.read_bytes()
    if len(data) < 12 or data[:8] != b'IMAOXPR1':
        raise ValueError('invalid/truncated XPRESS container header')
    count = struct.unpack_from('<I', data, 8)[0]
    if count < 1 or count > min(4, raw_bytes):
        raise ValueError('invalid XPRESS chunk count (must be 1..4 nonempty chunks)')
    pos, total, chunks = 12, 0, []
    for _ in range(count):
        if pos + 16 > len(data):
            raise ValueError('truncated XPRESS chunk header')
        raw_length, compressed_length = struct.unpack_from('<QQ', data, pos)
        pos += 16
        if (not raw_length or not compressed_length or raw_length > raw_bytes - total or
                compressed_length > len(data) - pos):
            raise ValueError('XPRESS chunk length is empty, oversized or truncated')
        chunks.append((raw_length, pos, compressed_length))
        pos += compressed_length
        total += raw_length
    if total != raw_bytes or pos != len(data):
        raise ValueError('XPRESS total length differs from descriptor or contains trailing bytes')
    if os.name != 'nt':
        raise ValueError('windows-xpress-chunks-v1 requires Windows CompressionAPI; this platform is unsupported')
    try:
        cabinet = ctypes.WinDLL('cabinet', use_last_error=True)
        cabinet.CreateDecompressor.argtypes = [ctypes.c_uint32, ctypes.c_void_p, ctypes.POINTER(ctypes.c_void_p)]
        cabinet.CreateDecompressor.restype = ctypes.c_int
        cabinet.Decompress.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t,
                                      ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
        cabinet.Decompress.restype = ctypes.c_int
        cabinet.CloseDecompressor.argtypes = [ctypes.c_void_p]
        cabinet.CloseDecompressor.restype = ctypes.c_int
    except (OSError, AttributeError) as exc:
        raise ValueError(f'Windows CompressionAPI unavailable: {exc}') from exc
    handle = ctypes.c_void_p()
    if not cabinet.CreateDecompressor(3, None, ctypes.byref(handle)):
        raise ValueError(f'CreateDecompressor(XPRESS) failed: Windows error {ctypes.get_last_error()}')
    try:
        pixels = np.empty(raw_bytes, dtype=np.uint8)
        offset = 0
        for raw_length, start, compressed_length in chunks:
            compressed = ctypes.create_string_buffer(data[start:start + compressed_length])
            written = ctypes.c_size_t()
            if not cabinet.Decompress(handle, compressed, compressed_length,
                                      ctypes.c_void_p(pixels.ctypes.data + offset), raw_length, ctypes.byref(written)):
                raise ValueError(f'XPRESS decompression failed: Windows error {ctypes.get_last_error()}')
            if written.value != raw_length:
                raise ValueError('XPRESS decoded chunk size differs from declared raw length')
            offset += raw_length
        return pixels.reshape((height, width) if channels == 1 else (height, width, channels))
    finally:
        cabinet.CloseDecompressor(handle)


def _key(record, fields):
    context = record.get('context', {})
    values = tuple(context.get(field) for field in fields)
    if any(value is None or isinstance(value, (list, dict)) for value in values):
        return None
    return values


def _rectangle(rectangle, width, height):
    if not isinstance(rectangle, list) or len(rectangle) != 4 or any(type(v) is not int for v in rectangle):
        raise ValueError('rectangle must be four integers')
    x, y, w, h = rectangle
    if x < 0 or y < 0 or w <= 0 or h <= 0 or x + w > width or y + h > height:
        raise ValueError('rectangle is outside the recorded source surface')
    return x, y, w, h


def inspect_session(session, output, reconstruct=False, model=None):
    root, output = Path(session).resolve(), Path(output).resolve()
    if _inside(output, root):
        raise ValueError('output must be outside the original evidence session')
    report = {'schemaVersion': 1, 'session': str(root), 'ok': False, 'errors': [], 'gaps': [],
              'sparseNotice': SPARSE_NOTICE, 'sparseFrames': [], 'reconstructed': [],
              'decodedPreviews': [], 'replayCount': 0}

    def issue(code, detail, record=None, gap=False):
        item = {'code': code, 'detail': str(detail)}
        if record is not None:
            item['recordId'] = record.get('recordId')
            item['context'] = record.get('context', {})
        report['gaps' if gap else 'errors'].append(item)

    def load(name):
        try:
            value = json.loads((root / name).read_text(encoding='utf-8'))
            if not isinstance(value, dict):
                raise ValueError('expected JSON object')
            return value
        except (OSError, ValueError) as exc:
            issue('manifest-invalid', f'{name}: {exc}')
            return {}

    metadata, completion = load('metadata.json'), load('completion.json')
    report['metadata'], report['completion'] = metadata, completion
    if metadata.get('schemaVersion') != 1:
        issue('schema-version', 'only schemaVersion 1 is supported')
    if not isinstance(metadata.get('metadata'), dict):
        issue('metadata-invalid', 'metadata must contain a metadata object')
    if completion.get('complete') is not True:
        issue('session-incomplete', 'completion.json is absent, invalid, or complete is not true')
    for counter in ('missingImages', 'missingRecords'):
        if completion.get(counter, 0) != 0:
            issue('recorder-loss', f'{counter}={completion[counter]}')

    records, ids = [], set()
    try:
        with (root / 'records.jsonl').open(encoding='utf-8') as stream:
            for number, line in enumerate(stream, 1):
                try:
                    row = json.loads(line)
                    if not isinstance(row, dict) or not isinstance(row.get('metadata'), dict) or not isinstance(row.get('context'), dict):
                        raise ValueError('record, context and metadata must be objects')
                    rid = row.get('recordId')
                    if type(rid) is not int or rid < 1 or rid in ids:
                        issue('record-id', f'line {number}: invalid or duplicate recordId', row)
                        continue
                    ids.add(rid)
                    records.append(row)
                except ValueError as exc:
                    issue('record-invalid', f'line {number}: {exc}')
    except (OSError, UnicodeError) as exc:
        issue('records-unreadable', exc)
    report['recordCount'] = len(records)
    if any(rid != expected for expected, rid in enumerate(sorted(ids), 1)):
        issue('record-id-gap', 'record IDs have gaps; evidence may be missing')
    for counter, actual in [('savedRecords', len(records)), ('savedImages', sum(r.get('kind') == 'image' for r in records))]:
        if counter in completion and completion[counter] != actual:
            issue('counter-mismatch', f'{counter}: manifest {completion[counter]}, records {actual}')

    images, sources, published, requests = {}, {}, {}, {}
    for row in records:
        rid, stage = row['recordId'], row.get('stage')
        if row.get('kind') == 'image':
            descriptor = row.get('image', {})
            try:
                path = _image_path(root, descriptor.get('path'))
            except (ValueError, AttributeError) as exc:
                issue('unsafe-image-path', exc, row)
            else:
                try:
                    if descriptor.get('encoding') == 'windows-xpress-chunks-v1' and stage != 'wgc':
                        raise ValueError('XPRESS encoding is supported only for WGC source frames; OCR replay requires PNG')
                    pixels = _decode_image(path, descriptor)
                    images[rid] = path
                    del pixels
                except (OSError, ValueError, cv2.error) as exc:
                    issue('image-invalid', exc, row)
        if stage == 'wgc':
            source_key = _key(row, ('captureRun', 'sourceSequence'))
            if source_key is not None:
                if source_key in sources:
                    issue('source-ambiguous', 'multiple WGC images for one source sequence', row)
                    sources[source_key] = None
                else:
                    sources[source_key] = row
            m = row['metadata']
            try:
                descriptor = row.get('image', {})
                width, height = m.get('width'), m.get('height')
                if type(width) is not int or type(height) is not int or width <= 0 or height <= 0:
                    raise ValueError('readback dimensions must be positive integers')
                if not isinstance(descriptor, dict) or width != descriptor.get('width') or height != descriptor.get('height'):
                    raise ValueError('readback dimensions differ from image descriptor')
                if type(m.get('partial')) is not bool or not isinstance(m.get('validRegions'), list) or not m['validRegions']:
                    raise ValueError('partial and nonempty validRegions are required')
                for region in m['validRegions']:
                    _rectangle(region, width, height)
                if not m['partial'] and m['validRegions'] != [[0, 0, width, height]]:
                    raise ValueError('full readback must describe the complete surface')
            except ValueError as exc:
                issue('readback-invalid', exc, row)
            report['sparseFrames'].append({'recordId': rid, 'context': row['context'],
                                          'partial': m.get('partial'), 'validRegions': m.get('validRegions'),
                                          'width': m.get('width'), 'height': m.get('height')})
        if stage == 'capture-published':
            published[_key(row, ('captureRun', 'frameId'))] = row
        if stage in ('ocr-crop', 'ocr-raw', 'ocr-contrast', 'ocr-detections'):
            request_key = _key(row, ('captureRun', 'frameId', 'requestId'))
            if request_key is None:
                issue('ocr-context-missing', 'captureRun/frameId/requestId is required', row)
                continue
            group = requests.setdefault(request_key, {})
            if stage in group:
                issue('ocr-stage-ambiguous', f'duplicate {stage} for the same request', row)
                group[stage] = None
            else:
                group[stage] = row

    output.mkdir(parents=True, exist_ok=True)
    # Prevent existing output symlinks from redirecting writes into immutable evidence.
    for name in ('report.json', 'ocr-replay.json', 'index.html', 'client', 'decoded'):
        target = output / name
        if target.is_symlink() or _inside(target.resolve(), root):
            raise ValueError(f'unsafe existing output target: {target}')

    previews = {}
    if reconstruct:
        for row in records:
            rid = row['recordId']
            if rid not in images or row.get('image', {}).get('encoding') != 'windows-xpress-chunks-v1':
                continue
            try:
                pixels = _decode_image(images[rid], row['image'])
                (output / 'decoded').mkdir(exist_ok=True)
                path = output / 'decoded' / f'{rid}.png'
                if path.is_symlink() or _inside(path.resolve(), root):
                    raise ValueError('unsafe decoded preview output')
                encoded, png = cv2.imencode('.png', pixels)
                if not encoded:
                    raise ValueError('decoded preview PNG encoding failed')
                path.write_bytes(png.tobytes())
                previews[rid] = path
                report['decodedPreviews'].append({'recordId': rid, 'path': str(path),
                                                  'partial': row['metadata'].get('partial')})
            except (OSError, ValueError, cv2.error) as exc:
                issue('preview-invalid', exc, row)

    for row in records:
        if row.get('stage') != 'client-crop':
            continue
        source = sources.get(_key(row, ('captureRun', 'sourceSequence')))
        if source is None:
            issue('source-outside-session', 'parent WGC sequence unavailable (recording boundary or ambiguous source)', row, gap=True)
            continue
        if source['recordId'] not in images:
            issue('client-source-invalid', 'recorded WGC image is corrupt or unavailable', row)
            continue
        try:
            crop = row['metadata']['crop']
            x, y, width, height = _rectangle(crop, source['image']['width'], source['image']['height'])
            if not reconstruct:
                continue
            source_pixels = _decode_image(images[source['recordId']], source['image'])
            (output / 'client').mkdir(exist_ok=True)
            path = output / 'client' / f'{row["recordId"]}.png'
            if path.is_symlink() or _inside(path.resolve(), root):
                raise ValueError('unsafe reconstructed image output')
            encoded, png = cv2.imencode('.png', source_pixels[y:y + height, x:x + width])
            if not encoded:
                raise ValueError('client PNG encoding failed')
            path.write_bytes(png.tobytes())
            report['reconstructed'].append({'recordId': row['recordId'], 'context': row['context'], 'path': str(path),
                                            'crop': crop, 'parentRecordId': source['recordId'],
                                            'partial': source['metadata'].get('partial'),
                                            'sourceValidRegions': source['metadata'].get('validRegions')})
        except (KeyError, ValueError, OSError, cv2.error) as exc:
            issue('client-crop-invalid', exc, row)

    replay = []
    for key, stages in requests.items():
        required = ('ocr-raw', 'ocr-contrast', 'ocr-detections')
        if any(not stages.get(stage) for stage in required):
            row = next((r for r in stages.values() if r), None)
            issue('ocr-stage-missing', 'request lacks an unambiguous raw/contrast/detections stage (session boundary possible)', row, gap=True)
            continue
        raw, contrast, detections = (stages[stage] for stage in required)
        if raw['recordId'] not in images or contrast['recordId'] not in images:
            issue('ocr-image-invalid', 'request images failed validation', detections)
            continue
        observations = detections['metadata']
        if not isinstance(observations.get('raw'), list) or not isinstance(observations.get('contrast'), list):
            issue('ocr-observations-invalid', 'raw and contrast observations must be arrays', detections)
            continue
        entry = {'id': f'record-{detections["recordId"]}', 'context': detections['context'],
                 'images': {'raw': str(images[raw['recordId']]), 'contrast': str(images[contrast['recordId']])},
                 'observations': {name: observations[name] for name in ('raw', 'contrast')},
                 'calibrated': observations.get('calibrated', False)}
        crop = stages.get('ocr-crop')
        if crop and crop['recordId'] in images:
            entry['crop'] = str(images[crop['recordId']])
        replay.append(entry)
        if key[:2] not in published:
            issue('publication-outside-session', 'OCR frame publication unavailable at recording boundary', detections, gap=True)
    report['replayCount'] = len(replay)

    if model is not None:
        try:
            inner = metadata.get('metadata', {})
            expected = inner.get('modelSha256') if isinstance(inner, dict) else None
            if not isinstance(expected, str) or len(expected) != 64:
                raise ValueError('session coordinate model SHA256 is unavailable')
            digest = hashlib.sha256()
            with (Path(model) / 'coordinate.onnx').open('rb') as stream:
                for chunk in iter(lambda: stream.read(1024 * 1024), b''):
                    digest.update(chunk)
            actual = digest.hexdigest()
            report['modelCheck'] = {'expected': expected, 'actual': actual, 'matches': expected.lower() == actual}
            if expected.lower() != actual:
                issue('model-mismatch', 'coordinate.onnx differs from the model recorded in this session')
        except (OSError, ValueError) as exc:
            issue('model-unverifiable', exc)
    else:
        report['modelCheck'] = {'checked': False, 'reason': '--model not supplied; inspection does not require a model'}

    report['ok'] = not report['errors']
    for name, value in [('report.json', report), ('ocr-replay.json', replay)]:
        (output / name).write_text(json.dumps(value, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    reconstructed = {r['recordId']: Path(r['path']) for r in report['reconstructed']}
    parts = ['<!doctype html><meta charset="utf-8"><title>Repro evidence</title>',
             '<style>body{font:15px system-ui;max-width:1100px;margin:2em auto}pre{white-space:pre-wrap;overflow-wrap:anywhere}summary{cursor:pointer}details{margin:1em 0}a{margin-right:1em}</style>',
             '<h1>Repro evidence</h1>', '<p>' + html.escape(SPARSE_NOTICE) + '</p>',
             '<p><a href="report.json">Validation report</a><a href="ocr-replay.json">Native OCR replay manifest</a></p>',
             '<pre>' + html.escape(json.dumps({'complete': completion.get('complete'), 'ok': report['ok'],
                                              'records': len(records), 'replayRequests': len(replay),
                                              'errors': report['errors'], 'gaps': report['gaps']}, ensure_ascii=False, indent=2)) + '</pre>']
    for row in records:
        rid = row['recordId']
        label = f'{rid} {row.get("kind")} {row.get("stage")} {json.dumps(row["context"], ensure_ascii=False)}'
        parts.append('<details><summary>' + html.escape(label) + '</summary>')
        image_path = images.get(rid)
        if image_path is not None and row.get('image', {}).get('encoding') == 'windows-xpress-chunks-v1':
            image_path = previews.get(rid)
            if image_path is None:
                parts.append('<p>XPRESS source validated. Use --reconstruct-client to generate a lossless PNG preview.</p>')
        for name, path in [('Stage image (may be sparse)', image_path), ('Reconstructed client (may be sparse)', reconstructed.get(rid))]:
            if path:
                parts.append('<a href="' + html.escape(path.as_uri(), quote=True) + '">' + name + '</a>')
        parts.append('<pre>' + html.escape(json.dumps(row, ensure_ascii=False, indent=2)) + '</pre></details>')
    (output / 'index.html').write_text('\n'.join(parts), encoding='utf-8')
    return report


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('session', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--reconstruct-client', action='store_true')
    parser.add_argument('--model', type=Path)
    args = parser.parse_args(argv)
    try:
        report = inspect_session(args.session, args.output, args.reconstruct_client, args.model)
    except (OSError, ValueError) as exc:
        print(f'Inspection failed: {exc}', file=sys.stderr)
        return 2
    print(f'{"OK" if report["ok"] else "INVALID/INCOMPLETE"}: {report["recordCount"]} records, '
          f'{report["replayCount"]} replay requests, {len(report["errors"])} errors, {len(report["gaps"])} boundary gaps. '
          f'Report: {args.output.resolve() / "report.json"}')
    return 0 if report['ok'] else 1


if __name__ == '__main__':
    sys.exit(main())
