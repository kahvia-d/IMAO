"""Captured strips must preserve full-client geometry and stay label-blind."""
import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
def load(name):
    path = ROOT / 'tools/coordinate-ocr' / (name + '.py')
    if not path.exists():
        return None
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module
sessions = load('sessions')
benchmark = load('benchmark')


class CapturedStripContracts(unittest.TestCase):
    def test_strip_is_cropped_using_original_client_dimensions(self):
        import cv2
        import numpy as np
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            # Original client: 1600x900; stored bottom-left context: (0,820,400,80).
            pixels = np.zeros((80, 400, 3), dtype=np.uint8)
            pixels[45:80, 20:160] = [11, 22, 33]
            cv2.imwrite(str(root/'strip.png'), pixels)
            frame = {'id': 'strip', 'image': 'strip.png', 'state': 'unknown', 'expected': None,
                     'frameSize': [1600, 900], 'sourceRect': [0, 820, 400, 80]}
            inputs = benchmark.prepare({'frames': [frame]}, root, root/'out', ['production'])
            decoded = cv2.imread(inputs[0]['image'])
            self.assertEqual(decoded.shape, (35, 140, 3))
            self.assertTrue(np.all(decoded == [11, 22, 33]))
            self.assertEqual(inputs[0]['rect'], [20, 865, 140, 35])
            self.assertEqual(inputs[0]['frameSize'], [1600, 900])

    def test_insufficient_strip_does_not_silently_truncate_requested_roi(self):
        import cv2
        import numpy as np
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            cv2.imwrite(str(root/'strip.png'), np.zeros((35, 140, 3), dtype=np.uint8))
            frame = {'id': 'strip', 'image': 'strip.png', 'state': 'unknown', 'expected': None,
                     'frameSize': [1600, 900], 'sourceRect': [20, 865, 140, 35]}
            with self.assertRaisesRegex(ValueError, 'cover'):
                benchmark.prepare({'frames': [frame]}, root, root/'out', ['upstream-band'])


class SessionExportContracts(unittest.TestCase):
    def setUp(self):
        self.assertIsNotNone(sessions, 'Capture session inspection/export is not implemented')
        import cv2
        import numpy as np
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name) / '中文样本'
        self.root.mkdir()
        cv2.imencode('.png', np.full((80, 400, 3), 42, dtype=np.uint8))[1].tofile(self.root/'frame-000001.png')
        data = (self.root/'frame-000001.png').read_bytes()
        self.records = [
            {'type': 'session-start', 'formatVersion': 1, 'sessionId': 'session-a', 'origin': 'game', 'backend': 'wgc'},
            {'type': 'sample', 'id': 'session-a-000001', 'image': 'frame-000001.png',
             'frameSize': [1600, 900], 'sourceRect': [0, 820, 400, 80], 'sha256': hashlib.sha256(data).hexdigest()},
            {'type': 'session-end', 'samples': 1, 'reason': 'user-stop'}]
        self.save()

    def tearDown(self):
        if hasattr(self, 'temp'):
            self.temp.cleanup()

    def save(self):
        (self.root/'index.jsonl').write_text(''.join(json.dumps(row)+'\n' for row in self.records), encoding='utf-8')

    def test_export_preserves_session_group_and_has_no_invented_labels(self):
        report = sessions.inspect_session(self.root)
        frame = report['corpus']['frames'][0]
        self.assertEqual(frame['sourceGroup'], 'session-a')
        self.assertEqual(frame['state'], 'unknown')
        self.assertIsNone(frame['expected'])
        self.assertEqual(frame['frameSize'], [1600, 900])
        self.assertEqual(frame['sourceRect'], [0, 820, 400, 80])
        self.assertTrue(Path(frame['image']).is_file())

    def test_missing_image_is_fatal(self):
        (self.root/'frame-000001.png').unlink()
        with self.assertRaisesRegex(ValueError, 'missing'):
            sessions.inspect_session(self.root)

    def test_changed_image_bytes_are_fatal(self):
        self.records[1]['sha256'] = '0'*64
        self.save()
        with self.assertRaisesRegex(ValueError, 'digest'):
            sessions.inspect_session(self.root)

    def test_unfinished_session_requires_explicit_recovery(self):
        self.records.pop()
        self.save()
        with self.assertRaisesRegex(ValueError, 'unfinished'):
            sessions.inspect_session(self.root)
        report = sessions.inspect_session(self.root, allow_incomplete=True)
        self.assertFalse(report['complete'])

    def test_fixture_samples_cannot_be_mixed_into_real_game_dataset(self):
        self.records[0]['origin'] = 'fixture'
        self.save()
        with self.assertRaisesRegex(ValueError, 'fixture'):
            sessions.inspect_session(self.root)

    def test_path_escape_and_duplicate_ids_are_fatal(self):
        self.records[1]['image'] = '../frame-000001.png'
        self.save()
        with self.assertRaisesRegex(ValueError, 'path'):
            sessions.inspect_session(self.root)
        self.records[1]['image'] = 'frame-000001.png'
        self.records.insert(2, dict(self.records[1]))
        self.records[-1]['samples'] = 2
        self.save()
        with self.assertRaisesRegex(ValueError, 'duplicate'):
            sessions.inspect_session(self.root)

    def test_end_count_must_match_index(self):
        self.records[-1]['samples'] = 99
        self.save()
        with self.assertRaisesRegex(ValueError, 'count'):
            sessions.inspect_session(self.root)

    def test_unindexed_images_are_reported_in_recovery_and_rejected_when_complete(self):
        (self.root/'orphan.png').write_bytes((self.root/'frame-000001.png').read_bytes())
        with self.assertRaisesRegex(ValueError, 'unindexed'):
            sessions.inspect_session(self.root)
        self.records.pop()
        self.save()
        report = sessions.inspect_session(self.root, allow_incomplete=True)
        self.assertEqual(report['unindexedFiles'], ['orphan.png'])

    def test_records_after_session_end_are_rejected(self):
        self.records.append({'type': 'session-end', 'samples': 1, 'reason': 'user-stop'})
        self.save()
        with self.assertRaisesRegex(ValueError, 'lifecycle'):
            sessions.inspect_session(self.root)

    def test_existing_annotations_are_validated_and_not_derived_from_model_outputs(self):
        labels = {'session-a-000001': {'state': 'present', 'expected': {'x': -1, 'y': 2, 'z': None}}}
        report = sessions.inspect_session(self.root, labels=labels)
        self.assertEqual(report['corpus']['frames'][0]['expected'], {'x': -1, 'y': 2, 'z': None})
        with self.assertRaisesRegex(ValueError, 'unknown'):
            sessions.inspect_session(self.root, labels={'other': labels['session-a-000001']})


