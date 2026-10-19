"""Caught upstream exceptions must fail evaluation, including debug-only parser failures."""
import logging
import os
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools/coordinate-ocr'))
import upstream_adapter


class UpstreamFailureContracts(unittest.TestCase):
    def test_logged_inference_error_cannot_be_treated_as_empty_detection(self):
        logger = logging.getLogger('coordinate-offline-error-contract')
        with self.assertRaisesRegex(ValueError, 'upstream'):
            with upstream_adapter.upstream_errors(logger):
                logger.error('ONNX inference failed: injected failure')

    def test_debug_only_parser_failure_is_visible_and_logger_level_is_restored(self):
        logger = logging.getLogger('coordinate-offline-parser-contract')
        logger.setLevel(logging.WARNING)
        with self.assertRaisesRegex(ValueError, 'upstream'):
            with upstream_adapter.upstream_errors(logger):
                logger.debug('[PATH_B] 解析失败: injected failure')
        self.assertEqual(logger.level, logging.WARNING)

    def test_normal_empty_detection_is_allowed(self):
        logger = logging.getLogger('coordinate-offline-normal-empty-contract')
        with upstream_adapter.upstream_errors(logger):
            logger.debug('no characters detected')

    @unittest.skipUnless(os.environ.get('IMAO_OCR_UPSTREAM'), 'set IMAO_OCR_UPSTREAM for the actual upstream integration probe')
    def test_actual_upstream_caught_exception_is_reported(self):
        import numpy as np
        module, _ = upstream_adapter.load_upstream(Path(os.environ['IMAO_OCR_UPSTREAM']))
        worker = module.OCRWorker({})
        class BrokenSession:
            def run(self, *args):
                raise RuntimeError('injected ORT failure')
        worker.model = BrokenSession()
        worker.model_input_name = 'images'
        worker.model_input_shape = (64, 640)
        with self.assertRaisesRegex(ValueError, 'injected ORT failure'):
            with upstream_adapter.upstream_errors(worker.logger):
                worker._run_onnx_inference(np.zeros((48, 320, 3), dtype=np.uint8))
        with self.assertRaisesRegex(ValueError, 'upstream'):
            with upstream_adapter.upstream_errors(worker.logger):
                worker._parse_path_b_spacing_dominant([object()])


if __name__ == '__main__':
    unittest.main()
