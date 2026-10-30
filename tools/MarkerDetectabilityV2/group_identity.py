"""Set-level identities: never assign a set's IDs to ordered reward members."""
import re


def covered_members(event):
    return {m for g in event.get('identityGroups', []) for m in g['members']}


def validate_groups(event):
    errors, covered = [], set()
    rewards = {r['member']: r for r in event.get('rewards', [])}
    for group in event.get('identityGroups', []):
        label = f"{event['eventId']} group {group.get('groupId')}"
        members, points = group.get('members', []), group.get('points', [])
        if group.get('status') != 'confirmed-set' or not group.get('analysis'):
            errors.append(f'{label}: reviewed set and analysis required')
        if (not members or len(set(members)) != len(members) or
                not set(members) <= rewards.keys() or covered & set(members)):
            errors.append(f'{label}: invalid or overlapping members')
        covered.update(members)
        if len(points) != len(members):
            errors.append(f'{label}: reward and identity counts differ')
        keys = [(p.get('stateId'), p.get('pointId')) for p in points]
        if any(not isinstance(v, str) or not v.strip() for k in keys for v in k) or len(set(keys)) != len(keys):
            errors.append(f'{label}: unique string identities required')
        if group.get('detectability') != 'undetectable' or any(
                rewards.get(m, {}).get('detectability') != 'undetectable' for m in members):
            errors.append(f'{label}: group and member attributes must agree')
        # A group does not silently replace an existing per-member assignment.
        if any(rewards.get(m, {}).get('identityStatus') == 'confirmed' for m in members):
            errors.append(f'{label}: individually confirmed member must not be remapped')
        for field in ('videoEvidence', 'positionEvidence', 'guideEvidence'):
            evidence = group.get(field, [])
            if not evidence or any(not all(x.get(k) for k in ('source', 'locator', 'reason')) or
                                   x.get('role') != 'support' for x in evidence):
                errors.append(f'{label}: {field} support required')
        guides = group.get('guideEvidence', [])
        for sid, pid in keys:
            pattern = re.escape(f'guide-{sid}-{pid}') + r'(?=[.\-/#?]|$)'
            if not any(re.search(pattern, x.get('locator', '')) for x in guides):
                errors.append(f'{label}: missing guide for {sid}/{pid}')
        excluded = {x['locator'] for m in members for x in
                    rewards.get(m, {}).get('matchingReview', {}).get('exclusionEvidence', [])}
        if excluded & {x.get('locator') for x in guides}:
            errors.append(f'{label}: excluded guide cannot establish a set')
    return errors


def group_positives(timelines):
    for t in timelines:
        for event in t.get('events', []):
            for group in event.get('identityGroups', []):
                for point in group['points']:
                    yield dict(point, bvid=t['bvid'], part=t['part'], eventId=event['eventId'],
                               member=None, groupId=group['groupId'], members=group['members'],
                               detectability=group['detectability'])
