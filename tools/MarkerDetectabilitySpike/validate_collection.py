"""Integrity checks for the offline curation, not a test of visual correctness."""
import hashlib
import json
from collections import Counter
from pathlib import Path

from PIL import Image
from collection_utils import ROOT, OUT, points

HERE = Path(__file__).resolve().parent


def read(path):
    return json.loads(path.read_text('utf-8-sig'))


def main():
    data = read(HERE/'collection-results.json')
    errors, images, checked_paths = [], set(), set()
    def check(condition, message):
        if not condition:
            errors.append(message)
    def exists(name):
        if name:
            path = (ROOT/name).resolve()
            check(path.is_relative_to(ROOT), f'Path outside repository: {name}')
            check(path.is_file(), f'Missing evidence: {name}')
            checked_paths.add(name)
    check(data['runtimeIntegration'] is False and data['experimentOnly'] is True, 'Offline-only flags')
    check(data['unlistedPointDefault']=='unknown', 'Unlisted point default')
    states = sorted({x['stateId'] for x in data['targets'] if x['identityStatus']=='confirmed'})
    lookup = {(p['stateId'],str(p['id'])):p for p in points(states)}
    parts = {(x['bvid'],x['part']):x for x in data['parts']}
    counts = Counter()
    keys = set()
    for r in data['targets']:
        key = r['targetKey']
        check(key not in keys, f'Duplicate target key: {key}')
        keys.add(key)
        part_key = (r['bvid'], r['part'])
        check(part_key in parts, f'Unknown inventory part: {key}')
        counts[part_key] += 1
        interval = r.get('segmentSecondsApprox')
        if interval and part_key in parts:
            check(0 <= interval[0] <= interval[1] <= parts[part_key]['durationSeconds'],f'Bad time interval: {key} {interval}')
        ev = r['localEvidence']
        for p in ev.get('frames', []) + ev.get('guideImagesReviewed', []) + ev.get('rejectedVideoFrames', []):
            exists(p)
            images.add(p)
        exists(ev.get('guideJson'))
        if r['identityStatus']=='confirmed':
            check(isinstance(r['pointId'],str), f'Point ID not string: {key}')
            point_key = (r['stateId'],r['pointId'])
            check(point_key in lookup,f'Unknown point: {point_key}')
            if point_key in lookup:
                check(lookup[point_key]['typeId']==r['typeId'],f'Wrong type: {key}')
            check(bool(ev.get('guideImagesReviewed')), f'Confirmed without reviewed image: {key}')
            check(bool(ev.get('frames')), f'Confirmed without source frame: {key}')
            check(bool(r['basis']),f'Confirmed without reason: {key}')
            if ev.get('guideJson') and (ROOT/ev['guideJson']).exists():
                guide = read(ROOT/ev['guideJson'])
                guide = guide.get('data',guide)
                check(str(guide.get('id'))==r['pointId'], f'Guide ID mismatch: {key}')
                check(guide.get('stateId')==r['stateId'], f'Guide state mismatch: {key}')
        else:
            check(r['pointId'] is None, f'Unresolved identity has assigned ID: {key}')
            check(r['detectability']=='unknown',f'Unresolved identity labeled: {key}')
            check(bool(r.get('unresolvedReason')),f'Unresolved without reason: {key}')
    for key,p in parts.items():
        check(p['records']==counts[key],f'Part count mismatch: {key}')
        check(p['isIntro'] or counts[key]>0, f'Part has no reviewed target records: {key}')
    for p in images:
        try:
            with Image.open(ROOT/p) as im:
                im.verify()
        except Exception as e:
            errors.append(f'Unreadable image {p}: {type(e).__name__}')
    media = []
    for p in data['parts']:
        bv,part = p['bvid'],p['part']
        if bv=='BV1evKmzxE34':
            video=ROOT/'out/detectability-spike-blackshores/video-480p.mp4'
            expected = None
        else:
            video=OUT/bv/f'p{part:02d}'/'video.mp4'
            expected=p['media']
        check(video.exists(),f'Missing video {bv} P{part}')
        if video.exists():
            with video.open('rb') as stream:
                digest=hashlib.file_digest(stream,'sha256').hexdigest()
            n=video.stat().st_size
            if expected:
                check(n==expected['bytes'],f'Media length mismatch: {bv} P{part}')
                check(digest==expected['sha256'],f'Media SHA mismatch: {bv} P{part}')
            media.append({'bvid':bv,'part':part,'path':video.relative_to(ROOT).as_posix(),'bytes':n,'sha256':digest})
    point_labels=data['pointLabels']
    label_keys={(x['stateId'],x['pointId']) for x in point_labels}
    check(len(label_keys)==len(point_labels),'Duplicate label IDs')
    eligible_keys={(r['stateId'],r['pointId']) for r in data['targets']
                   if r['identityStatus']=='confirmed' and r['detectability']=='not-detectable-for-normal-sweep'}
    check(label_keys==eligible_keys,'Labels differ from confirmed eligible identities')
    for label in point_labels:
        refs=[r for r in data['targets'] if r['targetKey'] in label['sourceTargetKeys']]
        check(bool(refs) and all(r['identityStatus']=='confirmed' and r['detectability']=='not-detectable-for-normal-sweep' for r in refs),f'Invalid label source {label["pointId"]}')
        check(len(refs)==len(label['sourceTargetKeys']) and all((r['stateId'],r['pointId'])==(label['stateId'],label['pointId']) for r in refs),f'Label source identity mismatch {label["pointId"]}')
    report={'passed':not errors,'errors':errors,'targetRecords':len(data['targets']),
            'inventoryParts':len(parts),'reviewedEvidenceImagesChecked':len(images),
            'evidencePathsChecked':len(checked_paths),'mediaFilesHashed':len(media),
            'downloadedVideoBytes':sum(x['bytes'] for x in media),
            'scope':'IDs/state/type, evidence readability, source timing, per-part coverage, media SHA256, label eligibility; not visual accuracy or in-game detector retest.'}
    (OUT/'verification.json').write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    (OUT/'media-manifest.json').write_text(json.dumps(media,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    print(json.dumps(report,ensure_ascii=True))
    raise SystemExit(0 if report['passed'] else 1)


if __name__=='__main__':
    main()
