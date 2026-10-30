"""Offline delivery: no runtime mutations; all first-pass reviews are mandatory."""
import argparse
import json
import re
from collections import Counter
from pathlib import Path
from urllib.parse import urlsplit
from contract import can_start_overview, export_positives
from group_identity import covered_members, group_positives


def audit_references(root, timelines, point_keys):
    errors=[]
    for t in timelines:
        for e in t['events']:
            for group in e.get('identityGroups', []):
                for point in group['points']:
                    if (point['stateId'], point['pointId']) not in point_keys:
                        errors.append(f"{group['groupId']}: unknown group point identity")
                for field in ('videoEvidence', 'positionEvidence', 'guideEvidence'):
                    for proof in group[field]:
                        path=(root/proof['locator'].split('#',1)[0]).resolve()
                        if not path.is_relative_to(root.resolve()) or not path.is_file():
                            errors.append(f"{group['groupId']}: missing group evidence {proof['locator']}")
            for r in e.get('rewards',[]):
                label=f"{e['eventId']}:r{r['member']}"
                if r.get('identityStatus')=='confirmed' and (r.get('stateId'),r.get('pointId')) not in point_keys:
                    errors.append(f'{label}: unknown point identity')
                m=r.get('matchingReview',{})
                evidence=[v for field in ('positionEvidence','videoEvidence','guideEvidence','exclusionEvidence') for v in m.get(field,[])]+m.get('attributeReview',{}).get('evidence',[])
                for v in evidence:
                    references=[v.get('locator',''),v.get('source','')]
                    if any(urlsplit(x).scheme in ('http','https') for x in references):continue
                    explicit=[x.split('#',1)[0] for x in references if re.search(r'\.(?:json|txt|jpg|jpeg|png|webp|mp4|m4a|html)(?:#.*)?$',x,re.I)]
                    bad_explicit=[x for x in explicit if not (root/x).resolve().is_relative_to(root.resolve()) or not (root/x).is_file()]
                    if bad_explicit:
                        errors.extend(f'{label}: missing evidence {x}' for x in bad_explicit)
                        continue
                    local_paths=[]
                    for ref in references:
                        local=ref.split('#',1)[0]
                        path=(root/local).resolve()
                        if path.is_relative_to(root.resolve()) and path.is_file():local_paths.append(path)
                    if local_paths:continue
                    # Some review batches use a source-part identity plus an event
                    # locator; those resolve through this event's source frames.
                    if v.get('source')==f"{t['bvid']}:p{t['part']}" and e.get('evidenceFrames'):
                        bad=[x for x in e['evidenceFrames'] if not (root/x).is_file()]
                        if not bad:continue
                    errors.append(f"{label}: missing evidence {v.get('locator','')}")

    return sorted(set(errors))


def categories(event,reward):
    review=reward.get('matchingReview',{});result=[]
    text=' '.join(str(x) for x in [review.get('analysis',''),*review.get('missingEvidence',[]),*event.get('uncertainties',[])])
    if reward.get('identityStatus')!='confirmed':
        if any(x in text for x in ('分段','边界','数量','未展示','未重演','narrated')):result.append('segmentation-unclear')
        if any(x in text for x in ('定位','SIFT','坐标','地图','layer','registration','location')):result.append('localization-failed-or-insufficient')
        if any(x in text for x in ('冲突','不符','不一致','排除','多个','两箱','多箱','different','conflict')):result.append('candidate-conflict')
        if not review.get('guideEvidence') or any(x in text for x in ('取证','下载失败','攻略缺','missing guide')):result.append('evidence-missing')
        if not result:result.append('identity-unresolved')
    if reward.get('detectability')=='unknown':result.append('attribute-unclear')
    return result


