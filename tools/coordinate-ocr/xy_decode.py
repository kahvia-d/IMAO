"""Own character-box decoder for the fixed, single-row offline coordinate ROI.

X/Y need two observed comma delimiters. Z is optional. Conflicts outside the
mandatory prefix cannot veto X/Y. No sign repair, temporal state or truth input.
"""
import math
import re
import statistics


def _iou(a, b):
    area = max(0, min(a[2], b[2]) - max(a[0], b[0])) * max(0, min(a[3], b[3]) - max(a[1], b[1]))
    union = (a[2] - a[0]) * (a[3] - a[1]) + (b[2] - b[0]) * (b[3] - b[1]) - area
    return area / union if union > 0 else 0


def _center(item):
    return (item['xyxy'][0] + item['xyxy'][2]) / 2


def _integer(text):
    if not re.fullmatch(r'[+-]?\d+', text):
        return None
    value = int(text)
    return value if -2147483648 <= value <= 2147483647 else None


def _conflicts(items):
    return any(a['char'] != b['char'] and _iou(a['xyxy'], b['xyxy']) > .45
               for i, a in enumerate(items) for b in items[i + 1:])


def _continuous(items, digit_width):
    return all(_center(b) - _center(a) <= 1.8 * digit_width for a, b in zip(items, items[1:]))


def decode(detections):
    def reject(reason, chunks=None):
        return {'selected': None, 'reason': reason, 'chunks': chunks or []}

    accepted = []
    for item in detections:
        if not math.isfinite(item['score']):
            return reject('invalid-score')
        if item['score'] < .25:
            continue
        box = item['xyxy']
        if len(box) != 4 or not all(math.isfinite(v) for v in box) or box[2] <= box[0] or box[3] <= box[1]:
            return reject('invalid-box')
        if len(item['char']) != 1 or item['char'] not in '0123456789-,+:':
            return reject('invalid-character')
        accepted.append(item)

    unique = []
    for item in sorted(accepted, key=lambda c: -c['score']):
        if not any(item['char'] == old['char'] and _iou(item['xyxy'], old['xyxy']) > .45 for old in unique):
            unique.append(item)
    unique.sort(key=_center)
    widths = [c['xyxy'][2] - c['xyxy'][0] for c in unique if c['char'].isdigit()]
    if not widths:
        return reject('no-digits')
    digit_width = statistics.median(widths)
    threshold = max(12., .75 * digit_width)
    groups = []
    right = None
    for item in unique:
        if right is None or item['xyxy'][0] - right > threshold:
            groups.append([])
            right = item['xyxy'][2]
        else:
            right = max(right, item['xyxy'][2])
        groups[-1].append(item)

    chunks = []
    candidates = []
    unsafe_coordinate = False
    for index, group in enumerate(groups):
        text = ''.join(c['char'] for c in group)
        chunk = {'text': text, 'reason': 'no-delimited-xy'}
        chunks.append(chunk)
        commas = [i for i, c in enumerate(group) if c['char'] == ',']
        if len(commas) < 2:
            continue
        end = commas[1] + 1
        prefix = group[:end]
        # Check ambiguity before syntax: conflicting signs may make the text
        # unparseable, but must not allow selecting a different coordinate group.
        if any(a['char'] != b['char'] and _iou(a['xyxy'], b['xyxy']) > .45
               for a in prefix for b in group if a is not b):
            chunk['reason'] = 'conflicting-xy'
            unsafe_coordinate = True
            continue
        prefix_text = ''.join(c['char'] for c in prefix)
        match = re.fullmatch(r'([+-]?\d+),([+-]?\d+),', prefix_text)
        if not match:
            continue
        # A comma alternative in an ambiguous Z slot is optional context.
        # An additional standalone comma still makes the coordinate ambiguous.
        if any(not any(other['char'] != ',' and _iou(extra['xyxy'], other['xyxy']) > .45
                       for other in group[end:]) for extra in (group[i] for i in commas[2:])):
            chunk['reason'] = 'ambiguous-extra-delimiter'
            unsafe_coordinate = True
            continue
        if not _continuous(prefix, digit_width):
            chunk['reason'] = 'discontinuous-xy'
            unsafe_coordinate = True
            continue
        if index:
            previous = groups[index - 1]
            previous_text = ''.join(c['char'] for c in previous)
            gap = group[0]['xyxy'][0] - max(c['xyxy'][2] for c in previous)
            if re.fullmatch(r'[+-]?\d+|[+-]', previous_text) and gap <= 2 * digit_width:
                chunk['reason'] = 'nearby-leading-number-fragment'
                unsafe_coordinate = True
                continue
        xy = [_integer(t) for t in match.groups()]
        if None in xy:
            chunk['reason'] = 'xy-overflow'
            continue

        suffix = group[end:]
        z = None
        if suffix and not _conflicts(suffix) and _continuous(suffix, digit_width):
            z = _integer(''.join(c['char'] for c in suffix))
            if index + 1 < len(groups):
                following = groups[index + 1]
                following_text = ''.join(c['char'] for c in following)
                gap = following[0]['xyxy'][0] - max(c['xyxy'][2] for c in group)
                if re.fullmatch(r'[+-]?\d+', following_text) and gap <= 2 * digit_width:
                    z = None
        chunk['reason'] = 'observed-delimited-xy'
        candidates.append({'xy': xy, 'z': z, 'xyScore': min(c['score'] for c in prefix),
                           'zScore': min(c['score'] for c in suffix) if z is not None else None,
                           'textXY': prefix_text[:-1], 'observedGroupText': text})

    if unsafe_coordinate:
        return reject('ambiguous-coordinate-group', chunks)
    if len(candidates) != 1:
        return reject('no-unique-delimited-xy', chunks)
    return {'selected': candidates[0], 'reason': 'observed-delimited-xy', 'chunks': chunks}
