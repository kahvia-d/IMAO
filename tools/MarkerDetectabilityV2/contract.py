"""Pure stage gates; validation checks evidence structure, not factual truth.

Minimal first-pass schema: reward.firstPassStatus = reviewed-unresolved and
reward.matchingReview.analysis = a nonblank reasoned assessment. Confirmed rewards
also require string stateId/pointId and three independent support arrays:
positionEvidence, videoEvidence, guideEvidence. Each evidence item has nonblank
source, locator, reason and role (support/exclusion). Video evidence source is
exactly BV:pN and includes bounded start/end. exclusionEvidence is separate.
attributeReview has status=reviewed, analysis, detectability and support evidence;
identity confirmation never implies a detectability conclusion.
"""
import copy
import math


def _text(value):
    return isinstance(value, str) and bool(value.strip())


def _number(value):
    return type(value) in (int, float) and math.isfinite(value)


def _time(value, duration):
    return _number(value) and 0 <= value <= duration


def validate_timeline(t):
    errors = []
    duration = t.get('duration')
    if not _number(duration) or duration <= 0:
        return ['invalid media duration']
    if not _text(t.get('bvid')) or type(t.get('part')) is not int or t['part'] < 1:
        errors.append('invalid source identity')
    events = t.get('events', [])
    starts = [e.get('start') for e in events]
    if all(_number(s) for s in starts) and starts != sorted(starts):
        errors.append('event order differs from video order')
    ordinals = [e.get('ordinal') for e in events]
    if any(type(v) is not int for v in ordinals) or ordinals != list(range(1, len(events) + 1)):
        errors.append('invalid ordinal order')
    ids = [e.get('eventId') for e in events]
    if len(set(str(v) for v in ids)) != len(ids):
        errors.append('duplicate event ID')
    intervals = []
    for ordinal, e in enumerate(events, 1):
        expected = f"{t.get('bvid')}:p{t.get('part')}:e{ordinal:03d}"
        if e.get('eventId') != expected:
            errors.append(f'event ID must encode source and ordinal: {expected}')
        a, b = e.get('start'), e.get('end')
        if not (_time(a, duration) and _time(b, duration) and a <= b):
            errors.append('event outside media')
        else:
            intervals.append((a, b))
        rewards = e.get('rewards', [])
        members = [r.get('member') for r in rewards]
        if any(type(v) is not int for v in members) or members != list(range(1, len(rewards) + 1)):
            errors.append('reward members must be continuous, unique and ordered')
        for r in rewards:
            for phase in ('triggerSeconds', 'appearanceSeconds', 'collectionSeconds'):
                value = r.get(phase)
                if value is not None and not _time(value, duration):
                    errors.append(f'{expected} member {r.get("member")}: invalid {phase}')
            appearance, collection = r.get('appearanceSeconds'), r.get('collectionSeconds')
            if _number(appearance) and _number(collection) and collection < appearance:
                errors.append(f'{expected}: collection before appearance')
            # Pre-existing boxes may be visible long before activation/unlocking.
    for interval in t.get('nonEventIntervals', []):
        if not isinstance(interval, (list, tuple)) or len(interval) != 3:
            errors.append('invalid non-event interval')
            continue
        a, b, reason = interval
        if not (_text(reason) and _time(a, duration) and _time(b, duration) and a <= b):
            errors.append('invalid non-event interval')
        else:
            intervals.append((a, b))
    end = 0
    for a, b in sorted(intervals):
        if a > end + 0.05:
            errors.append(f'coverage gap {end}-{a}')
        end = max(end, b)
    if end < duration - 0.05:
        errors.append(f'coverage gap {end}-{duration}')
    return errors


def can_start_matching(manifest, timelines):
    keys = [(t['bvid'], t['part']) for t in timelines]
    return bool(manifest) and (len(keys) == len(set(keys)) and set(keys) == set(manifest) and all(
        t.get('reviewStatus') == 'reviewed' and t.get('audioReviewStatus') == 'reviewed'
        and t.get('progressBarReviewStatus') == 'reviewed' and not validate_timeline(t) for t in timelines))


def _evidence(items, role, label, errors, required=True):
    if not isinstance(items, list) or (required and not items):
        errors.append(f'{label}: evidence list required')
        return set()
    keys = set()
    for item in items:
        if not isinstance(item, dict) or any(not _text(item.get(k)) for k in ('source', 'locator', 'reason')):
            errors.append(f'{label}: source, locator and reason required')
            continue
        if item.get('role') != role:
            errors.append(f'{label}: {role} evidence required; exclusion must not support confirmation')
        keys.add((item['source'].strip(), item['locator'].strip()))
    return keys


