"""A crop must preserve leading signs and never use coordinate labels."""
import importlib.util
from pathlib import Path
import unittest
import numpy as np

PATH = Path(__file__).resolve().parents[2] / 'tools/coordinate-ocr/line_crop.py'
module = None
if PATH.exists():
    spec = importlib.util.spec_from_file_location('line_crop', PATH)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)


class LineCropContracts(unittest.TestCase):
    def setUp(self):
        self.assertIsNotNone(module, 'Pixel-only line crop is not implemented')

    def test_clear_gap_after_coordinate_glyphs_preserves_leading_minus(self):
        image = np.zeros((32, 400, 3), dtype=np.uint8)
        image[15:18, 5:15] = 150  # A thin, isolated leading minus.
        for x in range(20, 170, 15):
            image[5:26, x:x+10] = 150
        for x in range(200, 390, 15):
            image[5:26, x:x+10] = 150
        result, info = module.split_line(image)
        self.assertEqual(result.shape[0], image.shape[0])
        self.assertTrue(170 < result.shape[1] < 200)
        self.assertTrue(np.array_equal(result, image[:, :result.shape[1]]))
        self.assertTrue(np.array_equal(result[15:18, 5:15], image[15:18, 5:15]))
        self.assertEqual(info['decision'], 'clear-gap')

    def test_no_clear_gap_keeps_all_pixels_including_negative_sign(self):
        image = np.zeros((32, 400, 3), dtype=np.uint8)
        for x in range(5, 390, 15):
            image[5:26, x:x+10] = 150
        result, info = module.split_line(image)
        self.assertTrue(np.array_equal(result, image))
        self.assertEqual(info['decision'], 'no-clear-gap')

    def test_empty_background_is_not_treated_as_coordinate_text(self):
        image = np.zeros((32, 400, 3), dtype=np.uint8)
        result, info = module.split_line(image)
        self.assertTrue(np.array_equal(result, image))
        self.assertEqual(info['decision'], 'insufficient-glyphs')

    def test_output_does_not_share_mutable_storage_with_source(self):
        image = np.zeros((32, 400, 3), dtype=np.uint8)
        result, _ = module.split_line(image)
        result[:] = 255
        self.assertEqual(int(image.max()), 0)


if __name__ == '__main__':
    unittest.main()
