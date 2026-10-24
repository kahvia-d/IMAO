import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
import ctypes
import os
import struct

import cv2
import numpy as np

MODULE = Path(__file__).resolve().parents[1] / 'tools/diagnostics/repro_capture.py'


class InspectorTests(unittest.TestCase):
    def setUp(self):
        spec = importlib.util.spec_from_file_location('repro_capture', MODULE)
        self.inspector = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(self.inspector)
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name) / 'session'
        self.root.mkdir()
        (self.root / 'images').mkdir()
        self.output = Path(self.tmp.name) / 'output'
        self.pixels = np.arange(6 * 8 * 4, dtype=np.uint8).reshape(6, 8, 4)
        self.records = []
        self.context = {'captureRun': 3, 'frameId': 12, 'sourceSequence': 9, 'requestId': 4}
        self.add('wgc', image=self.pixels, metadata={'partial': True, 'width': 8, 'height': 6, 'validRegions': [[1, 1, 3, 2]]})
        self.add('client-crop', metadata={'parentStage': 'wgc', 'storage': 'crop-reference', 'crop': [1, 1, 3, 2]})
        self.add('capture-published')
        self.add('ocr-raw', image=self.pixels[:2, :3, :3])
        self.add('ocr-contrast', image=self.pixels[:2, :3, :3])
        self.add('ocr-detections', metadata={'raw': [{'char': '1', 'score': .9, 'xyxy': [0, 0, 1, 1]}], 'contrast': [], 'calibrated': True})
        self.save()

    def add(self, stage, image=None, metadata=None, context=None):
        record = {'recordId': len(self.records) + 1, 'kind': 'image' if image is not None else 'stage', 'stage': stage, 'context': self.context.copy() if context is None else context, 'metadata': metadata or {}}
        if image is not None:
            path = f'images/{record["recordId"]}.png'
            self.assertTrue(cv2.imwrite(str(self.root / path), image))
            channels = 1 if image.ndim == 2 else image.shape[2]
            record['image'] = {'path': path, 'width': image.shape[1], 'height': image.shape[0], 'channels': channels, 'type': (channels - 1) * 8}
        self.records.append(record)
        return record

    def save(self, complete=True):
        (self.root / 'metadata.json').write_text(json.dumps({'schemaVersion': 1, 'metadata': {'appVersion': 'test', 'modelSha256': hashlib.sha256(b'model').hexdigest()}}), encoding='utf-8')
        (self.root / 'completion.json').write_text(json.dumps({'complete': complete, 'savedRecords': len(self.records), 'savedImages': sum('image' in r for r in self.records), 'missingImages': 0, 'missingRecords': 0}), encoding='utf-8')
        (self.root / 'records.jsonl').write_text(''.join(json.dumps(r) + '\n' for r in self.records), encoding='utf-8')

    def inspect(self, **kwargs):
        return self.inspector.inspect_session(self.root, self.output, **kwargs)

    def test_success_exact_crop_and_replay(self):
        report = self.inspect(reconstruct=True)
        self.assertTrue(report['ok'], report)
        self.assertTrue(report['sparseFrames'][0]['partial'])
        crop = cv2.imread(report['reconstructed'][0]['path'], cv2.IMREAD_UNCHANGED)
        np.testing.assert_array_equal(crop, self.pixels[1:3, 1:4])
        replay = json.loads((self.output / 'ocr-replay.json').read_text(encoding='utf-8'))
        self.assertEqual(len(replay), 1)
        self.assertEqual(replay[0]['context'], self.context)
        self.assertEqual(replay[0]['observations']['raw'], self.records[-1]['metadata']['raw'])
        self.assertTrue(Path(replay[0]['images']['raw']).is_absolute())
        self.assertTrue((self.output / 'index.html').is_file())

    def test_incomplete_is_failure(self):
        self.save(complete=False)
        self.assertFalse(self.inspect()['ok'])
        self.assertEqual(self.inspector.main([str(self.root), '--output', str(self.output)]), 1)

    def test_missing_image_is_corruption(self):
        (self.root / 'images/1.png').unlink()
        self.assertTrue(any(x['code'] == 'image-invalid' for x in self.inspect()['errors']))

    def test_unsafe_paths_refused(self):
        for value in ['../outside.png', r'..\outside.png', 'C:/outside.png', '/outside.png', 'images/a.png:stream']:
            with self.subTest(value=value):
                self.records[0]['image']['path'] = value
                self.save()
                self.assertTrue(any(x['code'] == 'unsafe-image-path' for x in self.inspect()['errors']))

    def test_shape_and_duplicate_ids_rejected(self):
        self.records[0]['image']['width'] = 7
        self.records[-1]['recordId'] = 1
        self.save()
        codes = {x['code'] for x in self.inspect()['errors']}
        self.assertIn('image-invalid', codes)
        self.assertIn('record-id', codes)

    def test_missing_source_is_boundary_gap(self):
        self.records[1]['context']['sourceSequence'] = 100
        self.save()
        report = self.inspect(reconstruct=True)
        self.assertEqual(report['gaps'][0]['code'], 'source-outside-session')
        self.assertFalse(report['errors'])

    def test_requests_never_mixed(self):
        self.records[3]['context']['requestId'] = 5
        self.save()
        report = self.inspect()
        self.assertTrue(any(x['code'] == 'ocr-stage-missing' for x in report['gaps']))
        self.assertEqual(json.loads((self.output / 'ocr-replay.json').read_text()), [])

    def test_model_hash_checked(self):
        model = Path(self.tmp.name) / 'models'
        model.mkdir()
        (model / 'coordinate.onnx').write_bytes(b'wrong')
        self.assertTrue(any(x['code'] == 'model-mismatch' for x in self.inspect(model=model)['errors']))
        (model / 'coordinate.onnx').write_bytes(b'model')
        self.assertTrue(self.inspect(model=model)['ok'])

    def test_pending_request_reported_at_stop_boundary(self):
        self.records.pop()
        self.save()
        report = self.inspect()
        self.assertTrue(report['ok'])
        self.assertTrue(any(x['code'] == 'ocr-stage-missing' and x['context']['requestId'] == 4 for x in report['gaps']))
        self.assertEqual(report['replayCount'], 0)

    def test_crop_validated_without_reconstruction(self):
        self.records[1]['metadata']['crop'] = [7, 1, 3, 2]
        self.save()
        self.assertTrue(any(x['code'] == 'client-crop-invalid' for x in self.inspect()['errors']))

    def test_sparse_region_bounds_checked(self):
        self.records[0]['metadata']['validRegions'] = [[1, 1, 30, 2]]
        self.save()
        self.assertTrue(any(x['code'] == 'readback-invalid' for x in self.inspect()['errors']))

    def test_png_damage_detected(self):
        path = self.root / 'images/1.png'
        data = bytearray(path.read_bytes())
        data[-5] ^= 1
        path.write_bytes(data)
        self.assertTrue(any(x['code'] == 'image-invalid' for x in self.inspect()['errors']))

    def test_output_inside_session_refused(self):
        with self.assertRaises(ValueError):
            self.inspector.inspect_session(self.root, self.root / 'derived')

    def xpress_record(self, payload):
        descriptor = self.records[0]['image']
        descriptor.update({'encoding': 'windows-xpress-chunks-v1', 'path': 'images/1.xpress',
                           'rawBytes': self.pixels.nbytes, 'rowBytes': self.pixels.strides[0]})
        (self.root / descriptor['path']).write_bytes(payload)
        self.save()
        return descriptor

    @unittest.skipUnless(os.name == 'nt', 'Windows CompressionAPI required')
    def test_xpress_exact_pixels_crop_and_replay(self):
        cabinet = ctypes.WinDLL('cabinet', use_last_error=True)
        cabinet.CreateCompressor.argtypes = [ctypes.c_uint32, ctypes.c_void_p, ctypes.POINTER(ctypes.c_void_p)]
        cabinet.CreateCompressor.restype = ctypes.c_int
        cabinet.Compress.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
        cabinet.Compress.restype = ctypes.c_int
        cabinet.CloseCompressor.argtypes = [ctypes.c_void_p]
        handle = ctypes.c_void_p()
        self.assertTrue(cabinet.CreateCompressor(3, None, ctypes.byref(handle)))
        pieces = []
        try:
            raw = self.pixels.tobytes()
            for chunk in np.array_split(np.frombuffer(raw, dtype=np.uint8), 4):
                source = ctypes.create_string_buffer(chunk.tobytes())
                needed = ctypes.c_size_t()
                cabinet.Compress(handle, source, len(chunk), None, 0, ctypes.byref(needed))
                self.assertGreater(needed.value, 0)
                target = ctypes.create_string_buffer(needed.value)
                self.assertTrue(cabinet.Compress(handle, source, len(chunk), target, len(target), ctypes.byref(needed)))
                pieces.append(struct.pack('<QQ', len(chunk), needed.value) + target.raw[:needed.value])
        finally:
            cabinet.CloseCompressor(handle)
        descriptor = self.xpress_record(b'IMAOXPR1' + struct.pack('<I', 4) + b''.join(pieces))
        decoded = self.inspector._decode_image(self.root / descriptor['path'], descriptor)
        np.testing.assert_array_equal(decoded, self.pixels)
        report = self.inspect(reconstruct=True)
        self.assertTrue(report['ok'], report)
        crop = cv2.imread(report['reconstructed'][0]['path'], cv2.IMREAD_UNCHANGED)
        np.testing.assert_array_equal(crop, self.pixels[1:3, 1:4])
        self.assertEqual(report['replayCount'], 1)
        preview = cv2.imread(report['decodedPreviews'][0]['path'], cv2.IMREAD_UNCHANGED)
        np.testing.assert_array_equal(preview, self.pixels)

    def test_xpress_malformed_payloads_rejected(self):
        for payload in [b'', b'IMAOXPR1', b'IMAOXPR1' + struct.pack('<I', 0),
                        b'IMAOXPR1' + struct.pack('<I', 2 ** 32 - 1),
                        b'IMAOXPR1' + struct.pack('<IQQ', 1, 2 ** 63, 4) + b'abcd',
                        b'IMAOXPR1' + struct.pack('<IQQ', 1, 192, 100) + b'abcd',
                        b'IMAOXPR1' + struct.pack('<IQQ', 1, 192, 4) + b'abcd']:
            with self.subTest(payload=payload[:20]):
                self.xpress_record(payload)
                self.assertTrue(any(x['code'] == 'image-invalid' for x in self.inspect()['errors']))


if __name__ == '__main__':
    unittest.main()