def validate_matching(timelines):
    """Return errors without matching anything. Pending rewards are allowed.

    A first-pass status is a claim, so unresolved needs analysis and confirmed
    must meet every evidence requirement. No dataset or candidate files are read.
    """
    errors = []
    from group_identity import validate_groups
    for t in timelines:
        errors.extend(validate_timeline(t))
        for e in t.get('events', []):
            errors.extend(validate_groups(e))
            for r in e.get('rewards', []):
                label = f"{e.get('eventId')} member {r.get('member')}"
                status = r.get('firstPassStatus')
                confirmed = r.get('identityStatus') == 'confirmed'
                if status not in (None, 'pending', 'reviewed-unresolved', 'reviewed-confirmed'):
                    errors.append(f'{label}: invalid first-pass status')
                if status == 'reviewed-confirmed' and not confirmed:
                    errors.append(f'{label}: first-pass confirmation lacks confirmed identity')
                if confirmed and status != 'reviewed-confirmed':
                    errors.append(f'{label}: confirmed identity requires reviewed-confirmed status')
                review = r.get('matchingReview')
                if confirmed or status in ('reviewed-unresolved', 'reviewed-confirmed'):
                    if not isinstance(review, dict) or not _text(review.get('analysis')):
                        errors.append(f'{label}: reasoned matchingReview.analysis required')
                if not confirmed:
                    continue
                for field in ('stateId', 'pointId'):
                    if not _text(r.get(field)):
                        errors.append(f'{label}: nonblank string {field} required')
                if not isinstance(review, dict):
                    review = {}
                supports = set()
                for field in ('positionEvidence', 'videoEvidence', 'guideEvidence'):
                    supports |= _evidence(review.get(field), 'support', f'{label}.{field}', errors)
                videos = review.get('videoEvidence')
                if isinstance(videos, list):
                    for video in videos:
                        if not isinstance(video, dict):
                            continue
                        if video.get('source') != f"{t['bvid']}:p{t['part']}":
                            errors.append(f'{label}: video evidence must identify the source part')
                        a, b = video.get('start'), video.get('end')
                        if not (_time(a, t['duration']) and _time(b, t['duration']) and a <= b):
                            errors.append(f'{label}: invalid source video evidence time')
                excluded = _evidence(review.get('exclusionEvidence', []), 'exclusion',
                                     f'{label}.exclusionEvidence', errors, required=False)
                attribute = review.get('attributeReview')
                if not isinstance(attribute, dict):
                    errors.append(f'{label}: independent attribute review required')
                else:
                    if attribute.get('status') != 'reviewed' or not _text(attribute.get('analysis')):
                        errors.append(f'{label}: completed reasoned attribute review required')
                    decision = attribute.get('detectability')
                    if decision not in ('unknown', 'detectable', 'undetectable') or decision != r.get('detectability'):
                        errors.append(f'{label}: attribute conclusion must agree with detectability')
                    supports |= _evidence(attribute.get('evidence'), 'support', f'{label}.attributeReview', errors)
                if supports & excluded:
                    errors.append(f'{label}: excluded evidence reused as support')
    return errors


def can_start_overview(manifest, timelines):
    return (can_start_matching(manifest, timelines) and not validate_matching(timelines) and all(
        r.get('firstPassStatus') in ('reviewed-confirmed', 'reviewed-unresolved')
        for t in timelines for e in t['events'] for r in e.get('rewards', [])))


def export_positives(timelines):
    """Return detached positive records, never write files or classify unknowns.

    Invalid confirmation claims abort export instead of silently disappearing.
    Caller must separately enforce full-manifest stage gates before publication.
    """
    errors = validate_matching(timelines)
    if errors:
        raise ValueError('; '.join(errors))
    positives = []
    for t in timelines:
        for e in t.get('events', []):
            for r in e.get('rewards', []):
                if r.get('identityStatus') == 'confirmed' and r.get('detectability') == 'undetectable':
                    positives.append(dict(copy.deepcopy(r), bvid=t['bvid'], part=t['part'], eventId=e['eventId']))
    from group_identity import group_positives
    positives.extend(group_positives(timelines))
    return positives
