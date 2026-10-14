"""Contracts that prevent an OCR comparison from reporting misleading wins."""
import copy
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

MODULE = Path(__file__).resolve().parents[2] / 'tools/coordinate-ocr/benchmark.py'
benchmark = None
if MODULE.exists():
    spec = importlib.util.spec_from_file_location('coordinate_benchmark', MODULE)
    benchmark = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(benchmark)


class BenchmarkContracts(unittest.TestCase):
    def setUp(self):
        self.assertIsNotNone(benchmark, 'The independent benchmark has not been implemented')
        self.frame = {'id': 'a', 'state': 'present', 'expected': {'x': -12, 'y': 34, 'z': 5}}
        self.correct = {'x': -12, 'y': 34, 'z': 5}

    def test_correct_lower_rank_candidate_does_not_make_wrong_selection_correct(self):
        wrong = {'x': 12, 'y': 34, 'z': 5}
        result = benchmark.score(self.frame, {'selected': wrong, 'candidates': [wrong, self.correct]})
        self.assertFalse(result['exactXYZ'])
        self.assertTrue(result['candidateExactXYZ'])
        self.assertTrue(result['wrongReturn'])
        self.assertEqual(result['signErrors'], ['x'])

    def test_abstention_is_distinct_from_wrong_coordinate(self):
        result = benchmark.score(self.frame, {'selected': None, 'candidates': []})
        self.assertTrue(result['abstained'])
        self.assertFalse(result['wrongReturn'])
        self.assertFalse(result['exactXY'])

    def test_uncertain_z_never_enters_xyz_denominator(self):
        self.frame['expected']['z'] = None
        result = benchmark.score(self.frame, {'selected': self.correct, 'candidates': [self.correct]})
        self.assertTrue(result['exactXY'])
        self.assertIsNone(result['exactXYZ'])
        self.assertIsNone(result['candidateExactXYZ'])

    def test_wrong_z_is_reported_without_calling_xy_wrong(self):
        result = benchmark.score(self.frame, {
            'selected': {'x': -12, 'y': 34, 'z': 99}, 'candidates': []})
        self.assertTrue(result['exactXY'])
        self.assertFalse(result['exactXYZ'])
        self.assertTrue(result['wrongReturn'])

    def test_absent_frame_counts_return_as_false_positive(self):
        result = benchmark.score({'id': 'empty', 'state': 'absent', 'expected': None}, {
            'selected': self.correct, 'candidates': [self.correct]})
        self.assertTrue(result['falsePositive'])
        self.assertIsNone(result['exactXY'])

    def test_unlabelled_hard_frame_is_not_silently_scored_as_wrong(self):
        result = benchmark.score({'id': 'hard', 'state': 'unknown', 'expected': None}, {
            'selected': self.correct, 'candidates': []})
        self.assertIsNone(result['wrongReturn'])
        self.assertIsNone(result['exactXYZ'])

    def test_bottom_anchor_on_16_by_10_does_not_scale_y_from_frame_top(self):
        self.assertEqual(benchmark.roi('production', 2560, 1600), (32, 1544, 224, 56))
        self.assertEqual(benchmark.roi('production', 1920, 1080), (24, 1038, 168, 42))
        self.assertEqual(benchmark.roi('upstream-band', 2560, 1440), (0, 1395, 640, 45))

    def test_inputs_contain_neither_labels_nor_previous_truth_and_pixels_are_unmodified(self):
        import cv2
        import numpy as np
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            image = np.zeros((900, 1600, 3), dtype=np.uint8)
            image[865:900, 20:160] = [11, 22, 33]
            cv2.imwrite(str(root / 'frame.png'), image)
            corpus = {'frames': [{**self.frame, 'image': 'frame.png', 'sourceGroup': 'session-a'}]}
            inputs = benchmark.prepare(corpus, root, root / 'prepared', ['production'])
            self.assertEqual(len(inputs), 1)
            text = json.dumps(inputs)
            self.assertNotIn('expected', text)
            self.assertNotIn('previous', text)
            decoded = cv2.imread(inputs[0]['image'])
            self.assertEqual(decoded.shape, (35, 140, 3))
            self.assertTrue(np.all(decoded == [11, 22, 33]))
            self.assertEqual(inputs[0]['pixelDigest'], benchmark.pixel_digest(decoded))

    def test_missing_image_fails_instead_of_improving_score_by_omission(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            with self.assertRaisesRegex(ValueError, 'missing'):
                benchmark.prepare({'frames': [{**self.frame, 'image': 'missing.png'}]},
                                  root, root / 'out', ['production'])

    def test_input_mismatch_and_missing_results_are_fatal(self):
        inputs = [{'id': 'a@production', 'pixelDigest': '1234'}]
        good = [{'id': 'a@production', 'pixelDigest': '1234', 'selected': None}]
        benchmark.validate_results(inputs, good)
        bad = copy.deepcopy(good)
        bad[0]['pixelDigest'] = '5678'
        with self.assertRaisesRegex(ValueError, 'pixels'):
            benchmark.validate_results(inputs, bad)
        with self.assertRaisesRegex(ValueError, 'missing'):
            benchmark.validate_results(inputs, [])

    def test_duplicate_ids_and_engine_errors_are_fatal(self):
        inputs = [{'id': 'a', 'pixelDigest': '1234'}]
        row = {'id': 'a', 'pixelDigest': '1234', 'selected': None}
        with self.assertRaisesRegex(ValueError, 'duplicate'):
            benchmark.validate_results(inputs, [row, row])
        with self.assertRaisesRegex(ValueError, 'error'):
            benchmark.validate_results(inputs, [{**row, 'error': 'model failed'}])

    def test_integer_zero_is_a_real_coordinate_component(self):
        frame = {'id': 'zero', 'state': 'present', 'expected': {'x': 0, 'y': 0, 'z': 0}}
        got = {'x': 0, 'y': 0, 'z': 0}
        result = benchmark.score(frame, {'selected': got, 'candidates': [got]})
        self.assertTrue(result['exactXYZ'])
        self.assertFalse(result['abstained'])

    def test_height_error_is_not_reported_as_map_position_error(self):
        prediction = {'selected': {'x': -12, 'y': 34, 'z': 99}, 'candidates': []}
        result = benchmark.score(self.frame, prediction)
        self.assertFalse(result['wrongXYReturn'])
        self.assertTrue(result['wrongXYZReturn'])

    def test_stale_native_binary_is_rejected_even_when_current_sources_are_hashed(self):
        provenance = {'sha256': {'preprocessingSource': 'new', 'parserSource': 'parser'}}
        metadata = {'preprocessingSourceSHA256': 'old', 'parserSourceSHA256': 'parser'}
        with self.assertRaisesRegex(ValueError, 'stale'):
            benchmark.verify_native_provenance(provenance, metadata)

    def test_missing_selected_field_is_not_silently_counted_as_abstention(self):
        with self.assertRaisesRegex(ValueError, 'selected'):
            benchmark.validate_results([{'id': 'a', 'pixelDigest': '1234'}],
                                       [{'id': 'a', 'pixelDigest': '1234'}])

    def test_summary_keeps_unknown_z_and_absent_frames_out_of_accuracy_denominators(self):
        frames = [self.frame, {'id': 'xy', 'state': 'present', 'expected': {'x': 0, 'y': 7, 'z': None}},
                  {'id': 'empty', 'state': 'absent', 'expected': None}]
        inputs = [{'id': f['id'], 'sourceId': f['id'], 'profile': 'production', 'pixelDigest': 'p'} for f in frames]
        results = [{'id': 'a', 'pixelDigest': 'p', 'selected': self.correct, 'candidates': [self.correct], 'totalMilliseconds': 10},
                   {'id': 'xy', 'pixelDigest': 'p', 'selected': {'x': 0, 'y': 7, 'z': 999}, 'candidates': [], 'totalMilliseconds': 20},
                   {'id': 'empty', 'pixelDigest': 'p', 'selected': None, 'candidates': [], 'totalMilliseconds': 30}]
        summary, _ = benchmark.summarize({'frames': frames}, inputs, {'engine': results})
        self.assertEqual(summary[0]['xyCorrect'], 2)
        self.assertEqual(summary[0]['xyLabeled'], 2)
        self.assertEqual(summary[0]['xyzCorrect'], 1)
        self.assertEqual(summary[0]['xyzLabeled'], 1)
        self.assertEqual(summary[0]['absentFrames'], 1)
        self.assertEqual(summary[0]['falsePositives'], 0)

    def test_parser_header_and_linked_library_changes_require_rebuild(self):
        for name in ('parserHeader', 'ppocrHeader', 'ppocrLibrary', 'nativeBuildConfiguration'):
            with self.subTest(name=name):
                with self.assertRaisesRegex(ValueError, 'stale'):
                    benchmark.verify_native_provenance({'sha256': {name: 'new'}},
                                                       {'compiledSHA256': {name: 'old'}})

    def test_empty_corpus_cannot_produce_a_successful_empty_report(self):
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaisesRegex(ValueError, 'empty'):
                benchmark.prepare({'frames': []}, Path(directory), Path(directory), ['production'])

    def test_malformed_label_is_not_silently_removed_from_denominator(self):
        bad = {**self.frame, 'image': 'frame.png', 'expected': {'x': '-12', 'y': 34, 'z': 5}}
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaisesRegex(ValueError, 'label'):
                benchmark.prepare({'frames': [bad]}, Path(directory), Path(directory), ['production'])


if __name__ == '__main__':
    unittest.main()
