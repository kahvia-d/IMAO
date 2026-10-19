"""Clock replay consumes decoded text, never earlier selected coordinates."""
import copy
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


@unittest.skipUnless(os.environ.get('IMAO_OFFLINE_NATIVE'), 'Set IMAO_OFFLINE_NATIVE for native replay tests')
class ClockReplayContracts(unittest.TestCase):
    def fixture(self):
        row = {'id': 'a', 'pixelDigest': '0123', 'rawText': '-1,2,3 2026-10-14 14:03:57+8* HD',
               'modelScore': .99, 'totalMilliseconds': 5,
               'selected': {'x': 9999, 'y': 9999, 'z': 9999}}
        return {'metadata': {'modelName': 'fixture'}, 'pipelines': {
            route: [copy.deepcopy(row)] for route in ('ppocr-contrast', 'ppocr-tophat', 'ppocr-corroborated')}}

    def run_replay(self, data, preexisting=False):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / '日期重放'
            root.mkdir()
            source, output = root / 'source.json', root / 'result.json'
            source.write_text(json.dumps(data), encoding='utf-8')
            if preexisting:
                output.write_text('preserve evidence', encoding='utf-8')
            process = subprocess.run([os.environ['IMAO_OFFLINE_NATIVE'], '--clock-prefix', str(source), str(output)],
                                     capture_output=True, text=True, errors='replace', timeout=20)
            text = output.read_text(encoding='utf-8') if output.exists() else None
            return process, text

    def test_literal_decoded_coordinates_override_wrong_previous_selection(self):
        process, output = self.run_replay(self.fixture())
        self.assertEqual(process.returncode, 0, process.stderr)
        row = json.loads(output)['pipelines']['ppocr-corroborated-clock'][0]
        self.assertEqual([row['selected'][key] for key in ('x', 'y', 'z')], [-1, 2, 3])

    def test_low_confidence_is_still_rejected_after_valid_clock_extraction(self):
        data = self.fixture()
        for rows in data['pipelines'].values():
            rows[0]['modelScore'] = .64
        process, output = self.run_replay(data)
        self.assertEqual(process.returncode, 0, process.stderr)
        self.assertIsNone(json.loads(output)['pipelines']['ppocr-contrast-clock'][0]['selected'])

    def test_partial_clock_does_not_enable_hd_suffix_removal(self):
        data = self.fixture()
        for rows in data['pipelines'].values():
            rows[0]['rawText'] = '-1,2,3 2026-10-14 HD'
        process, output = self.run_replay(data)
        self.assertEqual(process.returncode, 0, process.stderr)
        row = json.loads(output)['pipelines']['ppocr-contrast-clock'][0]
        self.assertEqual(row['clockBoundary'], 'unchanged')
        self.assertIsNone(row['selected'])

    def test_corroborated_input_identity_mismatch_is_fatal(self):
        data = self.fixture()
        data['pipelines']['ppocr-corroborated'][0]['id'] = 'other'
        process, output = self.run_replay(data)
        self.assertNotEqual(process.returncode, 0)
        self.assertIn('input mismatch', process.stderr)
        self.assertIsNone(output)

    def test_corroborated_input_count_mismatch_is_fatal(self):
        data = self.fixture()
        data['pipelines']['ppocr-corroborated'] = []
        process, output = self.run_replay(data)
        self.assertNotEqual(process.returncode, 0)
        self.assertIn('count mismatch', process.stderr)
        self.assertIsNone(output)

    def test_existing_evidence_is_never_overwritten(self):
        process, output = self.run_replay(self.fixture(), preexisting=True)
        self.assertNotEqual(process.returncode, 0)
        self.assertEqual(output, 'preserve evidence')


if __name__ == '__main__':
    unittest.main()
