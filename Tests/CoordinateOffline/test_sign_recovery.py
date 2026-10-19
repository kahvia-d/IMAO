import sys
from pathlib import Path
import unittest
import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools/coordinate-ocr'))
from xy_decode import decode
from sign_recovery import recover
from test_sign_layout import glyphs, CALIBRATION, CONTEXT


def proof(objects):
    image = np.full((48, 892, 3), 80, np.uint8)
    result = []
    for o in objects:
        if o['char'] == '-':
            a, _, b, _ = o['xyxy']
            box = [a, 22., b, 30.]
            result.append({**o, 'xyxy': box})
            image[24:28, round(a)+2:round(b)-2] = 220
        else:
            result.append(dict(o))
    return image, result


class SignRecoveryTests(unittest.TestCase):
    def call(self, raw, image, evidence, context=CONTEXT):
        return recover(raw, decode(raw), CALIBRATION, context, image, evidence)

    def test_x_restored_only_with_local_minus_and_stroke(self):
        raw = glyphs(xsign='-', omit='x')
        image, evidence = proof(glyphs(xsign='-'))
        result = self.call(raw, image, evidence)
        self.assertIsNotNone(result['selected'])
        self.assertEqual(result['selected']['xy'], [-12, 34])
        self.assertEqual(result['restoredAxes'], ['x'])

    def test_y_restored_with_numeric_values_preserved(self):
        raw = glyphs(ysign='-', omit='y')
        image, evidence = proof(glyphs(ysign='-'))
        result = self.call(raw, image, evidence)
        self.assertIsNotNone(result['selected'])
        self.assertEqual(result['selected']['xy'], [12, -34])

    def test_both_axes_require_both_proofs(self):
        raw = [o for o in glyphs('-', '-') if o['char'] != '-']
        image, evidence = proof(glyphs('-', '-'))
        result = self.call(raw, image, evidence)
        self.assertIsNotNone(result['selected'])
        self.assertEqual(result['selected']['xy'], [-12, -34])
        evidence = [o for o in evidence if not(o['char'] == '-' and o['xyxy'][0] > 80)]
        self.assertIsNone(self.call(raw, image, evidence)['selected'])

    def test_flat_pixels_cannot_confirm_a_detector_minus(self):
        image, evidence = proof(glyphs(xsign='-'))
        image[:] = 80
        self.assertIsNone(self.call(glyphs(xsign='-', omit='x'), image, evidence)['selected'])

    def test_monotonic_brightness_ramp_is_not_a_minus(self):
        image, evidence = proof(glyphs(xsign='-'))
        image[:] = np.arange(48, dtype=np.uint8)[:, None, None] * 4
        self.assertIsNone(self.call(glyphs(xsign='-', omit='x'), image, evidence)['selected'])

    def test_single_background_boundary_is_not_a_minus(self):
        image, evidence = proof(glyphs(xsign='-'))
        image[:] = 80
        image[26:] = 220
        self.assertIsNone(self.call(glyphs(xsign='-', omit='x'), image, evidence)['selected'])

    def test_full_width_background_band_is_not_a_local_minus(self):
        image, evidence = proof(glyphs(xsign='-'))
        image[:] = 80
        image[24:28] = 220
        self.assertIsNone(self.call(glyphs(xsign='-', omit='x'), image, evidence)['selected'])

    def test_right_extending_background_band_is_not_a_local_minus(self):
        image, evidence = proof(glyphs(xsign='-'))
        image[:] = 80
        image[24:28, 36:] = 220
        self.assertIsNone(self.call(glyphs(xsign='-', omit='x'), image, evidence)['selected'])

    def test_left_extending_background_band_is_not_a_local_minus(self):
        image, evidence = proof(glyphs(xsign='-'))
        image[:] = 80
        image[24:28, :53] = 220
        self.assertIsNone(self.call(glyphs(xsign='-', omit='x'), image, evidence)['selected'])

    def test_pixels_without_minus_detection_are_insufficient(self):
        image, evidence = proof(glyphs(xsign='-'))
        evidence = [o for o in evidence if o['char'] != '-']
        self.assertIsNone(self.call(glyphs(xsign='-', omit='x'), image, evidence)['selected'])

    def test_weak_minus_is_not_confirmation(self):
        image, evidence = proof(glyphs(xsign='-'))
        for o in evidence:
            if o['char'] == '-':
                o['score'] = .49
        self.assertIsNone(self.call(glyphs(xsign='-', omit='x'), image, evidence)['selected'])

    def test_other_character_at_sign_slot_is_ambiguous(self):
        image, evidence = proof(glyphs(xsign='-'))
        sign = next(o for o in evidence if o['char'] == '-')
        evidence.append({**sign, 'char': '+', 'score': .6})
        self.assertIsNone(self.call(glyphs(xsign='-', omit='x'), image, evidence)['selected'])

    def test_date_minus_cannot_repair_x(self):
        image, evidence = proof(glyphs(xsign='-'))
        for o in evidence:
            if o['char'] == '-':
                o['xyxy'] = [400., 22., 417., 30.]
        self.assertIsNone(self.call(glyphs(xsign='-', omit='x'), image, evidence)['selected'])

    def test_neighbor_digit_must_agree(self):
        image, evidence = proof(glyphs(xsign='-'))
        next(o for o in evidence if o['char'] == '1')['char'] = '9'
        self.assertIsNone(self.call(glyphs(xsign='-', omit='x'), image, evidence)['selected'])

    def test_positive_numbers_and_visible_signs_are_untouched(self):
        for raw in [glyphs(), glyphs('-', '-')]:
            result = self.call(raw, np.zeros((48, 892, 3), np.uint8), [])
            self.assertEqual(result['selected'], decode(raw)['selected'])
            self.assertEqual(result['restoredAxes'], [])

    def test_uncalibrated_profile_cannot_restore(self):
        raw = glyphs(xsign='-', omit='x')
        image, evidence = proof(glyphs(xsign='-'))
        result = self.call(raw, image, evidence, {**CONTEXT, 'frameSize': [1920, 1080]})
        self.assertEqual(result['restoredAxes'], [])
        self.assertEqual(result['selected'], decode(raw)['selected'])

    def test_wrong_image_shape_and_base_missing_are_rejected(self):
        raw = glyphs(xsign='-', omit='x')
        image, evidence = proof(glyphs(xsign='-'))
        self.assertIsNone(self.call(raw, image[:20], evidence)['selected'])
        self.assertIsNone(self.call([], image, evidence)['selected'])


if __name__ == '__main__':
    unittest.main()
