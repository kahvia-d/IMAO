"""Build a file://-compatible, lazy v2 review viewer; never reads v1 results."""
import argparse
import json
import os
import re
from datetime import datetime, timezone
from pathlib import Path
from urllib.parse import quote, urlencode

from contract import can_start_matching, can_start_overview, validate_timeline

ROOT = Path(__file__).resolve().parents[2]


def read_json(path):
    return json.loads(path.read_text(encoding='utf-8-sig'))


def build(root=ROOT, output=None):
    root = Path(root).resolve()
    output = Path(output or root / 'out/detectability-v2/review/index.html').resolve()
    manifest = read_json(root / 'out/detectability-v2/manifest.json')
    assets, issues, parts, timelines = {}, [], [], []
    guide_index = {}
    for guide_path in (root / 'out').rglob('guide-*.json'):
        guide_index.setdefault(guide_path.name, guide_path)
    briefs_path = root / 'tools/MarkerDetectabilityV2/review-briefs.json'
    briefs = read_json(briefs_path) if briefs_path.exists() else {}


    def asset(value):
        if not isinstance(value, str) or not value:
            return
        if value in assets:
            return
        path = (root / value).resolve()
        try:
            path.relative_to(root)
        except ValueError:
            issues.append({'path': value, 'error': 'path outside repository'})
            return
        if not path.is_file():
            issues.append({'path': value, 'error': 'missing file'})
            return
        assets[value] = quote(os.path.relpath(path, output.parent).replace('\\', '/'), safe='/')

    def evidence_assets(items):
        for item in items or []:
            if isinstance(item, str):
                asset(item)
            elif isinstance(item, dict):
                asset(item.get('path'))
                source=item.get('source','')
                if isinstance(source,str) and re.search(r'\.(?:json|txt|jpg|jpeg|png|gif|webp|mp4|m4a|html)$',source,re.I) and not source.startswith(('http://','https://')):
                    asset(source)
                locator = item.get('locator')
                if isinstance(locator, str) and locator:
                    # A locator may be prose or a timestamp, not a missing file.
                    local, _, fragment = locator.partition('#')
                    try:
                        path = (root / local).resolve()
                        path.relative_to(root)
                        exists = path.is_file()
                    except (ValueError, OSError):
                        exists = False
                    if exists:
                        asset(local)
                        assets[locator] = assets[local] + ('#' + quote(fragment, safe='') if fragment else '')
                    elif local.startswith(('../', '..\\')) or Path(local).is_absolute() or (re.search(r'\.(?:json|txt|jpg|jpeg|png|gif|webp|mp4|m4a|html)$',local,re.I) and not local.startswith(('http://','https://'))):
                        asset(local)

    for entry in manifest:
        bvid, part = entry['bvid'], entry['part']
        folder = root / 'out/detectability-v2' / bvid / f'p{part:02}'
        timeline_path = root / 'tools/MarkerDetectabilityV2/timelines' / f'{bvid}-p{part:02}.json'
        timeline = read_json(timeline_path) if timeline_path.exists() else None
        media_path = folder / 'media.json'
        media = read_json(media_path) if media_path.exists() else {}
        transcript_path = folder / 'transcript.raw.json'
        transcript = read_json(transcript_path) if transcript_path.exists() else {}
        video = media.get('video', {}).get('path') or entry.get('videoPath')
        audio = media.get('audio', {}).get('path')
        asset(video)
        asset(audio)
        errors = []
        if timeline:
            timelines.append(timeline)
            try:
                errors = validate_timeline(timeline)
            except (KeyError, TypeError, ValueError) as exc:
                errors = [f'invalid timeline: {exc}']
            for event in timeline.get('events', []):
                for group in event.get('identityGroups', []):
                    for field in ('positionEvidence', 'videoEvidence', 'guideEvidence'):
                        evidence_assets(group.get(field))
                event['reviewBrief'] = briefs.get(event['eventId'], {})
                evidence_assets(event.get('evidenceFrames'))
                asset(event.get('progressBarMark', {}).get('source'))
                for reward in event.get('rewards', []):
                    matching = reward.get('matchingReview') or {}
                    for field in ('positionEvidence', 'videoEvidence', 'guideEvidence', 'exclusionEvidence', 'diagnosticEvidence'):
                        evidence_assets(matching.get(field))
                    evidence_assets((matching.get('attributeReview') or {}).get('evidence'))
                    # Recover existing guide references for display only, never infer a match.
                    if not matching.get('candidates'):
                        refs = json.dumps(matching, ensure_ascii=False)
                        pairs = list(dict.fromkeys(re.findall(r'guide-(\d+)-(\d+)', refs)))
                        if event['eventId']=='BV13ZvrBtEde:p1:e014':
                            pairs=[('906',x) for x in ('1454160397155495936','1454159815621423104','1454159713108439040')]
                        if event['eventId']=='BV19kNu6AEVR:p1:e006':
                            pairs=[('910','1524481633250820096')]
                            pairs=[(m.group(1),m.group(2)) for name in guide_index for m in [re.match(r'guide-(\d+)-(1524481633250820096)\.json',name)] if m]
                        matching['candidates']=[dict(stateId=a,pointId=b,status='candidate-not-confirmed') for a,b in pairs]
                    for candidate in matching.get('candidates', []):
                        if isinstance(candidate, dict):
                            evidence_assets(candidate.get('evidence'))
                            cached = guide_index.get(f"guide-{candidate.get('stateId')}-{candidate.get('pointId')}.json")
                            if cached:
                                detail = read_json(cached).get('data') or {}
                                content = detail.get('content') or {}
                                if isinstance(content, dict):
                                    candidate['name'] = candidate.get('name') or detail.get('name')
                                    candidate['guideImages'] = candidate.get('guideImages') or [str(x.relative_to(root)).replace('\\','/') for x in sorted(cached.parent.glob(cached.stem+'-*.png'))]
                                    candidate['displayDescription'] = content.get('description') or candidate.get('analysis','')
                                    if detail.get('countryId') is not None and detail.get('typeId'):
                                        candidate['guideUrl'] = 'https://www.kurobbs.com/mc/map/?' + urlencode(dict(state=candidate['stateId'],country=detail['countryId'],typeId=detail['typeId'],pointId=candidate['pointId']))
                            candidate['displayImages'] = [x for x in candidate.get('guideImages',[]) if (root/x).is_file()]
                            evidence_assets(candidate['displayImages'])
        parts.append(dict(manifest=entry, timeline=timeline, errors=errors,
                          video=video, audio=audio, alignmentStatus=media.get('alignmentStatus'),
                          transcript=transcript.get('segments', [])))
    overview_path=root/'tools/MarkerDetectabilityV2/overview-review.json'
    overview=read_json(overview_path) if overview_path.exists() else {}
    overview_by_key={(p['bvid'],p['part']):p for p in overview.get('parts',[])}
    for part in parts:
        part['overview']=overview_by_key.get((part['manifest']['bvid'],part['manifest']['part']))
        if part['overview']:
            evidence_assets(part['overview'].get('evidence',[]))
    result_path=root/'tools/MarkerDetectabilityV2/results/summary.json'
    results=read_json(result_path) if result_path.exists() else None
    downloads={}
    for name in ('summary','positives','unresolved','matching','v1-diff'):
        path=f'tools/MarkerDetectabilityV2/results/{name}.json'
        if (root/path).exists():asset(path);downloads[name]=path
    keys = [(m['bvid'], m['part']) for m in manifest]
    valid = not any(p['errors'] for p in parts)
    matching_gate = valid and can_start_matching(keys, timelines)
    overview_gate = valid and can_start_overview(keys, timelines)
    data = dict(generatedAt=datetime.now(timezone.utc).isoformat(), parts=parts, assets=assets,
                pathIssues=issues, gates=dict(matching=matching_gate, overview=overview_gate), overviewStatus=overview.get('status','pending'), results=results, downloads=downloads)
    template = Path(__file__).with_name('review_template.html').read_text(encoding='utf-8')
    payload = json.dumps(data, ensure_ascii=False).replace('<', '\\u003c').replace('>', '\\u003e').replace('&', '\\u0026')
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(template.replace('__REVIEW_DATA__', payload), encoding='utf-8')
    report = dict(output=str(output), parts=len(parts), timelines=len(timelines),
                  matchingGate=matching_gate, overviewGate=overview_gate,
                  checkedAssets=len(assets), pathIssues=issues)
    output.with_name('build-report.json').write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
    return report


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    print(json.dumps(build(output=args.output), ensure_ascii=False, indent=2))
