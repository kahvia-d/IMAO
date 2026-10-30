"""Apply explicit image-review findings, never promote by candidate distance."""
import json
from pathlib import Path

HERE = Path(__file__).resolve().parent
BV = 'BV1iMTXzVE8J'


def read(name):
    return json.loads((HERE/name).read_text('utf-8'))


def apply_reviews(result, lookup):
    def patch(part, pin, confirmed, pid, basis, frames, guide_json, images, seconds=None):
        r = next(r for r in result['targets'] if r['targetKey']==f'{BV}:p{part}:overview-{pin}')
        p = lookup[pid]
        r.update(reviewStatus='confirmed-by-agent-cross-check' if confirmed else 'unresolved',
                 detectability='not-detectable-for-normal-sweep' if confirmed else 'unknown',
                 pointId=pid if confirmed else None, stateId=8 if confirmed else None,
                 typeId=p['typeId'] if confirmed else None, name=p['name'] if confirmed else None,
                 rawPosition=[p['x'],p['y']] if confirmed else None,
                 basis=basis, unresolvedReason=None if confirmed else basis,
                 mechanism=p['description'] if confirmed else '候选攻略：'+p['description'])
        r['localEvidence'].update(frames=frames, guideJson=guide_json, guideImagesReviewed=images)
        if seconds:
            r['source'].update(reviewFrameSeconds=seconds[0], segmentSecondsApprox=[min(seconds),max(seconds)],
                               url=f'https://www.bilibili.com/video/{BV}/?p={part}&t={seconds[0]}')
        else:
            r['source']['reviewFrameSeconds'] = 0
            r['source']['url'] = f'https://www.bilibili.com/video/{BV}/?p={part}'

    extra = read('extra-review.json')
    for r in extra['p08PinReview']+extra['p09PinReview']:
        g=r['guide']
        patch(r['part'],r['pinIndexZeroBased'],r['reviewStatus'].startswith('confirmed'),
              r['candidatePointId'],r['basis'],r['localEvidence'],g['localJson'],g['reviewedImages'],r['frameSeconds'])
    for r in read('p10-quality-review.json')['reviews']:
        e=r['localEvidence']
        patch(10,r['pin'],r['recommendation'].startswith(('retain-confirmed','upgrade-to-confirmed')),
              r['pointId'],r['basis'],e['videoFrames'],e['guideJson'],e['guideImagesReviewed'],r['segmentSecondsApprox'])
    # Explicit unbound segment; the mushroom segment remains represented by pin3.
    r=extra['unresolvedVideoSegments'][1]
    result['targets'].append(dict(r, bvid=BV,targetKey=f'{BV}:p9:unbound-rail-challenge',
        sourceLabel='断轨列车竞速 / 未绑定',localEvidence={'frames':r['localEvidence']},
        source={'bvid':BV,'part':9,'segmentSecondsApprox':r['timeRangeSeconds']},basis=r['observedMechanism']))
    result['coverageConcerns']=[read('p10-quality-review.json')['coverageConcern']]
    result['incidentalObservations']=extra['incidentalObservations']
    result['supplementalReviewFiles']=['extra-review.json','p10-quality-review.json']
    firm = read('firmament-quality-review.json')
    for review in firm['recommendations']:
        prefix=f'BV1p9K1zvE5E:p1:overview-{review["pinIndex"]}'
        for r in result['targets']:
            if r['targetKey']!=prefix and not r['targetKey'].startswith(prefix+':member-'):
                continue
            confirmed=review['action'] in ['retain_confirmed','promote_confirmed']
            pid=Path(r['localEvidence']['guideJson']).stem.split('-')[-1]
            p=lookup[pid]
            if confirmed:
                assert review['pointIdsByType'][p['typeId']]==pid
            r.update(reviewStatus='confirmed-by-agent-cross-check' if confirmed else 'unresolved',
                     detectability='not-detectable-for-normal-sweep' if confirmed else 'unknown',
                     pointId=pid if confirmed else None,stateId=8 if confirmed else None,
                     typeId=p['typeId'] if confirmed else None,name=p['name'] if confirmed else None,
                     rawPosition=[p['x'],p['y']] if confirmed else None,
                     basis=review['reason'],unresolvedReason=None if confirmed else review['reason'],
                     mechanism=review.get('mechanism','候选攻略：'+p['description']))
            r['localEvidence']['frames']=[f for f in review['evidence'] if '/guides/' not in f]
            reviewed=[f for f in review['evidence'] if '/guides/' in f and not f.endswith('.json')]
            r['localEvidence']['guideImagesReviewed']=list(dict.fromkeys(reviewed+r['localEvidence']['guideImagesReviewed']))
            interval=review.get('timeRangeSeconds')
            r['source']['reviewFrameSeconds']=interval[0] if interval else 0
            r['source']['url']=f'https://www.bilibili.com/video/BV1p9K1zvE5E/?t={interval[0] if interval else 0}'
            if interval:
                r['source']['segmentSecondsApprox']=interval
            r['supplementalReviewFile']='firmament-quality-review.json'
    result['coverageConcerns']+= [dict(r,bvid='BV1p9K1zvE5E',part=1) for r in firm['coverageObservations'] if r['status'].startswith('unbound')]
    result['incidentalObservations']+= [r for r in firm['coverageObservations'] if r['status'].startswith('incidental')]
    result['supplementalReviewFiles'].append('firmament-quality-review.json')
    # Recheck of the ending against the already selected gold-chest guide image 2.
    gold=next(r for r in result['targets'] if r['targetKey']=='BV1p9K1zvE5E:p1:overview-13')
    gold['basis']='总览配准命中辉光箱候选；三个机关及踏光兽机制对应。639秒视频与攻略第2图的水岸、右侧平石、远处木障碍、树丛和前景圆形石纹一致，片尾金箱属于此标记14，不属于标记16精密箱。'
    gold['source'].update(reviewFrameSeconds=639,segmentSecondsApprox=[568,645],url='https://www.bilibili.com/video/BV1p9K1zvE5E/?p=1&t=639')
    gold['localEvidence']['frames']+=['out/detectability-collection/BV1p9K1zvE5E/p01/quality-review/frame-0639.jpg','out/detectability-collection/BV1p9K1zvE5E/p01/quality-review/frame-0642.jpg']
    unknown=next(r for r in result['targets'] if r['targetKey']=='BV1p9K1zvE5E:p1:overview-15')
    unknown['sourceLabel']='总览候选位置 16（内部编号，非视频收集序号）'
    unknown['basis']=unknown['unresolvedReason']='仅总览位置生成了精密箱候选1260175017361457152，尚未绑定它的跟跑片段。曾误挂的639–642秒辉光箱已确认属于总览标记14（1253810339575873536），已从本条匹配证据移除。精密箱候选来自总览配准，绝非由该辉光箱视频匹配得出。'
    unknown['localEvidence']['rejectedVideoFrames']=unknown['localEvidence']['frames']
    unknown['localEvidence']['rejectionReason']='这些画面属于已确认的标记14辉光箱，只保留作误配审计，不作为本条目标画面。'
    unknown['localEvidence']['frames']=['out/detectability-collection/BV1p9K1zvE5E/p01/frame-0000.00.jpg']
    unknown['localEvidence']['frameRole']='来源总览（尚未绑定跟跑片段）'
    result['coverageConcerns']=[r for r in result['coverageConcerns'] if not (r.get('bvid')=='BV1p9K1zvE5E' and r.get('timeRangeSeconds')==[568,645])]
    result['resolvedCoverageObservations']=[{'bvid':'BV1p9K1zvE5E','part':1,'timeRangeSeconds':[568,645],
        'targetKey':gold['targetKey'],'pointId':gold['pointId'],'basis':gold['basis'],
        'supersedes':'firmament-quality-review.json coverageObservations: unbound ending gold chest'}]
