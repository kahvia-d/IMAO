"""Compare identities only after v2 first-pass gate; never import v1 assertions."""
import json
from pathlib import Path
from collections import defaultdict
from contract import can_start_overview
from group_identity import covered_members, group_positives


def compare(old,timelines):
    new=defaultdict(list);events=defaultdict(list);unbound=[]
    for t in timelines:
        for e in t['events']:
            events[(t['bvid'],t['part'])].append(e)
            for r in e['rewards']:
                entry=dict(bvid=t['bvid'],part=t['part'],eventId=e['eventId'],member=r['member'],detectability=r.get('detectability'),analysis=r.get('matchingReview',{}).get('analysis',''))
                if r.get('identityStatus')=='confirmed':new[(r['stateId'],r['pointId'])].append(entry)
                elif r['member'] not in covered_members(e):unbound.append(entry)
    for row in group_positives(timelines):
        new[(row['stateId'],row['pointId'])].append(row)
    oldids=defaultdict(list)
    for r in old['targets']:
        if r.get('identityStatus')=='confirmed' and r.get('pointId') is not None:oldids[(str(r['stateId']),str(r['pointId']))].append(r)
    identities=[]
    for key in sorted(set(oldids)|set(new)):
        status='retained' if key in oldids and key in new else 'revoked-confirmation' if key in oldids else 'new-confirmation'
        identities.append(dict(stateId=key[0],pointId=key[1],change=status,oldTargets=[r['targetKey'] for r in oldids.get(key,[])],newSources=new.get(key,[]),reason='v2 independently re-established identity; attribute is reviewed separately.' if status=='retained' else 'v2 evidence did not re-establish this identity. Removal is from confirmed output, not proof that the point is detectable or nonexistent.' if status=='revoked-confirmation' else 'v2 independent evidence establishes an identity absent from old confirmed identities.'))
    old_records=[]
    for r in old['targets']:
        key=(str(r.get('stateId')),str(r.get('pointId')));same=new.get(key,[]);interval=r.get('segmentSecondsApprox') or r.get('source',{}).get('segmentSecondsApprox');overlap=[]
        if isinstance(interval,list) and len(interval)==2 and all(isinstance(v,(int,float)) for v in interval):
            for e in events[(r['bvid'],int(r['part']))]:
                inter=max(0,min(interval[1],e['end'])-max(interval[0],e['start']))
                if inter>0:overlap.append(dict(eventId=e['eventId'],overlapSeconds=inter,members=len(e['rewards'])))
        old_records.append(dict(oldTargetKey=r['targetKey'],oldIdentityStatus=r.get('identityStatus'),oldStateId=str(r.get('stateId')) if r.get('stateId') is not None else None,oldPointId=str(r.get('pointId')) if r.get('pointId') is not None else None,exactIdentitySources=same,chronologicalOverlaps=overlap,change='retained-identity' if same else 'not-reconfirmed' if r.get('identityStatus')=='confirmed' else 'old-unresolved',reason='Exact identity correspondence only; temporal overlap is a review aid and never proves reassignment. Old overview numbering is not a v2 event ID.'))
    groups=[]
    for key,es in events.items():
        for e in es:
            if len(e['rewards'])>1:groups.append(dict(bvid=key[0],part=key[1],eventId=e['eventId'],change='explicit-event-with-reward-members',identityGroups=e.get('identityGroups',[]),members=[dict(member=r['member'],pointId=r.get('pointId'),identityStatus=r.get('identityStatus')) for r in e['rewards']],reason='Set-level confirmation may cover members without assigning IDs by array order.'))
    return dict(identityChanges=identities,oldTargetCoverage=old_records,multiRewardGroups=groups,unboundV2Rewards=unbound,summary=dict(oldTargets=len(old['targets']),oldConfirmedUnique=len(oldids),v2ConfirmedUnique=len(new),retained=sum(k in oldids for k in new),revoked=sum(k not in new for k in oldids),new=sum(k not in oldids for k in new)),limitations=['Reassignment/merge/split is not inferred from equal list index, overview number or overlapping time.','Old confirmation absent in v2 means withdrawn pending evidence, not a negative detector classification.'])


def main():
    root=Path(__file__).resolve().parents[2];manifest=json.loads((root/'out/detectability-v2/manifest.json').read_text('utf-8'));ts=[json.loads(p.read_text('utf-8')) for p in sorted((root/'tools/MarkerDetectabilityV2/timelines').glob('*.json'))]
    if not can_start_overview([(m['bvid'],m['part']) for m in manifest],ts):raise ValueError('No v1 comparison before independent first-pass completion.')
    old=json.loads((root/'tools/MarkerDetectabilitySpike/collection-results.json').read_text('utf-8'));result=compare(old,ts);manual=root/'tools/MarkerDetectabilityV2/manual-diff.json';result['reviewedStructuralChanges']=json.loads(manual.read_text('utf-8'))['changes'] if manual.exists() else [];out=root/'tools/MarkerDetectabilityV2/results';out.mkdir(exist_ok=True);(out/'v1-diff.json').write_text(json.dumps(result,ensure_ascii=False,indent=2),encoding='utf-8');print(result['summary'])
if __name__=='__main__':main()