def summarize(manifest_keys,timelines):
    if not can_start_overview(manifest_keys,timelines):raise ValueError('All source timelines and reasoned first-pass rewards must pass before delivery.')
    confirmed={}
    for t in timelines:
        for e in t['events']:
            for r in e.get('rewards',[]):
                if r.get('identityStatus')=='confirmed':
                    confirmed.setdefault((r['stateId'],r['pointId']),[]).append(dict(bvid=t['bvid'],part=t['part'],eventId=e['eventId'],member=r['member'],detectability=r.get('detectability')))
    for row in group_positives(timelines):
        confirmed.setdefault((row['stateId'],row['pointId']),[]).append(row)
    conflicts=[dict(stateId=k[0],pointId=k[1],sources=v,reason='Confirmed sources disagree on detector behavior; positive export withheld.') for k,v in confirmed.items() if {'detectable','undetectable'}<={x['detectability'] for x in v}]
    conflict_keys={(x['stateId'],x['pointId']) for x in conflicts}
    unique={}
    for r in export_positives(timelines):
        key=(r['stateId'],r['pointId'])
        if key in conflict_keys:continue
        entry=unique.setdefault(key,dict(stateId=key[0],pointId=key[1],detectability='undetectable',sources=[]))
        source=dict(bvid=r['bvid'],part=r['part'],eventId=r['eventId'],member=r['member'])
        if r.get('groupId'):source.update(groupId=r['groupId'],members=r['members'],identityBasis='confirmed-set')
        entry['sources'].append(source)
    unresolved=[];counts=Counter();event_count=0
    for t in timelines:
        for e in t['events']:
            event_count+=1
            for r in e.get('rewards',[]):
                in_group=r['member'] in covered_members(e)
                counts['rewards']+=1;counts['group-confirmed' if in_group else r.get('identityStatus','pending')]+=1
                if (r.get('identityStatus')!='confirmed' and not in_group) or r.get('detectability')=='unknown':
                    unresolved.append(dict(bvid=t['bvid'],part=t['part'],eventId=e['eventId'],member=r['member'],categories=categories(e,r),identityStatus=r.get('identityStatus'),detectability=r.get('detectability'),analysis=r.get('matchingReview',{}).get('analysis'),missingEvidence=r.get('matchingReview',{}).get('missingEvidence',[])))
    return dict(schemaVersion=2,scope='offline-candidate-labels-no-runtime-integration',parts=len(timelines),events=event_count,counts=dict(counts),identityConflicts=conflicts,positives=list(unique.values()),unresolved=unresolved,limitations=['Unlisted points remain unknown.','Author statements plus visual mechanisms are not independent in-game detector tests.','Local ASR and visual checks only; no direct listening claim.'])


def validate_overview(manifest_keys, overview):
    errors=[];parts=overview.get('parts',[])
    keys=[(p.get('bvid'),p.get('part')) for p in parts]
    if overview.get('status')!='reviewed' or len(keys)!=len(set(keys)) or set(keys)!=set(manifest_keys):errors.append('Overview must account for all parts exactly once.')
    for p in parts:
        if p.get('status')!='reviewed' or not p.get('analysis') or not p.get('evidence'):errors.append(f"{p.get('bvid')}:p{p.get('part')}: reasoned overview review with evidence required")
        if p.get('overviewPresence') not in ('present','not-found','uncertain'):errors.append('Overview presence assessment required.')
    return errors


def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--root',type=Path,default=Path(__file__).resolve().parents[2]);args=parser.parse_args();root=args.root
    manifest=json.loads((root/'out/detectability-v2/manifest.json').read_text('utf-8'));ts=[json.loads(p.read_text('utf-8')) for p in sorted((root/'tools/MarkerDetectabilityV2/timelines').glob('*.json'))]
    keys=[(m['bvid'],m['part']) for m in manifest];result=summarize(keys,ts);point_keys=set()
    for path in (root/'Assets/KuroMap/states').glob('state-*.json'):
        state=path.stem.removeprefix('state-')
        for group in json.loads(path.read_text('utf-8-sig')):
            point_keys.update((state,str(p['id'])) for p in group.get('location',[]))
    errors=audit_references(root,ts,point_keys)
    if errors:raise ValueError('\n'.join(errors))
    overview_path=root/'tools/MarkerDetectabilityV2/overview-review.json'
    if not overview_path.exists():raise ValueError('Overview supplementation review is still missing; no final delivery exported.')
    overview=json.loads(overview_path.read_text('utf-8'))
    overview_errors=validate_overview(keys,overview)
    if overview_errors:raise ValueError('; '.join(overview_errors))
    for p in overview['parts']:
        for value in p['evidence']:
            proof=(root/value.split('#',1)[0]).resolve()
            if not proof.is_relative_to(root.resolve()) or not proof.is_file():raise ValueError(f'Missing overview review evidence: {value}')
    output=root/'tools/MarkerDetectabilityV2/results';output.mkdir(exist_ok=True)
    for name,data in [('summary',result),('positives',result['positives']),('unresolved',result['unresolved']),('matching',ts)]:
        (output/f'{name}.json').write_text(json.dumps(data,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    print(json.dumps(dict(parts=result['parts'],events=result['events'],counts=result['counts'],uniquePositives=len(result['positives']),unresolved=len(result['unresolved'])),ensure_ascii=False))

if __name__=='__main__':main()
