"""Combine reviewed source records and produce an offline, searchable evidence index.

Run from the repository root after all curation JSON files are present. This script
does not download media, infer identities, or write production map data.
"""
import copy
import json
import re
from collections import defaultdict
from pathlib import Path

from collection_utils import ROOT, OUT

HERE = Path(__file__).resolve().parent
NONDETECTABLE = 'not-detectable-for-normal-sweep'


def read(path):
    return json.loads(path.read_text('utf-8-sig'))


def save(path, data):
    path.write_text(json.dumps(data, ensure_ascii=False, indent=2)+'\n', encoding='utf-8')


def normalize(record, filename, index):
    r = copy.deepcopy(record)
    if r.get('point'):
        r.update(r['point'])
    src, ev, guide = r.get('source') or {}, r.get('localEvidence') or {}, r.get('guide') or {}
    bv, part = r.get('bvid', src.get('bvid')), r.get('part', src.get('part', 1))
    key = r.get('targetKey', r.get('segmentId', f'{bv}:p{part}:record-{index+1}'))
    frames = ev.get('frames', ev.get('videoFrames', []))
    reviewed = ev.get('guideImagesReviewed', guide.get('reviewedImagePaths', guide.get('reviewedImages', [])))
    guide_json = ev.get('guideJson', guide.get('jsonPath', guide.get('localJson')))
    confirmed = r.get('pointId') is not None and r.get('reviewStatus', '').startswith('confirmed')
    return {
        'targetKey': key, 'bvid': bv, 'part': part,
        'sourceLabel': r.get('sourceLabel', f'目标 {r.get("ordinal", index+1)}'),
        'identityStatus': 'confirmed' if confirmed else 'unresolved',
        'pointId': str(r['pointId']) if confirmed else None,
        'stateId': r.get('stateId') if confirmed else None,
        'typeId': r.get('typeId') if confirmed else None,
        'name': r.get('name'), 'rawPosition': r.get('rawPosition') if confirmed else None,
        'detectability': r.get('detectability', 'unknown') if confirmed else 'unknown',
        'mechanism': r.get('mechanism', r.get('observedMechanism', '')),
        'basis': r.get('basis', ''), 'unresolvedReason': r.get('unresolvedReason'),
        'source': dict(src, bvid=bv, part=part, url=src.get('url', r.get('sourceUrl', f'https://www.bilibili.com/video/{bv}/?p={part}'))),
        'segmentSecondsApprox': src.get('segmentSecondsApprox', r.get('segmentSecondsApprox', r.get('timeRangeSeconds'))),
        'guide': guide,
        'localEvidence': dict(ev, frames=frames, guideImagesReviewed=reviewed, guideJson=guide_json),
        'candidates': r.get('candidates', []), 'groupKey': r.get('groupKey', r.get('groupId')),
        'groupNote': r.get('groupNote'),
        'curationFile': f'tools/MarkerDetectabilitySpike/{filename}', 'curationKey': key,
        'directProbeComparisonObserved': False}


