import importlib.util
from pathlib import Path
import unittest

PATH = Path(__file__).resolve().parents[2] / 'tools/coordinate-ocr/xy_decode.py'
spec = importlib.util.spec_from_file_location('xy_decode', PATH)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
decode = module.decode


def line(text, start=0):
    result = []
    x = start
    for char in text:
        if char == ' ':
            x += 40
            continue
        result.append({'char': char, 'score': .95, 'xyxy': [x, 4, x + 20, 38]})
        x += 24
    return result


def conflict(objects, index, char):
    return objects + [{**objects[index], 'char': char, 'score': .8}]


class XYDecoderTests(unittest.TestCase):
    def test_complete_xyz(self):
        selected = decode(line('-12,34,-5'))['selected']
        self.assertIsNotNone(selected)
        self.assertEqual(selected['xy'], [-12, 34])
        self.assertEqual(selected['z'], -5)

    def test_missing_z_with_observed_y_terminator(self):
        selected = decode(line('-12,34,'))['selected']
        self.assertIsNotNone(selected)
        self.assertEqual(selected['xy'], [-12, 34])
        self.assertIsNone(selected['z'])

    def test_date_conflict_does_not_reject_xy(self):
        objects = line('-12,34,-5 2026-10-14 14:10:21+8')
        selected = decode(conflict(objects, 9, '7'))['selected']
        self.assertIsNotNone(selected)
        self.assertEqual(selected['xy'], [-12, 34])
        self.assertEqual(selected['z'], -5)

    def test_conflicting_z_is_optional(self):
        selected = decode(conflict(line('-12,34,5'), 7, '8'))['selected']
        self.assertIsNotNone(selected)
        self.assertEqual(selected['xy'], [-12, 34])
        self.assertIsNone(selected['z'])

    def test_comma_alternative_in_z_does_not_veto_xy(self):
        selected = decode(conflict(line('-12,34,5'), 7, ','))['selected']
        self.assertIsNotNone(selected)
        self.assertEqual(selected['xy'], [-12, 34])
        self.assertIsNone(selected['z'])

    def test_nearby_isolated_sign_is_not_silently_lost(self):
        for sign in '-+':
            with self.subTest(sign=sign):
                objects = [{'char': sign, 'score': .95, 'xyxy': [-36, 4, -16, 38]}] + line('12,34,5')
                self.assertIsNone(decode(objects)['selected'])

    def test_ambiguous_coordinate_group_is_not_replaced_by_another(self):
        objects = conflict(line('1,2,3 4,5,6'), 0, '7')
        self.assertIsNone(decode(objects)['selected'])

    def test_invalid_syntax_from_sign_conflict_cannot_select_other_group(self):
        objects = conflict(line('-12,34,5 4,5,6'), 0, '+')
        self.assertIsNone(decode(objects)['selected'])

    def test_one_box_cannot_represent_zero_or_multiple_characters(self):
        for char in ['', '12']:
            with self.subTest(char=char):
                objects = line('1,2,3')
                objects[0]['char'] = char
                self.assertIsNone(decode(objects)['selected'])

    def test_conflicting_xy_rejected(self):
        self.assertIsNone(decode(conflict(line('-12,34,5'), 2, '7'))['selected'])

    def test_conflicting_sign_rejected(self):
        self.assertIsNone(decode(conflict(line('-12,34,5'), 0, '+'))['selected'])

    def test_missing_y_digit_is_not_a_shorter_integer(self):
        objects = line('-12,345,6')
        del objects[5]
        self.assertIsNone(decode(objects)['selected'])

    def test_fragment_of_x_is_not_a_new_coordinate(self):
        objects = line('1234,56,7')
        del objects[1]
        self.assertIsNone(decode(objects)['selected'])

    def test_y_requires_observed_closing_comma(self):
        self.assertIsNone(decode(line('-12,34'))['selected'])

    def test_z_glued_to_date_does_not_change_xy(self):
        selected = decode(line('-12,34,52026-10-14'))['selected']
        self.assertIsNotNone(selected)
        self.assertEqual(selected['xy'], [-12, 34])
        self.assertIsNone(selected['z'])

    def test_multiple_coordinate_groups_rejected(self):
        self.assertIsNone(decode(line('1,2,3 4,5,6'))['selected'])

    def test_extra_comma_is_ambiguous(self):
        self.assertIsNone(decode(line('1,2,3,4,5'))['selected'])

    def test_overflow_xy_rejected_but_overflow_z_optional(self):
        self.assertIsNone(decode(line('2147483648,2,3'))['selected'])
        selected = decode(line('1,2,2147483648'))['selected']
        self.assertIsNotNone(selected)
        self.assertEqual(selected['xy'], [1, 2])
        self.assertIsNone(selected['z'])

    def test_same_class_duplicate_is_not_an_extra_digit(self):
        objects = line('+12,0,-3')
        selected = decode(objects + [{**objects[1], 'score': .8}])['selected']
        self.assertIsNotNone(selected)
        self.assertEqual(selected['xy'], [12, 0])

    def test_low_confidence_delimiter_cannot_be_invented(self):
        objects = line('1,2,3')
        objects[3]['score'] = .1
        self.assertIsNone(decode(objects)['selected'])

    def test_empty_and_date_only(self):
        self.assertIsNone(decode([])['selected'])
        self.assertIsNone(decode(line('2026-10-14 14:10:21+8'))['selected'])

    def test_invalid_geometry_and_nonfinite_score(self):
        objects = line('1,2,3')
        objects[0]['xyxy'][0] = float('nan')
        self.assertIsNone(decode(objects)['selected'])
        objects = line('1,2,3')
        objects[0]['score'] = float('nan')
        self.assertIsNone(decode(objects)['selected'])


if __name__ == '__main__':
    unittest.main()
