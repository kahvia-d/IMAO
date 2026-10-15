"""Confirm a missing minus in actual image pixels and a second detector route.

Never repairs digits, commas, positive signs, unknown layouts or missing XY.
Thresholds are fixed before development replay, not adjusted per failed frame.
"""
import math
import numpy as np

from xy_decode import decode, _iou, _center
from sign_layout import audit


def _valid(item):
    box = item.get('xyxy', [])
    return (len(box) == 4 and all(math.isfinite(v) for v in box)
            and box[2] > box[0] and box[3] > box[1]
            and math.isfinite(item.get('score', float('nan'))))


def _stroke(image, box):
    height, width = image.shape[:2]
    a, t, b, d = box
    if not (0 <= a < b <= width and 0 <= t < d <= height):
        return None
    patch = image[int(math.floor(t)):int(math.ceil(d)), int(math.floor(a)):int(math.ceil(b))]
    if min(patch.shape[:2]) < 3:
        return None
    gray = patch.astype(np.float32).mean(axis=2) if patch.ndim == 3 else patch.astype(np.float32)
    row_edge = float(np.abs(np.diff(gray, axis=0)).mean())
    column_edge = float(np.abs(np.diff(gray, axis=1)).mean())
    contrast = float(gray.max() - gray.min())
    if contrast < 4. or row_edge < 1. or row_edge < 1.5 * column_edge:
        return None
    # A true local bar needs two opposite edges. A monotonic slope or a single
    # background boundary may have strong horizontal gradients but no bar.
    core = gray[:, 1:-1]
    gradients = np.diff(core, axis=0)
    profile = gradients.mean(axis=1)
    top_edge, bottom_edge = int(profile.argmax()), int(profile.argmin())
    edge_floor = max(1.5, .08 * contrast)
    if profile[top_edge] < edge_floor or profile[bottom_edge] > -edge_floor:
        return None
    if abs(top_edge-bottom_edge) < 1:
        return None
    top_support = float((gradients[top_edge] >= 1.5).mean())
    bottom_support = float((gradients[bottom_edge] <= -1.5).mean())
    if min(top_support, bottom_support) < .60:
        return None
    # Both edges must end locally; a full-width scenery stripe is not enough.
    left, right = int(math.floor(a)), int(math.ceil(b))
    margin = max(2, int(round((right-left)*.2)))
    if left-margin < 0 or right+margin > width:
        return None
    sides = [image[int(math.floor(t)):int(math.ceil(d)), left-margin:left],
             image[int(math.floor(t)):int(math.ceil(d)), right:right+margin]]
    outside_strengths = []
    for outside in sides:
        outside = outside.astype(np.float32).mean(axis=2) if outside.ndim == 3 else outside.astype(np.float32)
        outside_grad = np.diff(outside, axis=0)
        outside_strengths.append(float(np.abs(outside_grad[[top_edge, bottom_edge]]).mean()))
    outside_strength = max(outside_strengths)
    inside_strength = float((abs(profile[top_edge])+abs(profile[bottom_edge]))/2)
    if inside_strength < 1.3*outside_strength or inside_strength-outside_strength < 1.:
        return None
    return {'contrastRange': contrast, 'horizontalStrokeEdge': row_edge, 'verticalStrokeEdge': column_edge,
            'pairedEdgeStrength': inside_strength, 'outsideEdgeStrength': outside_strength,
            'outsideEdgeStrengthBySide': outside_strengths,
            'pairedEdgeSupport': [top_support, bottom_support]}


def recover(detections, decoded, calibration, context, confirmation_image, confirmation_detections):
    guarded = audit(detections, decoded, calibration, context)
    result = {'selected': guarded['selected'], 'restoredAxes': [], 'reason': guarded['reason'],
              'layout': guarded, 'confirmations': []}
    if not guarded['applied'] or not guarded['suspectAxes']:
        return result
    checks = {c['axis']: c for c in guarded['checks']}
    if any(checks[a]['status'] != 'possible-missing-sign' for a in guarded['suspectAxes']):
        return {**result, 'reason': 'unexplained-layout-not-repairable'}
    expected_shape = tuple(reversed(calibration['context']['imageSize']))
    if confirmation_image is None or tuple(confirmation_image.shape[:2]) != expected_shape:
        return {**result, 'reason': 'invalid-confirmation-image'}
    evidence = [o for o in confirmation_detections if _valid(o) and o['score'] >= .25]
    features = guarded['measurements']
    digit_width = features['digitWidth']
    additions = []
    expected_xy = list(decoded['selected']['xy'])
    for axis in guarded['suspectAxes']:
        index = 'xy'.index(axis)
        if expected_xy[index] <= 0:
            return {**result, 'reason': 'nonpositive-axis-not-repairable'}
        digit = features['firstXBox' if axis == 'x' else 'firstYBox']
        h = digit[3] - digit[1]
        cx = (digit[0] + digit[2]) / 2 - .84 * digit_width
        cy = digit[1] + .63 * h
        neighbors = [o for o in evidence if _iou(o['xyxy'], digit) >= .5]
        expected_char = decoded['selected']['textXY'].split(',')[index].lstrip('+-')[0]
        if (not any(o['char'] == expected_char and o['score'] >= .5 for o in neighbors)
                or any(o['char'] != expected_char for o in neighbors)):
            return {**result, 'reason': 'neighbor-digit-not-confirmed'}
        candidates = []
        for o in evidence:
            a, t, b, d = o['xyxy']
            if o['char'] != '-' or o['score'] < .5:
                continue
            if not (.35 * digit_width <= b-a <= .95 * digit_width and .10*h <= d-t <= .40*h):
                continue
            if abs(_center(o)-cx) > .25*digit_width or abs((t+d)/2-cy) > .15*h:
                continue
            if a < digit[0]-1.05*digit_width or b > digit[0]+.10*digit_width:
                continue
            if any(p['char'] != '-' and _iou(p['xyxy'], o['xyxy']) > .45 for p in evidence):
                continue
            stroke = _stroke(confirmation_image, o['xyxy'])
            if stroke is not None:
                candidates.append((o, stroke))
        unique = []
        for candidate in sorted(candidates, key=lambda pair: -pair[0]['score']):
            if not any(_iou(candidate[0]['xyxy'], old[0]['xyxy']) > .45 for old in unique):
                unique.append(candidate)
        if len(unique) != 1:
            return {**result, 'reason': 'local-minus-not-confirmed'}
        sign, stroke = unique[0]
        additions.append({'char': '-', 'score': sign['score'], 'xyxy': list(sign['xyxy'])})
        result['confirmations'].append({'axis': axis, 'detectedMinus': sign, 'pixelStroke': stroke})
        expected_xy[index] = -expected_xy[index]
    repaired = decode(list(detections) + additions)
    checked = audit(list(detections) + additions, repaired, calibration, context)
    if checked['selected'] is None or checked['selected']['xy'] != expected_xy:
        return {**result, 'reason': 'repair-failed-full-xy-validation'}
    return {**result, 'selected': checked['selected'], 'restoredAxes': list(guarded['suspectAxes']),
            'reason': 'local-minus-confirmed', 'postRepairLayout': checked}
