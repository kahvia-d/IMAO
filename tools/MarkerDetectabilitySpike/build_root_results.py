"""Materialize manually reviewed annotations; never decides identity from distance."""
import json
from pathlib import Path
from collection_utils import ROOT, OUT, points

HERE = Path(__file__).resolve().parent


def rel(path):
    return Path(path).relative_to(ROOT).as_posix()


def main():
    annotations = json.loads((HERE / 'root-review-notes.json').read_text('utf-8'))
    lookup = {str(p['id']): p for p in points([8])}
    result = {
        'schemaVersion': 1, 'date': '2026-10-29', 'experimentOnly': True,
        'runtimeIntegration': False, 'directProbeComparisonObserved': False,
        'definition': '正常探测仪查漏会漏掉，包括触发前不显示、触发后出现或可探测；未列入的点仍为未知。',
        'automationBoundary': 'SIFT配准总览只生成候选。root-review-notes.json记录代理实际视觉核对；未确认的点没有pointId。',
        'limitations': ['全片按15秒抽帧查看，疑点补帧；非逐帧连续播放，不保证捕获每一个跳剪。',
                        '总览pin与跟跑时间是两种证据，frameSeconds是复核帧而非精确章节起点。',
                        '没有在当前游戏中重新测试探测仪；属性依据作者视频与库街区机制说明。'],
        'reviewManifest': [], 'targets': []}
    for part in annotations['parts']:
        bv, p = part['bvid'], part['part']
        folder = OUT / bv / f'p{p:02d}'
        meta = json.loads((folder / 'media.json').read_text('utf-8'))
        selected = json.loads((folder / 'selected-candidates.json').read_text('utf-8'))
        contacts = sorted(folder.glob('contact-15s-*.jpg'))
        if bv == 'BV1p9K1zvE5E':
            contacts += sorted(folder.glob('contact-5s-*.jpg'))
        result['reviewManifest'].append({
            'bvid': bv, 'part': p, 'durationSeconds': meta['durationSeconds'],
            'mediaSha256': meta['sha256'], 'reviewedContactSheets': list(map(rel, contacts)),
            'reviewedGuideSheets': list(map(rel, sorted(folder.glob('guide-review-*.jpg')))),
            'reviewMethod': '完整15秒抽帧时间轴、总览及攻略图接触表逐图核对，局部补看原图。'})
        for note in part['notes']:
            pin, second, confirmed, basis, *override = note
            entry = selected[pin]
            candidates = entry['selectedCandidates']
            ids = [str(x['id']) for x in candidates[:3]] if bv == 'BV1p9K1zvE5E' and pin in [2, 16] else [override[0] if override else str(candidates[0]['id'])]
            for member, pid in enumerate(ids):
                point = lookup[pid]
                guide_json = folder / 'guides' / f'guide-8-{pid}.json'
                guide_data = json.loads(guide_json.read_text('utf-8'))['data']
                images = sorted((folder / 'guides').glob(f'guide-8-{pid}-*.png'))[:2]
                assert images, f'No reviewed guide images: {pid}'
                frame = folder / f'frame-{second:07.2f}.jpg'
                assert frame.exists(), frame
                record = {
                    'targetKey': f'{bv}:p{p}:overview-{pin}' + (f':member-{member+1}' if len(ids)>1 else ''),
                    'sourceLabel': f'总览标记 {pin+1}' + (f' / 三箱组成员{member+1}' if len(ids)>1 else ''),
                    'bvid': bv, 'part': p,
                    'source': {'bvid': bv, 'part': p, 'overviewSeconds': 0,
                               'overviewPinPixels': entry['pin'], 'reviewFrameSeconds': second,
                               'url': f'https://www.bilibili.com/video/{bv}/?p={p}&t={second}'},
                    'reviewStatus': 'confirmed-by-agent-cross-check' if confirmed else 'unresolved',
                    'detectability': 'not-detectable-for-normal-sweep' if confirmed else 'unknown',
                    'basis': basis, 'unresolvedReason': None if confirmed else basis,
                    'mechanism': point['description'] if confirmed else '候选攻略：'+point['description'],
                    'pointId': pid if confirmed else None, 'stateId': 8 if confirmed else None,
                    'typeId': point['typeId'] if confirmed else None, 'name': point['name'] if confirmed else None,
                    'rawPosition': [point['x'], point['y']] if confirmed else None,
                    'candidates': [{'stateId': 8, 'pointId': str(c['id']), 'typeId': c['typeId'],
                                    'name': c['name'], 'description': c['description'],
                                    'distance': c['distance']} for c in candidates],
                    'guide': {'description': guide_data.get('content', {}).get('content', point['description']),
                              'pictureUrls': guide_data.get('content', {}).get('picturesUrl', []),
                              'url': f'https://www.kurobbs.com/mc/map/?state=8&country={point["countryId"]}&typeId={point["typeId"]}&pointId={pid}'},
                    'localEvidence': {'video': rel(folder/'video.mp4'),
                                      'frames': list(dict.fromkeys([rel(folder/'frame-0000.00.jpg'), rel(frame)])),
                                      'guideJson': rel(guide_json), 'guideImagesReviewed': list(map(rel, images)),
                                      'candidateGeneration': rel(folder/'selected-candidates.json')},
                    'directProbeComparisonObserved': False}
                if len(ids)>1:
                    record['groupKey'] = f'{bv}:p{p}:overview-{pin}'
                    record['groupNote'] = '三档奖励组成员，编号不等同视频开箱顺序。'
                result['targets'].append(record)
    from apply_root_reviews import apply_reviews
    apply_reviews(result, lookup)
    (HERE / 'root-results.json').write_text(json.dumps(result, ensure_ascii=False, indent=2)+'\n', encoding='utf-8')
    print(json.dumps({'targets': len(result['targets']), 'confirmed': sum(x['pointId'] is not None for x in result['targets'])}))


if __name__ == '__main__':
    main()