def main():
    inventory = read(OUT/'inventory.json')
    targets, source_files, coverage, concerns, incidental, resolved = [], [], [], [], [], []
    inputs = [('huanglong-first-results.json', 'segments'), ('new-regions-results.json', 'targets'),
              ('root-results.json', 'targets'), ('rinascita-results.json', 'records')]
    for filename, records_key in inputs:
        data = read(HERE/filename)
        source_files.append(filename)
        targets += [normalize(r, filename, i) for i,r in enumerate(data[records_key])]
        coverage += data.get('reviewManifest', data.get('pageCoverage', data.get('coverage', [])))
        concerns += data.get('coverageConcerns', [])
        incidental += data.get('incidentalObservations', [])
        resolved += data.get('resolvedCoverageObservations', [])
        source_files += data.get('supplementalReviewFiles', [])

    # A second review confirms two existing rewards and adds two visible room
    # chests. Keep identity confirmation separate from detector classification.
    filename = 'p12-additional-review.json'
    additional = read(HERE/filename)
    source_files.append(filename)
    replacements = {'1288287493370998784': 'BV1iMTXzVE8J:p11:overview-10',
                    '1288578802371588096': 'BV1iMTXzVE8J:p12:overview-2'}
    for i,r in enumerate(additional['confirmedIdentityReviews']):
        n = normalize(r, filename, i)
        n['targetKey'] = replacements.get(r['pointId'], f'BV1iMTXzVE8J:p{r["part"]}:room-{r["pointId"]}')
        n['sourceLabel'] = r['name']+' / 晶核任务关联'
        targets = [x for x in targets if x['targetKey'] != n['targetKey']]
        targets.append(n)
    for item in additional['p12TargetInventory']:
        for r in targets:
            if r['bvid'] == 'BV1iMTXzVE8J' and r['part'] == 12 and r['pointId'] == item['pointId']:
                r['segmentSecondsApprox'] = item['segmentSecondsApprox']

    # The pilot retains its original evidence filenames; normalize those paths.
    filename = 'blackshores-spike.json'
    pilot = read(HERE/filename)
    source_files.append(filename)
    for i,r in enumerate(pilot['confirmedPoints']):
        r = copy.deepcopy(r)
        prefix = 'out/detectability-spike-blackshores/'
        ev = r['localEvidence']
        r['localEvidence'] = {
            'frames': [prefix+ev[k] for k in ['videoBefore','videoAfter']],
            'guideJson': prefix+'guide-'+r['pointId']+'.json',
            'guideImagesReviewed': [prefix+ev[k] for k in ['guideBefore','guideAfter']]}
        n = normalize(r, filename, i)
        n['targetKey'] = f'BV1evKmzxE34:p1:pilot-{i+1}'
        targets.append(n)

    apply_supplemental_reviews(targets, source_files)
    video_order = {v['bvid']: i for i,v in enumerate(inventory)}
    targets.sort(key=lambda x: (video_order[x['bvid']], x['part'],
                 [int(s) if s.isdigit() else s for s in re.split(r'(\d+)',x['targetKey'])]))
    confirmed = [x for x in targets if x['identityStatus']=='confirmed']
    labels = defaultdict(list)
    for r in confirmed:
        if r['detectability']==NONDETECTABLE:
            labels[(r['stateId'],r['pointId'])].append(r['targetKey'])
    unknown_identity = [x for x in targets if x['identityStatus']!='confirmed']
    known_unknown = [x for x in confirmed if x['detectability']!=NONDETECTABLE]

    parts = []
    for video in inventory:
        for page in video['pages']:
            bv, p = video['bvid'], page['page']
            group = [x for x in targets if x['bvid']==bv and x['part']==p]
            folder = OUT/bv/f'p{p:02d}'
            meta_path = folder/'media.json'
            parts.append({'bvid': bv, 'part': p, 'title': video['title'], 'partTitle': page['part'],
                          'durationSeconds': page['duration'], 'cid': str(page['cid']),
                          'url': f'https://www.bilibili.com/video/{bv}/?p={p}',
                          'records':len(group), 'confirmedIdentity':sum(x['identityStatus']=='confirmed' for x in group),
                          'nonDetectableRecords':sum(x['detectability']==NONDETECTABLE for x in group),
                          'isIntro':bv=='BV1iMTXzVE8J' and p==1,
                          'media':read(meta_path) if meta_path.exists() else None,
                          'reviewManifests': [x for x in coverage if x.get('bvid')==bv and x.get('part')==p]})
    result = {
        'schemaVersion': 1, 'date': '2026-10-29', 'baseCommit': pilot['baseCommit'],
        'experimentOnly': True, 'runtimeIntegration': False,
        'collectionUrl': 'https://space.bilibili.com/650388538/lists/5640337?type=season',
        'sourceAuthor': '又肝又氪ya', 'definition': pilot['definition'],
        'statusMeaning': 'confirmed仅表示代理做过视频/总览与库街区图文身份交叉核对；不是玩家实测，也不是全自动准确率。',
        'unlistedPointDefault': 'unknown',
        'sourceFiles': source_files,
        'summary': {'videos': len(inventory), 'parts': len(parts), 'remainingVideosProcessed':len(inventory)-1,
                    'remainingPartsProcessed':len(parts)-1, 'sourceTargetRecords':len(targets),
                    'confirmedIdentityRecords':len(confirmed), 'unresolvedIdentityRecords':len(unknown_identity),
                    'confirmedIdentityButUnknownDetectability':len(known_unknown),
                    'uniqueNonDetectablePoints':len(labels),
                    'durationSeconds':sum(x['durationSeconds'] for x in parts),
                    'remainingDownloadedVideoBytes':sum((x['media'] or {}).get('bytes',0) for x in parts)},
        'limitations': ['抽帧视觉复核并非连续逐帧观看，源视频跳剪/总览标记可能不完整；各分组保留局部覆盖缺口。',
                        '匹配困难、候选冲突、普通沿途箱和多档奖励中不能区分的成员保留未知。',
                        '只建立不可探测的正例；没有证据不能推导可探测。',
                        '数据基于历史攻略，未对当前游戏版本进行探测仪逐点实测。'],
        'coverageConcerns': concerns, 'incidentalObservations': incidental,
        'resolvedCoverageObservations': resolved,
        'parts': parts, 'targets': targets,
        'pointLabels': [{'stateId':s,'pointId':p,'detectability':NONDETECTABLE,'sourceTargetKeys':refs}
                        for (s,p),refs in sorted(labels.items())],
        'supplementalReviewFiles': [f for f in source_files if 'review' in f]}
    save(HERE/'collection-results.json',result)
    save(HERE/'collection-inventory.json', [{k:x[k] for k in ['bvid','title','author','pages']} for x in inventory])
    save(HERE/'non-detectable-point-ids.json', {'schemaVersion':1,'experimentOnly':True,'runtimeIntegration':False,
         'unlistedPointDefault':'unknown','source':'collection-results.json','points':result['pointLabels']})
    payload = json.dumps(result,ensure_ascii=False).replace('<','\\u003c').replace('\u2028','\\u2028').replace('\u2029','\\u2029')
    template = (HERE/'review-template.html').read_text('utf-8')
    (OUT/'review.html').write_text(template.replace('__DATA__',payload),encoding='utf-8')
    print(json.dumps(result['summary'],ensure_ascii=False))


def apply_supplemental_reviews(targets, source_files):
    """Explicit patches only: no automated promotion from nearest candidates."""
    path = HERE/'collection-review-amendments.json'
    if not path.exists():
        return
    amendments = read(path)
    source_files.append(path.name)
    for patch in amendments.get('patches', []):
        target = next(t for t in targets if t['targetKey']==patch['targetKey'])
        target.update(patch['changes'])
    for record in amendments.get('additionalTargets', []):
        targets.append(normalize(record,path.name,len(targets)))


if __name__ == '__main__':
    main()
