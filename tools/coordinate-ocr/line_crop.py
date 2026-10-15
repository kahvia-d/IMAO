"""Rejected gap probe: missing strokes can truncate z. Diagnostic only, no labels."""
import cv2
import numpy as np


def split_line(image):
    if image.ndim != 3 or image.shape[2] != 3 or image.dtype != np.uint8 or not image.size:
        raise ValueError('split_line requires a nonempty uint8 BGR image')
    height, width = image.shape[:2]
    gray = cv2.cvtColor(image, cv2.COLOR_BGR2GRAY)
    # HUD strokes are approximately neutral, bright local features. This mask is
    # only a gap probe: no mask pixels replace the original OCR input.
    kernel = cv2.getStructuringElement(cv2.MORPH_RECT,
                                      (max(3, round(height * .35)), max(3, round(height * .22))))
    top_hat = cv2.morphologyEx(gray, cv2.MORPH_TOPHAT, kernel)
    spread = image.max(axis=2).astype(np.int16) - image.min(axis=2).astype(np.int16)
    mask = ((top_hat >= 5) & (gray >= 40) & (spread <= 60)).astype(np.uint8)
    count, _, stats, _ = cv2.connectedComponentsWithStats(mask, 8)
    boxes = sorted([tuple(map(int, row[:4])) for row in stats[1:]
                    if height * .3 <= row[3] <= height * .95 and
                    row[2] <= height and row[4] >= 6])
    info = {'decision': 'insufficient-glyphs', 'sourceWidth': width,
            'outputWidth': width, 'glyphBoxes': boxes, 'unsafeDiagnosticOnly': True}
    if len(boxes) < 9:
        return image.copy(), info
    info['decision'] = 'no-clear-gap'
    # Merge x intervals, so a digit fragmented vertically cannot invent a gap.
    intervals = []
    for x, y, w, h in boxes:
        if intervals and x <= intervals[-1][1]:
            intervals[-1][1] = max(intervals[-1][1], x+w)
        else:
            intervals.append([x, x+w])
    if not intervals:
        return image.copy(), info
    first = intervals[0][0]
    for index, (left, right) in enumerate(zip(intervals, intervals[1:])):
        gap_start, gap_end = left[1], right[0]
        if (index+1 >= 5 and len(intervals)-index-1 >= 4 and
                height * 3 <= gap_start-first <= height * 11 and
                gap_end-gap_start >= height * .5):
            end = (gap_start+gap_end)//2
            info.update(decision='clear-gap', outputWidth=end, separator=[gap_start, gap_end])
            # Always preserve the entire left edge, including isolated minus signs.
            return image[:, :end].copy(), info
    return image.copy(), info
