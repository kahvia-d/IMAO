"""Read-only dataset audit. No matching, overview or point database inputs.

Exit 0 means all requested source/annotation integrity checks passed; overview
readiness remains a separate field. --report only writes the audit report.
"""
import argparse
import json
import math
from pathlib import Path
import sys

from contract import validate_timeline, validate_matching, can_start_matching, can_start_overview


def audit(root, manifest_path, timeline_dir, expected_parts=26, tolerance=0.25):
    root = root.resolve()
    errors, parts, timelines = [], [], []

    def error(code, source, detail):
        errors.append({'code': code, 'source': str(source), 'detail': detail})

    def read(path):
        try:
            return json.loads(path.read_text('utf-8-sig'))
        except (OSError, ValueError) as exc:
            error('unreadable-json', path, type(exc).__name__)
            return None

    def local(value, source, kind):
        if not isinstance(value, str) or not value.strip():
            error('invalid-path', source, kind)
            return None
        path = (root / value).resolve()
        if not path.is_relative_to(root):
            error('unsafe-path', source, value)
            return None
        if not path.is_file():
            error('missing-' + kind, source, value)
            return None
        return path

    def finite(value):
        return type(value) in (int, float) and math.isfinite(value)

    manifest = read(manifest_path)
    if not isinstance(manifest, list):
        error('invalid-manifest', manifest_path, 'Expected array')
        manifest = []
    keys = []
    for item in manifest:
        if not isinstance(item, dict) or not isinstance(item.get('bvid'), str) or type(item.get('part')) is not int:
            error('invalid-manifest-part', manifest_path, 'bvid and integer part required')
            continue
        keys.append((item['bvid'], item['part']))
    if len(keys) != expected_parts:
        error('manifest-count', manifest_path, f'Expected {expected_parts}; found {len(keys)}')
    if len(set(keys)) != len(keys):
        error('duplicate-manifest-part', manifest_path, 'Source parts must be unique')
    found = {}
    for path in sorted(timeline_dir.glob('*.json')):
        t = read(path)
        if not isinstance(t, dict):
            continue
        key = (t.get('bvid'), t.get('part'))
        if not isinstance(key[0], str) or type(key[1]) is not int:
            error('invalid-timeline-source', path, 'bvid and integer part required')
            continue
        if key in found:
            error('duplicate-timeline', path, str(key))
        found[key] = (path, t)
        timelines.append(t)
        if key not in keys:
            error('unexpected-timeline', path, str(key))
    for item in manifest:
        if not isinstance(item, dict):
            continue
        key = (item.get('bvid'), item.get('part'))
        if key not in found:
            error('missing-timeline', manifest_path, str(key))
            continue
        path, t = found[key]
        label = f'{key[0]}:p{key[1]}'
        before = len(errors)
        try:
            for detail in validate_timeline(t):
                error('timeline-contract', label, detail)
        except (KeyError, TypeError, ValueError) as exc:
            error('timeline-schema', label, type(exc).__name__)
        folder = root / 'out/detectability-v2' / key[0] / f'p{key[1]:02}'
        media = read(folder / 'media.json')
        raw = read(folder / 'transcript.raw.json')
        real = {}
        if isinstance(media, dict):
            for kind in ('video', 'audio'):
                metadata = media.get(kind)
                if not isinstance(metadata, dict):
                    error('missing-media-metadata', label, kind)
                    continue
                media_file = local(metadata.get('path'), label, 'media')
                if media_file is None:
                    continue
                try:
                    import av
                    with av.open(str(media_file)) as container:
                        seconds = container.duration / av.time_base if container.duration is not None else None
                        starts = [float(s.start_time * s.time_base) for s in container.streams if s.start_time is not None]
                    if not finite(seconds):
                        raise ValueError('no finite duration')
                    real[kind] = {'duration': seconds, 'start': min(starts) if starts else None}
                    recorded = metadata.get('duration')
                    if not finite(recorded) or abs(seconds - recorded) > tolerance:
                        error('media-metadata-duration', label, f'{kind}: actual={seconds}, metadata={recorded}')
                except Exception as exc:
                    error('media-probe-failed', label, f'{kind}: {type(exc).__name__}')
            if 'video' in real:
                seconds = real['video']['duration']
                if not finite(t.get('duration')) or abs(t['duration'] - seconds) > 0.05:
                    error('timeline-duration', label, f'actual={seconds}, timeline={t.get("duration")}')
                if finite(item.get('duration')) and abs(item['duration'] - seconds) > 1.5:
                    error('manifest-duration', label, 'Rounded catalog duration differs from source by over 1.5s')
                if item.get('videoPath') != media.get('video', {}).get('path'):
                    error('source-video-path', label, 'Manifest and media video paths differ')
            if 'video' in real and 'audio' in real:
                if abs(real['video']['duration'] - real['audio']['duration']) > tolerance:
                    error('audio-video-duration', label, 'Container durations exceed alignment tolerance')
                a, v = real['audio']['start'], real['video']['start']
                if a is None or v is None:
                    error('audio-video-start', label, 'Container start timestamp unavailable')
                elif abs(a - v) > tolerance:
                    error('audio-video-start', label, 'Container start timestamps exceed alignment tolerance')
        refs = set()
        if isinstance(raw, dict) and isinstance(raw.get('segments'), list):
            for segment in raw['segments']:
                if isinstance(segment, dict):
                    refs.add((segment.get('start'), segment.get('end'), segment.get('text')))
        else:
            error('invalid-transcript', label, 'Raw segments array required')
        evidence = list(t.get('reviewedEvidence', []))
        for event in t.get('events', []):
            evidence.extend(event.get('evidenceFrames', []))
            mark = event.get('progressBarMark', {})
            if isinstance(mark, dict) and mark.get('evidenceFrame'):
                evidence.append(mark['evidenceFrame'])
            for ref in event.get('transcriptRefs', []):
                if not isinstance(ref, dict) or (ref.get('start'), ref.get('end'), ref.get('text')) not in refs:
                    error('asr-reference-mismatch', label, event.get('eventId', 'unnamed event'))
        for value in evidence:
            local(value, label, 'evidence')
        parts.append({'bvid': key[0], 'part': key[1], 'events': len(t.get('events', [])),
                      'rewards': sum(len(e.get('rewards', [])) for e in t.get('events', [])),
                      'media': real, 'integrityValid': len(errors) == before})
    try:
        for detail in validate_matching(timelines):
            error('matching-contract', 'timelines', detail)
        matching = can_start_matching(keys, timelines)
        overview = can_start_overview(keys, timelines)
    except (KeyError, TypeError, ValueError) as exc:
        error('matching-schema', 'timelines', type(exc).__name__)
        matching = overview = False
    return {'valid': not errors, 'expectedParts': expected_parts, 'manifestParts': len(keys),
            'timelineParts': len(timelines), 'canStartMatching': not errors and matching,
            'canStartOverview': not errors and overview, 'parts': parts, 'errors': errors,
            'limitations': ['Container timestamp/duration alignment only; no direct listening or perceptual sync claim.',
                           'Evidence existence and exact raw ASR references are checked; factual conclusions still require review.']}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument('--manifest', type=Path, default=Path('out/detectability-v2/manifest.json'))
    parser.add_argument('--timelines', type=Path, default=Path('tools/MarkerDetectabilityV2/timelines'))
    parser.add_argument('--expected-parts', type=int, default=26)
    parser.add_argument('--alignment-tolerance', type=float, default=0.25)
    parser.add_argument('--report', type=Path)
    args = parser.parse_args()
    if args.expected_parts < 1 or not math.isfinite(args.alignment_tolerance) or args.alignment_tolerance < 0:
        parser.error('expected-parts must be positive and alignment-tolerance finite/nonnegative')
    report = audit(args.root, args.root / args.manifest, args.root / args.timelines,
                   args.expected_parts, args.alignment_tolerance)
    output = json.dumps(report, ensure_ascii=False, indent=2) + '\n'
    if args.report:
        destination = args.root / args.report
        # Existing annotation and media files must never be overwritten by reports.
        if destination.exists():
            parser.error('report destination already exists; use a new output path')
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_text(output, encoding='utf-8')
    if hasattr(sys.stdout, 'reconfigure'):
        sys.stdout.reconfigure(encoding='utf-8')
    print(output, end='')
    return 0 if report['valid'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
