#!/usr/bin/env python3
"""Verify native capture sessions and export label-blind offline replay inputs."""
from __future__ import annotations
import argparse
import hashlib
import json
from pathlib import Path
import sys


def inspect_session(directory, *, allow_incomplete=False, allow_fixture=False, labels=None):
    import cv2
    import numpy as np
    directory = Path(directory).resolve()
    path = directory / 'index.jsonl'
    if not path.is_file():
        raise ValueError(f'missing session index: {path}')
    records = [json.loads(line) for line in path.read_text(encoding='utf-8-sig').splitlines() if line.strip()]
    if not records or records[0].get('type') != 'session-start' or records[0].get('formatVersion') != 1:
        raise ValueError('invalid capture session header')
    header = records[0]
    if header.get('origin') == 'fixture' and not allow_fixture:
        raise ValueError('fixture samples are not a real game dataset')
    if header.get('origin') not in ('fixture', 'game'):
        raise ValueError('unknown capture origin')
    complete = records[-1].get('type') == 'session-end'
    if any(row.get('type') == 'session-start' for row in records[1:]) or any(
            row.get('type') == 'session-end' for row in records[:-1]):
        raise ValueError('invalid session lifecycle: repeated header or records after session end')
    if not complete and not allow_incomplete:
        raise ValueError('unfinished capture session; use explicit recovery to inspect it')
    frames, ids, files, sizes, black, hashes = [], set(), set(), set(), 0, set()
    for row in records[1:]:
        if row.get('type') != 'sample':
            continue
        key = row['id']
        image_name = row['image']
        if key in ids or image_name in files:
            raise ValueError('duplicate sample id or file')
        ids.add(key)
        files.add(image_name)
        if Path(image_name).name != image_name or Path(image_name).suffix.lower() != '.png':
            raise ValueError('unsafe image path in session index')
        source = directory / image_name
        if not source.is_file():
            raise ValueError(f'missing sample image: {source}')
        data = source.read_bytes()
        if hashlib.sha256(data).hexdigest() != row['sha256']:
            raise ValueError(f'sample digest mismatch: {image_name}')
        hashes.add(row['sha256'])
        image = cv2.imdecode(np.frombuffer(data, np.uint8), cv2.IMREAD_COLOR)
        width, height = row['frameSize']
        x, y, w, h = row['sourceRect']
        if (image is None or image.shape[:2] != (h, w) or x < 0 or y < 0 or
            w <= 0 or h <= 0 or x+w > width or y+h > height):
            raise ValueError(f'invalid pixel geometry: {image_name}')
        annotation = (labels or {}).get(key, {'state': 'unknown', 'expected': None})
        state, expected = annotation['state'], annotation.get('expected')
        if state not in ('unknown', 'present', 'absent'):
            raise ValueError(f'invalid label state: {key}')
        if expected is not None and (state != 'present' or not isinstance(expected, dict) or
            any(type(expected.get(axis)) is not int for axis in ('x', 'y')) or
            (expected.get('z') is not None and type(expected['z']) is not int)):
            raise ValueError(f'invalid coordinate label: {key}')
        frames.append({'id': key, 'image': str(source), 'state': state, 'expected': expected,
                       'frameSize': [width, height], 'sourceRect': [x, y, w, h],
                       'sourceGroup': header['sessionId'],
                       'annotation': {'origin': 'capture session; labels supplied separately',
                                      'note': annotation.get('note', '')}})
        sizes.add((width, height))
        black += int(not np.any(image))
    if complete and records[-1]['samples'] != len(frames):
        raise ValueError('session end sample count differs from index')
    unindexed = sorted(path.name for path in directory.iterdir()
                       if path.is_file() and path.name not in files and
                       (path.suffix.lower() == '.png' or '.tmp' in path.suffixes))
    if complete and unindexed:
        raise ValueError(f'complete session has unindexed image files: {unindexed}')
    if labels and labels.keys() - ids:
        raise ValueError(f'labels refer to unknown sample ids: {sorted(labels.keys()-ids)}')
    if not frames:
        raise ValueError('empty capture session: no usable samples')
    errors = [row for row in records if row.get('type') == 'error']
    return {'sessionId': header['sessionId'], 'complete': complete, 'samples': len(frames),
            'origin': header['origin'], 'clientSizes': sorted(sizes), 'allBlackSamples': black,
            'uniqueImages': len(hashes),
            'unindexedFiles': unindexed,
            'captureDiagnostics': [row for row in records if row.get('state') in ('geometry-refused', 'capture-no-samples', 'producer-rate-limit-unavailable')],
            'errors': errors, 'stopReason': records[-1].get('reason') if complete else None,
            'corpus': {'formatVersion': 1, 'captureOrigin': header['origin'], 'incompleteRecovery': not complete,
                       'note': 'Raw capture session; unlabelled frames are observational only. Fixture samples are integration probes, not real game data. Keep sessions together when splitting data.',
                       'frames': frames}}


def main():
    sys.stdout.reconfigure(encoding='utf-8')
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('session')
    parser.add_argument('--output', required=True, help='New inspection/export directory')
    parser.add_argument('--labels', help='Optional explicit labels.json; never inferred from model predictions')
    parser.add_argument('--allow-incomplete', action='store_true')
    parser.add_argument('--allow-fixture', action='store_true')
    args = parser.parse_args()
    try:
        labels = json.loads(Path(args.labels).read_text(encoding='utf-8-sig')) if args.labels else None
        report = inspect_session(args.session, allow_incomplete=args.allow_incomplete,
                                 allow_fixture=args.allow_fixture, labels=labels)
        output = Path(args.output).resolve()
        if output.exists() and any(output.iterdir()):
            raise ValueError('inspection output must be a new or empty directory')
        output.mkdir(parents=True, exist_ok=True)
        (output/'corpus.json').write_text(json.dumps(report.pop('corpus'), ensure_ascii=False, indent=2)+'\n', encoding='utf-8')
        (output/'inspection.json').write_text(json.dumps(report, ensure_ascii=False, indent=2)+'\n', encoding='utf-8')
        print(json.dumps(report, ensure_ascii=False, indent=2))
        print(f'Replay corpus: {output / "corpus.json"}')
        return 0
    except (OSError, ValueError, KeyError, TypeError) as error:
        print(f'capture inspection failed: {error}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
