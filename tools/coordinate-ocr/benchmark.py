#!/usr/bin/env python3
"""Independent, label-blind inference with explicit scoring and input provenance."""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import statistics
import subprocess
import sys
import time

PROFILES = ('production', 'readout-wide', 'upstream-band')


def read_json(path):
    return json.loads(Path(path).read_text(encoding='utf-8-sig'))


def write_json(path, value):
    Path(path).parent.mkdir(parents=True, exist_ok=True)
    Path(path).write_text(json.dumps(value, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')


def sha256(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def pixel_digest(image):
    # FNV-1a over decoded BGR bytes, independently computed by the native adapter.
    value = 14695981039346656037
    for byte in image.tobytes():
        value = ((value ^ byte) * 1099511628211) & 0xffffffffffffffff
    return f'{value:016x}'


def roi(profile, width, height):
    if width <= 0 or height <= 0:
        raise ValueError('empty frame')
    if profile == 'upstream-band':
        return 0, height - max(1, height // 32), max(1, width // 4), max(1, height // 32)
    boxes = {'production': (20, 865, 160, 900), 'readout-wide': (20, 870, 248, 900)}
    if profile not in boxes:
        raise ValueError(f'unknown ROI profile: {profile}')
    left, top, right, bottom = boxes[profile]
    scale = min(width / 1600, height / 900)
    # Positive pixel coordinates use C++ std::lround, not Python ties-to-even round.
    x1, x2 = (math.floor(value * scale + .5) for value in (left, right))
    y1, y2 = (math.floor(height - (900 - value) * scale + .5) for value in (top, bottom))
    return x1, y1, x2 - x1, y2 - y1


def prepare(corpus, data_root, output, profiles):
    import cv2
    if not isinstance(corpus.get('frames'), list) or not corpus['frames']:
        raise ValueError('empty or invalid corpus')
    if not profiles or len(set(profiles)) != len(profiles):
        raise ValueError('empty or duplicate ROI profiles')
    for frame in corpus['frames']:
        if frame.get('state') not in ('present', 'absent', 'unknown'):
            raise ValueError('invalid frame state')
        expected = frame.get('expected')
        if expected is not None:
            if (frame['state'] != 'present' or not isinstance(expected, dict) or
                any(type(expected.get(axis)) is not int for axis in ('x', 'y')) or
                (expected.get('z') is not None and type(expected['z']) is not int)):
                raise ValueError(f'invalid coordinate label for {frame.get("id")}')
            if any(value is not None and not -2147483648 <= value <= 2147483647 for value in expected.values()):
                raise ValueError('coordinate label outside int32 range')
    output = Path(output).resolve()
    inputs = []
    seen = set()
    for frame in corpus['frames']:
        frame_id = frame['id']
        if frame_id in seen or not frame_id or any(c not in 'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_' for c in frame_id):
            raise ValueError(f'duplicate or unsafe frame id: {frame_id}')
        seen.add(frame_id)
        source = Path(data_root) / frame['image']
        if not source.is_file():
            raise ValueError(f'missing image: {source}')
        # Decode from bytes so Unicode filenames are supported by the Python adapter.
        import numpy as np
        image = cv2.imdecode(np.frombuffer(source.read_bytes(), dtype=np.uint8), cv2.IMREAD_COLOR)
        if image is None:
            raise ValueError(f'unreadable image: {source}')
        stored_height, stored_width = image.shape[:2]
        width, height = frame.get('frameSize', [stored_width, stored_height])
        source_rect = frame.get('sourceRect', [0, 0, stored_width, stored_height])
        if (len(source_rect) != 4 or any(type(value) is not int for value in source_rect) or
            type(width) is not int or type(height) is not int or width <= 0 or height <= 0):
            raise ValueError(f'invalid source geometry: {frame_id}')
        sx, sy, sw, sh = source_rect
        if (sw != stored_width or sh != stored_height or sx < 0 or sy < 0 or
            sx+sw > width or sy+sh > height):
            raise ValueError(f'stored pixels do not match source geometry: {frame_id}')
        capture_digest = frame.get('captureSHA256')
        if corpus.get('capturePairedMinimap') or 'captureSHA256' in frame:
            if (not isinstance(capture_digest, str) or len(capture_digest) != 64 or
                    any(char not in '0123456789abcdef' for char in capture_digest)):
                raise ValueError(f'missing or invalid capture image digest: {frame_id}')
        if capture_digest is not None and sha256(source) != capture_digest:
            raise ValueError(f'capture image digest mismatch: {frame_id}')
        minimap = frame.get('minimap')
        if corpus.get('capturePairedMinimap') or minimap is not None:
            if (not isinstance(minimap, dict) or minimap.get('frameId') != frame_id or
                    type(frame.get('frameSystemRelativeTime100ns')) is not int):
                raise ValueError(f'invalid minimap frame pairing: {frame_id}')
            mini_source = Path(data_root) / minimap['image']
            if not mini_source.is_file():
                raise ValueError(f'missing paired minimap: {mini_source}')
            mini_data = mini_source.read_bytes()
            if hashlib.sha256(mini_data).hexdigest() != minimap['sha256']:
                raise ValueError(f'minimap digest mismatch: {frame_id}')
            mini_image = cv2.imdecode(np.frombuffer(mini_data, dtype=np.uint8), cv2.IMREAD_COLOR)
            rect = minimap['sourceRect']
            if len(rect) != 4 or any(type(value) is not int for value in rect):
                raise ValueError(f'invalid minimap geometry: {frame_id}')
            mx, my, mw, mh = rect
            if (mini_image is None or mini_image.shape[:2] != (mh, mw) or mx < 0 or my < 0 or
                    mw <= 0 or mh <= 0 or mx+mw > width or my+mh > height):
                raise ValueError(f'invalid minimap pixel geometry: {frame_id}')
        for profile in profiles:
            x, y, w, h = roi(profile, width, height)
            if w <= 0 or h <= 0 or x < 0 or y < 0 or x + w > width or y + h > height:
                raise ValueError(f'invalid crop: {frame_id} {profile}')
            if x < sx or y < sy or x+w > sx+sw or y+h > sy+sh:
                raise ValueError(f'saved strip does not cover ROI: {frame_id} {profile}')
            crop = image[y-sy:y-sy+h, x-sx:x-sx+w].copy()
            target = output / profile / f'{frame_id}.png'
            target.parent.mkdir(parents=True, exist_ok=True)
            ok, encoded = cv2.imencode('.png', crop)
            if not ok:
                raise ValueError(f'cannot encode crop: {target}')
            target.write_bytes(encoded.tobytes())
            # These are deliberately the only fields passed to either engine.
            inputs.append({'id': f'{frame_id}@{profile}', 'sourceId': frame_id,
                           'profile': profile, 'image': str(target), 'rect': [x, y, w, h],
                           'frameSize': [width, height], 'sourceSHA256': sha256(source),
                           'cropSHA256': sha256(target), 'pixelDigest': pixel_digest(crop)})
    return inputs


def validate_results(inputs, results):
    wanted = {row['id']: row for row in inputs}
    if len(wanted) != len(inputs):
        raise ValueError('duplicate input ids')
    found = set()
    for row in results:
        key = row['id']
        if key in found:
            raise ValueError(f'duplicate result id: {key}')
        found.add(key)
        if key not in wanted:
            raise ValueError(f'unexpected result id: {key}')
        if row.get('error'):
            raise ValueError(f'engine error for {key}: {row["error"]}')
        if row.get('pixelDigest') != wanted[key]['pixelDigest']:
            raise ValueError(f'different input pixels for {key}')
        if 'selected' not in row:
            raise ValueError(f'missing selected field for {key}')
        selected = row.get('selected')
        if selected is not None and (not isinstance(selected, dict) or
                any(type(selected.get(axis)) is not int for axis in ('x', 'y', 'z'))):
            raise ValueError(f'invalid selected coordinate for {key}')
    missing = wanted.keys() - found
    if missing:
        raise ValueError(f'missing engine results: {sorted(missing)}')


def score(frame, prediction):
    selected = prediction.get('selected')
    expected = frame.get('expected')
    xy_known = expected is not None and all(type(expected.get(axis)) is int for axis in ('x', 'y'))
    xyz_known = xy_known and type(expected.get('z')) is int
    same = lambda candidate, axes: candidate is not None and all(candidate.get(axis) == expected[axis] for axis in axes)
    exact_xy = same(selected, ('x', 'y')) if xy_known else None
    exact_xyz = same(selected, ('x', 'y', 'z')) if xyz_known else None
    candidates = prediction.get('candidates', [])
    sign_errors = []
    if selected is not None and xy_known:
        for axis in ('x', 'y', 'z'):
            if type(expected.get(axis)) is int and expected[axis] != 0 and selected[axis] != 0 and (expected[axis] < 0) != (selected[axis] < 0):
                sign_errors.append(axis)
    return {'exactXY': exact_xy, 'exactXYZ': exact_xyz,
            'candidateExactXY': any(same(c, ('x', 'y')) for c in candidates) if xy_known else None,
            'candidateExactXYZ': any(same(c, ('x', 'y', 'z')) for c in candidates) if xyz_known else None,
            'abstained': selected is None,
            'wrongReturn': (selected is not None and not (exact_xyz if xyz_known else exact_xy)) if xy_known else None,
            'wrongXYReturn': (selected is not None and not exact_xy) if xy_known else None,
            'wrongXYZReturn': (selected is not None and not exact_xyz) if xyz_known else None,
            'falsePositive': selected is not None if frame['state'] == 'absent' else None,
            'signErrors': sign_errors}


def summarize(corpus, inputs, pipelines):
    labels = {frame['id']: frame for frame in corpus['frames']}
    summaries, scored = [], []
    for mode, results in pipelines.items():
        validate_results(inputs, results)
        predictions = {row['id']: row for row in results}
        for profile in dict.fromkeys(row['profile'] for row in inputs):
            rows, durations = [], []
            for item in inputs:
                if item['profile'] != profile:
                    continue
                frame = labels[item['sourceId']]
                prediction = predictions[item['id']]
                judgment = score(frame, prediction)
                rows.append(judgment)
                durations.append(prediction['totalMilliseconds'])
                scored.append({'mode': mode, 'id': item['id'], 'state': frame['state'],
                               'expected': frame.get('expected'), **judgment, **prediction})
            count = lambda field: sum(row[field] is True for row in rows)
            eligible = lambda field: sum(row[field] is not None for row in rows)
            summaries.append({'mode': mode, 'profile': profile, 'frames': len(rows),
                'xyCorrect': count('exactXY'), 'xyLabeled': eligible('exactXY'),
                'xyzCorrect': count('exactXYZ'), 'xyzLabeled': eligible('exactXYZ'),
                'candidateXYZ': count('candidateExactXYZ'),
                'wrongReturns': count('wrongReturn'), 'abstained': count('abstained'),
                'wrongXYReturns': count('wrongXYReturn'), 'wrongXYZReturns': count('wrongXYZReturn'),
                'falsePositives': count('falsePositive'), 'absentFrames': eligible('falsePositive'),
                'signErrorFrames': sum(bool(row['signErrors']) for row in rows),
                'totalP50Milliseconds': statistics.median(durations),
                'totalP95Milliseconds': sorted(durations)[max(0, math.ceil(.95 * len(durations))-1)]})
    return summaries, scored


def provenance(source_root, data_root, corpus_path, native, upstream):
    import cv2
    import numpy as np
    import onnxruntime as ort
    def git_head(root):
        return subprocess.check_output(['git', '-C', str(root), 'rev-parse', 'HEAD'], text=True).strip()
    files = {'nativeExecutable': native,
             'preprocessingSource': source_root / 'IMao-Core/src/Coordinate/IdentifyWorldCoordinates/IdentifyWorldCoordinates.cpp',
             'parserSource': source_root / 'IMao-Core/src/Coordinate/IdentifyWorldCoordinates/CoordinateCandidate.cpp',
             'upstreamCode': upstream / 'src/ocr_engine.py',
             'upstreamModel': upstream / 'models/coord_ocr.onnx',
             'upstreamDictionary': upstream / 'models/class_names.txt',
             'nativeAdapterSource': source_root / 'IMao-Core/tools/CoordinateOffline/main.cpp',
             'nativeBuildConfiguration': source_root / 'IMao-Core/tools/CoordinateOffline/CMakeLists.txt',
             'nativeBuildScript': source_root / 'scripts/Build-CoordinateOffline.ps1',
             'ppocrLibrary': source_root / 'IMao-Core/paddleocr_cpp_infer/lib/ppocr.lib',
             'ppocrHeader': source_root / 'IMao-Core/paddleocr_cpp_infer/include/ocr_rec.h',
             'parserHeader': source_root / 'IMao-Core/src/Coordinate/IdentifyWorldCoordinates/CoordinateCandidate.h',
             'coordinateStructHeader': source_root / 'IMao-Core/src/Coordinate/CoordinateStruct.h',
             'hudLayoutHeader': source_root / 'IMao-Core/src/Coordinate/HudLayout.h',
             'ppocrPreprocessHeader': source_root / 'IMao-Core/paddleocr_cpp_infer/include/preprocess_op.h',
             'ppocrUtilityHeader': source_root / 'IMao-Core/paddleocr_cpp_infer/include/utility.h',
             'corpus': corpus_path,
             'benchmarkCode': Path(__file__),
             'upstreamAdapterCode': Path(__file__).with_name('upstream_adapter.py')}
    pp = data_root / 'Assets/models/PP-OCRv5_mobile_rec_infer'
    for path in sorted(pp.iterdir()):
        if path.is_file():
            files[f'ppocr/{path.name}'] = path
    return {'sourceCommit': git_head(source_root), 'upstreamCommit': git_head(upstream),
            'versions': {'python': sys.version, 'opencvPython': cv2.__version__,
                         'numpy': np.__version__, 'onnxruntime': ort.__version__},
            'sha256': {name: sha256(path) for name, path in files.items()}}


def verify_native_provenance(expected, metadata):
    compiled = metadata.get('compiledSHA256', {})
    for key in ('preprocessingSource', 'parserSource', 'nativeAdapterSource', 'nativeBuildConfiguration',
                'parserHeader', 'coordinateStructHeader', 'hudLayoutHeader',
                'ppocrLibrary', 'ppocrHeader', 'ppocrPreprocessHeader', 'ppocrUtilityHeader'):
        actual = compiled.get(key, metadata.get(key + 'SHA256'))
        if key in expected['sha256'] and actual != expected['sha256'][key]:
            raise ValueError(f'stale native executable: compiled {key} does not match current source; rebuild it')


def markdown(report):
    lines = ['# 坐标 OCR 独立离线评测', '',
        report.get('corpusNote') or '素材说明未提供；泛化结论需要独立验收素材。', '',
        '推理输入不含标签和位置先验。两侧读取同一份无缩放 BGR 裁剪，解码像素摘要逐条核对。', '',
        '单条输出与候选召回分别计分；未知 z 不计三维命中。无读数误报单独统计。', '',
        '本报告评估识别与解析，不模拟 App 的位置发布、位移预算或视觉定位。', '',
        '| 输入区域 | 管线 | XY 正确 | XYZ 正确 | 候选 XYZ 召回 | 错误返回 XY/XYZ | 无输出 | 无读数误报 | 总耗时 P50/P95 ms |',
        '|---|---|---:|---:|---:|---:|---:|---:|---:|']
    for row in report['summary']:
        lines.append(f"| {row['profile']} | {row['mode']} | {row['xyCorrect']}/{row['xyLabeled']} | "
                     f"{row['xyzCorrect']}/{row['xyzLabeled']} | {row['candidateXYZ']}/{row['xyzLabeled']} | "
                     f"{row['wrongXYReturns']}/{row['wrongXYZReturns']} | {row['abstained']} | {row['falsePositives']}/{row['absentFrames']} | "
                     f"{row['totalP50Milliseconds']:.1f}/{row['totalP95Milliseconds']:.1f} |")
    lines += ['', 'PP-OCR 单路线取解析器最高分候选（同分保留原顺序）；这是固定的离线选择规则。',
              'ppocr-corroborated 调用实际 Corroborate，以 TopHat 为主、contrast 为见证，匹配 XY。',
              'YOLO 使用上游的字符检测和 spacing-dominant 解析器，只计它实际接受的单条输出。',
              '耗时包含预处理、推理、解析；初始化单列，不包含磁盘读取、摘要计算和冷启动。',
              '三种取景只用于诊断：production=(20,865)-(160,900)，readout-wide=(20,870)-(248,900)，',
              'upstream-band=左侧四分之一宽、贴底高度 floor(H/32)。没有将扫描最高分当作新默认值。', '',
              '逐帧输出、原文、分数、修复标志、字形框、输入摘要和代码/模型 SHA256 见 report.json。', '']
    return '\n'.join(lines)


def run(args):
    source_root = Path(__file__).resolve().parents[2]
    data_root, upstream, native = map(lambda p: Path(p).resolve(), (args.data_root, args.upstream, args.native))
    corpus_path, output = Path(args.corpus).resolve(), Path(args.output).resolve()
    corpus = read_json(corpus_path)
    if (output / 'report.json').exists():
        raise ValueError('output already contains a report; choose a fresh directory to preserve evidence')
    if args.repeats < 1:
        raise ValueError('repeats must be positive')
    inputs = prepare(corpus, data_root, output / 'inputs', args.profiles)
    manifest = output / 'inputs.json'
    write_json(manifest, {'formatVersion': 1, 'inputs': inputs})
    provenance_before = provenance(source_root, data_root, corpus_path, native, upstream)
    env = dict(os.environ)
    env['PATH'] = str(data_root / 'x64/Release') + os.pathsep + env.get('PATH', '')
    native_report = output / 'ppocr.json'
    completed = subprocess.run([str(native), str(data_root / 'Assets/models/PP-OCRv5_mobile_rec_infer'),
                                str(manifest), str(native_report), str(args.repeats)],
                               env=env, capture_output=True, text=True, errors='replace')
    (output / 'native.log').write_text(completed.stdout + completed.stderr, encoding='utf-8')
    if completed.returncode != 0:
        raise ValueError(f'native adapter failed ({completed.returncode}); see {output / "native.log"}')
    native_payload = read_json(native_report)
    verify_native_provenance(provenance_before, native_payload['metadata'])
    sys.path.insert(0, str(Path(__file__).parent))
    from upstream_adapter import infer
    yolo = infer(upstream, inputs, args.repeats)
    write_json(output / 'yolo.json', yolo)
    pipelines = {name: rows for name, rows in native_payload['pipelines'].items()}
    pipelines['yolo'] = yolo['results']
    summaries, scored = summarize(corpus, inputs, pipelines)
    for item in inputs:
        if sha256(item['image']) != item['cropSHA256']:
            raise ValueError(f'input file changed during evaluation: {item["id"]}')
    provenance_after = provenance(source_root, data_root, corpus_path, native, upstream)
    if provenance_before != provenance_after:
        raise ValueError('model, source, or binary changed during evaluation')
    report = {'formatVersion': 1, 'corpusNote': corpus.get('note'), 'repeats': args.repeats,
              'sourceFrameIds': len(corpus['frames']),
              'uniqueSourceFrameHashes': len({row['sourceSHA256'] for row in inputs}), 'inputCases': len(inputs),
              'provenance': provenance_before, 'native': native_payload['metadata'],
              'yolo': yolo['metadata'], 'summary': summaries, 'samples': scored}
    write_json(output / 'report.json', report)
    (output / 'report.md').write_text(markdown(report), encoding='utf-8')
    print(markdown(report))


def main():
    sys.stdout.reconfigure(encoding='utf-8')
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--data-root', required=True, help='Existing checkout containing models and screenshots')
    parser.add_argument('--upstream', required=True, help='Existing local upstream checkout; never downloaded')
    parser.add_argument('--native', required=True, help='Freshly built IMaoCoordinateOffline.exe')
    parser.add_argument('--corpus', default=str(Path(__file__).resolve().parents[2] / 'Tests/CoordinateOffline/corpus.json'))
    parser.add_argument('--output', required=True)
    parser.add_argument('--profiles', nargs='+', choices=PROFILES, default=list(PROFILES))
    parser.add_argument('--repeats', type=int, default=3)
    args = parser.parse_args()
    try:
        run(args)
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        print(f'evaluation failed: {error}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
