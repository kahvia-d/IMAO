import importlib.util
from pathlib import Path
import sys
import unittest

TOOLS = Path(__file__).resolve().parents[2] / 'tools/coordinate-ocr'
sys.path.insert(0, str(TOOLS))
from xy_decode import decode
from sign_layout import audit, measure

CONTEXT = {'profile': 'readout-line-2560x1440', 'imageSize': [892, 48],
           'frameSize': [2560, 1440], 'roi': [32, 1405, 608, 33]}
CALIBRATION = {'context': CONTEXT, 'digitWidthRange': [20., 32.],
               'unsignedXOriginRange': [1.55, 1.95], 'signedXOriginRange': [2.2, 2.6],
               'unsignedYSpanRange': [1.3, 1.65], 'signedYSpanRange': [1.95, 2.35]}


def glyphs(xsign='', ysign='', omit=None, xd='12', yd='34'):
    result = []
    x = 36.
    for field, text in [('x', xsign + xd), ('separator', ','), ('y', ysign + yd), ('separator', ','), ('z', '5')]:
        for char in text:
            width, advance = (17., 17.45) if char in '+-' else (13., 13.09) if char == ',' else (26., 24.42)
            if not (field == omit and char in '+-'):
                result.append({'char': char, 'score': .95, 'xyxy': [x, 4., x + width, 40.]})
            x += advance
    return result


class SignLayoutTests(unittest.TestCase):
    def check(self, objects, context=CONTEXT):
        return audit(objects, decode(objects), CALIBRATION, context)

    def test_missing_x_minus_rejected_without_repair(self):
        objects = glyphs(xsign='-', omit='x')
        self.assertIsNotNone(decode(objects)['selected'])
        result = self.check(objects)
        self.assertIsNone(result['selected'])
        self.assertIn('x', result['suspectAxes'])

    def test_missing_y_minus_rejected_without_repair(self):
        objects = glyphs(ysign='-', omit='y')
        self.assertIsNotNone(decode(objects)['selected'])
        result = self.check(objects)
        self.assertIsNone(result['selected'])
        self.assertIn('y', result['suspectAxes'])

    def test_actual_positive_x_and_y_are_allowed(self):
        result = self.check(glyphs())
        self.assertIsNotNone(result['selected'])
        self.assertEqual(result['selected']['xy'], [12, 34])
        self.assertTrue(result['applied'])

    def test_visible_signs_are_not_repaired_or_removed(self):
        for xsign, ysign in [('-', '-'), ('+', '+'), ('-', ''), ('', '-')]:
            with self.subTest(signs=(xsign, ysign)):
                objects = glyphs(xsign, ysign)
                self.assertEqual(self.check(objects)['selected'], decode(objects)['selected'])

    def test_y_relative_geometry_ignores_x_digit_count(self):
        for xd in ['1', '12345']:
            result = self.check(glyphs(xsign='-', yd='123', xd=xd, ysign='-', omit='y'))
            self.assertIsNone(result['selected'])
            self.assertIn('y', result['suspectAxes'])

    def test_unknown_profile_is_explicitly_not_calibrated(self):
        objects = glyphs()
        result = self.check(objects, {**CONTEXT, 'frameSize': [1920, 1080]})
        self.assertFalse(result['applied'])
        self.assertEqual(result['reason'], 'uncalibrated-profile')
        self.assertEqual(result['selected'], decode(objects)['selected'])

    def test_different_font_scale_is_not_silently_validated(self):
        objects = glyphs()
        for o in objects:
            o['xyxy'] = [v * 2 for v in o['xyxy']]
        result = self.check(objects)
        self.assertFalse(result['applied'])
        self.assertEqual(result['reason'], 'uncalibrated-font-scale')

    def test_no_new_return_when_base_decoder_rejects(self):
        result = self.check([])
        self.assertIsNone(result['selected'])
        self.assertFalse(result['applied'])

    def test_unexplained_unsigned_origin_is_rejected(self):
        objects = glyphs()
        for o in objects:
            o['xyxy'][0] += 100
            o['xyxy'][2] += 100
        self.assertIsNone(self.check(objects)['selected'])

    def test_same_class_duplicates_do_not_change_geometry(self):
        objects = glyphs()
        duplicate = {**objects[0], 'score': .8}
        self.assertEqual(self.check(objects)['selected'], self.check(objects + [duplicate])['selected'])

    def test_measurement_separates_signed_and_unsigned_y(self):
        positive = glyphs()
        negative = glyphs(ysign='-')
        a = measure(positive, decode(positive))
        b = measure(negative, decode(negative))
        self.assertIsNotNone(a)
        self.assertIsNotNone(b)
        self.assertGreater(b['ySpan'] - a['ySpan'], .5)


if __name__ == '__main__':
    unittest.main()
