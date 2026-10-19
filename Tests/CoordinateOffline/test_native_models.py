"""Real native model-load failures must be explicit before inference starts."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


@unittest.skipUnless(os.environ.get('IMAO_OFFLINE_NATIVE') and os.environ.get('IMAO_ENGLISH_MODEL'),
                     'Set IMAO_OFFLINE_NATIVE and IMAO_ENGLISH_MODEL for native integration tests')
class NativeModelContracts(unittest.TestCase):
    def rejected(self, change, message):
        source = Path(os.environ['IMAO_ENGLISH_MODEL'])
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for filename in ('inference.yml', 'ppocr_keys.txt'):
                (root / filename).write_bytes((source / filename).read_bytes())
            change(root)
            (root / 'inputs.json').write_text(json.dumps({'inputs': []}), encoding='utf-8')
            report = root / 'report.json'
            result = subprocess.run([os.environ['IMAO_OFFLINE_NATIVE'], str(root),
                                     str(root / 'inputs.json'), str(report), '1'],
                                    capture_output=True, text=True, errors='replace', timeout=20)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn(message, result.stderr)
            self.assertFalse(report.exists(), 'A rejected model must never produce a successful report')

    def test_reordered_dictionary_is_rejected_instead_of_misdecoding_digits(self):
        def change(root):
            path = root / 'ppocr_keys.txt'
            lines = path.read_text(encoding='utf-8').splitlines()
            lines[0], lines[1] = lines[1], lines[0]
            path.write_text('\n'.join(lines) + '\n', encoding='utf-8')
        self.rejected(change, 'character order')

    def test_unknown_model_is_rejected(self):
        def change(root):
            path = root / 'inference.yml'
            path.write_text(path.read_text(encoding='utf-8').replace(
                'en_PP-OCRv5_mobile_rec', 'unverified_rec'), encoding='utf-8')
        self.rejected(change, 'unsupported offline model')

    def test_rgb_model_is_rejected_instead_of_changing_pixel_contract(self):
        def change(root):
            path = root / 'inference.yml'
            path.write_text(path.read_text(encoding='utf-8').replace('img_mode: BGR', 'img_mode: RGB'), encoding='utf-8')
        self.rejected(change, 'preprocessing')

    def test_missing_weights_are_not_silently_replaced_by_default_model(self):
        self.rejected(lambda root: None, 'model file is missing')


if __name__ == '__main__':
    unittest.main()