class PairedSessionContracts(SessionExportContracts):
    def setUp(self):
        super().setUp()
        import cv2
        import numpy as np
        cv2.imencode('.png', np.full((154, 154, 3), 99, dtype=np.uint8))[1].tofile(self.root/'frame-000001.minimap.png')
        data = (self.root/'frame-000001.minimap.png').read_bytes()
        self.records[0].update(formatVersion=2, pairedMinimap=True)
        self.records[1]['frameSystemRelativeTime100ns'] = 12345
        self.records[1]['minimap'] = {'frameId': 'session-a-000001', 'image': 'frame-000001.minimap.png',
                                     'sourceRect': [30, 23, 154, 154], 'sha256': hashlib.sha256(data).hexdigest(),
                                     'pngBytes': len(data)}
        self.records[1]['pngBytes'] = (self.root/'frame-000001.png').stat().st_size
        self.records[-1]['pngBytes'] = self.records[1]['pngBytes'] + len(data)
        self.save()

    def test_export_keeps_same_frame_minimap_without_inventing_coordinates(self):
        report = sessions.inspect_session(self.root)
        frame = report['corpus']['frames'][0]
        self.assertEqual(report['pairedSamples'], 1)
        self.assertEqual(frame['frameSystemRelativeTime100ns'], 12345)
        self.assertEqual(frame['minimap']['frameId'], frame['id'])
        self.assertEqual(frame['minimap']['sourceRect'], [30, 23, 154, 154])
        self.assertTrue(Path(frame['minimap']['image']).is_file())
        self.assertIsNone(frame['expected'])
        inputs = benchmark.prepare(report['corpus'], self.root, self.root/'ocr-inputs', ['production'])
        self.assertNotIn('minimap', inputs[0])
        self.assertNotIn('frameSystemRelativeTime100ns', inputs[0])

    def test_missing_minimap_is_fatal_even_in_recovery(self):
        (self.root/'frame-000001.minimap.png').unlink()
        with self.assertRaisesRegex(ValueError, 'missing'):
            sessions.inspect_session(self.root, allow_incomplete=True)

    def test_replay_rechecks_pair_after_export_without_leaking_it_to_ocr(self):
        report = sessions.inspect_session(self.root)
        (self.root/'frame-000001.minimap.png').unlink()
        with self.assertRaisesRegex(ValueError, 'minimap'):
            benchmark.prepare(report['corpus'], self.root, self.root/'ocr-inputs', ['production'])

    def test_pair_replay_requires_coordinate_digest_and_rejects_replaced_pixels(self):
        import cv2
        import numpy as np
        report = sessions.inspect_session(self.root)
        frame = report['corpus']['frames'][0]
        original = frame.pop('captureSHA256')
        with self.assertRaisesRegex(ValueError, 'digest'):
            benchmark.prepare(report['corpus'], self.root, self.root/'ocr-inputs', ['production'])
        frame['captureSHA256'] = ''
        with self.assertRaisesRegex(ValueError, 'digest'):
            benchmark.prepare(report['corpus'], self.root, self.root/'ocr-inputs', ['production'])
        frame['captureSHA256'] = original
        cv2.imencode('.png', np.full((80, 400, 3), 123, dtype=np.uint8))[1].tofile(self.root/'frame-000001.png')
        with self.assertRaisesRegex(ValueError, 'digest'):
            benchmark.prepare(report['corpus'], self.root, self.root/'ocr-inputs', ['production'])

    def test_missing_pair_metadata_is_not_treated_as_legacy(self):
        del self.records[1]['minimap']
        self.save()
        with self.assertRaisesRegex(ValueError, 'paired'):
            sessions.inspect_session(self.root)

    def test_wrong_minimap_digest_geometry_or_frame_id_is_fatal(self):
        original = dict(self.records[1]['minimap'])
        for field, value, error in [('sha256', '0'*64, 'digest'),
                                     ('sourceRect', [30, 23, 153, 154], 'geometry'),
                                     ('frameId', 'another-frame', 'frame')]:
            self.records[1]['minimap'] = dict(original, **{field: value})
            self.save()
            with self.assertRaisesRegex(ValueError, error):
                sessions.inspect_session(self.root)

    def test_pair_png_budget_and_time_are_checked(self):
        self.records[-1]['pngBytes'] += 1
        self.save()
        with self.assertRaisesRegex(ValueError, 'bytes'):
            sessions.inspect_session(self.root)
        self.records[-1]['pngBytes'] -= 1
        del self.records[1]['frameSystemRelativeTime100ns']
        self.save()
        with self.assertRaisesRegex(ValueError, 'time'):
            sessions.inspect_session(self.root)


if __name__ == '__main__':
    unittest.main()
