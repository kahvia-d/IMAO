"""Decline-only sign-layout audit for an explicitly calibrated offline HUD.

Uses observed glyph positions, never truth, temporal state or sign repair.
Unsupported profiles return an explicit not-applied diagnostic; this is not a
general game-layout detector. The base XY decoder remains unchanged.
"""
import statistics

from xy_decode import _center, _iou


def measure(detections, decoded):
    selected = decoded['selected']
    if selected is None:
        return None
    unique = []
    for item in sorted((o for o in detections if o['score'] >= .25), key=lambda o: -o['score']):
        if not any(item['char'] == old['char'] and _iou(item['xyxy'], old['xyxy']) > .45 for old in unique):
            unique.append(item)
    unique.sort(key=_center)
    widths = [o['xyxy'][2] - o['xyxy'][0] for o in unique if o['char'].isdigit()]
    if not widths:
        return None
    width = statistics.median(widths)
    threshold = max(12., .75 * width)
    groups = []
    right = None
    for item in unique:
        if right is None or item['xyxy'][0] - right > threshold:
            groups.append([])
            right = item['xyxy'][2]
        else:
            right = max(right, item['xyxy'][2])
        groups[-1].append(item)
    matches = []
    for group in groups:
        commas = [i for i, o in enumerate(group) if o['char'] == ',']
        if len(commas) < 2:
            continue
        first, second = commas[:2]
        prefix = group[:second + 1]
        if ''.join(o['char'] for o in prefix) != selected['textXY'] + ',':
            continue
        xdigits = [o for o in group[:first] if o['char'].isdigit()]
        ydigits = [o for o in group[first + 1:second] if o['char'].isdigit()]
        if not xdigits or not ydigits:
            continue
        matches.append({'xOrigin': (_center(xdigits[0]) - 4.) / width,
                        'ySpan': (_center(ydigits[0]) - _center(xdigits[-1])) / width,
                        'digitWidth': width, 'xHasSign': group[0]['char'] in '+-',
                        'yHasSign': group[first + 1]['char'] in '+-',
                        'firstXBox': xdigits[0]['xyxy'], 'lastXBox': xdigits[-1]['xyxy'],
                        'firstYBox': ydigits[0]['xyxy']})
    return matches[0] if len(matches) == 1 else None


def _inside(value, interval):
    return interval[0] <= value <= interval[1]


def audit(detections, decoded, calibration, context):
    result = {'selected': decoded['selected'], 'applied': False, 'reason': 'base-not-returned',
              'suspectAxes': [], 'measurements': None, 'checks': []}
    if result['selected'] is None:
        return result
    if any(context.get(k) != v for k, v in calibration['context'].items()):
        result['reason'] = 'uncalibrated-profile'
        return result
    features = measure(detections, decoded)
    if features is None:
        return {**result, 'selected': None, 'reason': 'missing-candidate-geometry'}
    result['measurements'] = features
    if not _inside(features['digitWidth'], calibration['digitWidthRange']):
        result['reason'] = 'uncalibrated-font-scale'
        return result
    result['applied'] = True
    for axis, feature, unsigned, signed in [
        ('x', 'xOrigin', 'unsignedXOriginRange', 'signedXOriginRange'),
        ('y', 'ySpan', 'unsignedYSpanRange', 'signedYSpanRange'),
    ]:
        has_sign = features[axis + 'HasSign']
        value = features[feature]
        status = 'visible-sign' if has_sign else 'compatible-unsigned' if _inside(value, calibration[unsigned]) else (
            'possible-missing-sign' if _inside(value, calibration[signed]) else 'outside-calibration')
        result['checks'].append({'axis': axis, 'feature': value, 'status': status})
        if not has_sign and status != 'compatible-unsigned':
            result['suspectAxes'].append(axis)
    if result['suspectAxes']:
        result['selected'] = None
        result['reason'] = 'sign-layout-suspect'
    else:
        result['reason'] = 'layout-compatible'
    return result
