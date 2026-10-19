"""Load the existing upstream OCR code with Qt stubs; no inference reimplementation.

Loader adapted from chore/coord-ocr-vs-yolo:scripts/Compare-CoordinateOcr.py.
Only the local model/code are loaded; this module never downloads anything.
"""
from __future__ import annotations

import argparse
import ast
import json
import sys
import time
import types
import logging
from contextlib import contextmanager
from pathlib import Path
from typing import Any, Dict, List, Optional, Sequence, Tuple

# The upstream project drives its worker with Qt signals.  Only the class body needs them to exist.
for _name in ("PySide6", "PySide6.QtCore"):
    if _name not in sys.modules:
        sys.modules[_name] = types.ModuleType(_name)


class _QThread:
    def __init__(self, *args: Any, **kwargs: Any) -> None:
        pass

    def msleep(self, milliseconds: int) -> None:
        time.sleep(max(0.0, milliseconds / 1000.0))

    def wait(self, *args: Any, **kwargs: Any) -> bool:
        return True

    def start(self, *args: Any, **kwargs: Any) -> None:
        pass


class _Signal:
    def __init__(self, *args: Any, **kwargs: Any) -> None:
        pass

    def __get__(self, instance: Any, owner: Any = None) -> "_Signal":
        return self

    def __set_name__(self, owner: Any, name: str) -> None:
        pass

    def emit(self, *args: Any, **kwargs: Any) -> None:
        pass

    def connect(self, *args: Any, **kwargs: Any) -> None:
        pass


sys.modules["PySide6.QtCore"].QThread = _QThread  # type: ignore[attr-defined]
sys.modules["PySide6.QtCore"].Signal = _Signal  # type: ignore[attr-defined]

# Upstream's own auto capture region, from src/screen_capture.py: a bottom-left band one quarter
# of the frame wide and one thirty-second tall, anchored to the bottom edge.
AUTO_OCR_REGION_WIDTH_DIVISOR = 4
AUTO_OCR_REGION_HEIGHT_DIVISOR = 32


def auto_ocr_region_from_frame_size(width: int, height: int) -> Tuple[int, int, int, int]:
    region_width = max(1, int(width) // AUTO_OCR_REGION_WIDTH_DIVISOR)
    region_height = max(1, int(height) // AUTO_OCR_REGION_HEIGHT_DIVISOR)
    return (0, max(0, int(height) - region_height), region_width, region_height)


def load_upstream(upstream_root: Path) -> Tuple[Any, Any]:
    """Execute upstream's module-level OCR helpers and ``OCRWorker`` class.

    Returns ``(module_namespace, OCRWorker)``.  Import statements are skipped on purpose: the
    script supplies the handful of names the parts under test actually touch, so that neither Qt
    nor the rest of the upstream application has to be installed.
    """
    source_path = upstream_root / "src" / "ocr_engine.py"
    if not source_path.is_file():
        raise SystemExit(f"upstream ocr_engine.py not found at {source_path}")
    source = source_path.read_text(encoding="utf-8")

    tree = ast.parse(source)
    wanted: List[ast.stmt] = []
    worker_node: Optional[ast.ClassDef] = None
    for node in tree.body:
        if isinstance(node, (ast.Import, ast.ImportFrom)):
            continue
        if isinstance(node, ast.ClassDef) and node.name == "OCRWorker":
            worker_node = node
        wanted.append(node)
    if worker_node is None:
        raise SystemExit("OCRWorker class not found in upstream ocr_engine.py")

    import numpy as np
    import cv2
    import onnxruntime as ort
    import re
    import logging
    import datetime
    import traceback
    from typing import Callable, ClassVar, Optional as _Optional, List as _List, Tuple as _Tuple, Dict as _Dict, Any as _Any

    namespace: Dict[str, Any] = {
        "__name__": "upstream_ocr_engine",
        "__file__": str(source_path),
        "np": np,
        "npt": np.typing,
        "cv2": cv2,
        "ort": ort,
        "re": re,
        "logging": logging,
        "datetime": datetime,
        "time": time,
        "traceback": traceback,
        "Callable": Callable,
        "ClassVar": ClassVar,
        "Optional": _Optional,
        "List": _List,
        "Tuple": _Tuple,
        "Dict": _Dict,
        "Any": _Any,
        "QThread": _QThread,
        "Signal": _Signal,
    }

    # Only _load_class_names() touches this, and it is replaced below with a direct file read.
    paths_stub = types.ModuleType("core.paths")
    paths_stub.model_file = lambda name: upstream_root / "models" / name  # type: ignore[attr-defined]
    core_stub = types.ModuleType("core")
    core_stub.paths = paths_stub  # type: ignore[attr-defined]
    namespace["paths"] = paths_stub
    namespace["enumerate_gpu_adapters"] = lambda *a, **k: []
    namespace["resolve_saved_adapter"] = lambda *a, **k: None

    module = types.ModuleType("upstream_ocr_engine")
    module.__dict__.update(namespace)
    exec(compile(ast.Module(body=wanted, type_ignores=[]), str(source_path), "exec"), module.__dict__)

    worker_class = module.__dict__["OCRWorker"]
    class_names = [
        line.strip()
        for line in (upstream_root / "models" / "class_names.txt").read_text(encoding="utf-8").splitlines()
        if line.strip()
    ]
    worker_class._class_names_static = class_names
    module.__dict__["_loaded_from"] = str(source_path)
    return module, worker_class


def build_worker(upstream_root: Path, config: Dict[str, Any], module: Any = None):
    """An ``OCRWorker`` ready for ``_run_onnx_inference`` without touching Qt's event loop."""
    if module is None:
        module, _ = load_upstream(upstream_root)
    worker = module.OCRWorker(dict(config))
    model_path = upstream_root / "models" / "coord_ocr.onnx"
    if not model_path.is_file():
        raise SystemExit(f"upstream model not found at {model_path}")
    import onnxruntime as ort

    options = ort.SessionOptions()
    options.intra_op_num_threads = min(4, max(2, (__import__("os").cpu_count() or 1) // 2))
    options.inter_op_num_threads = 1
    worker.model = ort.InferenceSession(str(model_path), sess_options=options, providers=["CPUExecutionProvider"])
    inputs = worker.model.get_inputs()
    worker.model_input_name = inputs[0].name
    shape = inputs[0].shape
    if len(shape) >= 4 and isinstance(shape[2], int) and isinstance(shape[3], int):
        worker.model_input_shape = (int(shape[2]), int(shape[3]))
    return worker, module


@contextmanager
def upstream_errors(logger):
    """Expose the pinned upstream's caught exceptions without changing its algorithms."""
    failures = []
    class Capture(logging.Handler):
        def emit(self, record):
            message = record.getMessage()
            if record.levelno >= logging.ERROR or message.startswith('[PATH_B] 解析失败:'):
                failures.append(message)
    handler = Capture()
    original_level = logger.level
    logger.addHandler(handler)
    logger.setLevel(logging.DEBUG)
    try:
        yield
        if failures:
            raise ValueError('upstream engine error: ' + '; '.join(failures))
    finally:
        logger.removeHandler(handler)
        logger.setLevel(original_level)


def infer(upstream_root, inputs, repeats):
    import cv2
    import numpy as np
    from benchmark import pixel_digest
    initialize = time.perf_counter()
    worker, module = build_worker(upstream_root, {})
    with upstream_errors(worker.logger):
        worker._run_onnx_inference(np.zeros((48, 320, 3), dtype=np.uint8))
    startup_ms = (time.perf_counter() - initialize) * 1000
    results = []
    for item in inputs:
        crop = cv2.imdecode(np.frombuffer(Path(item['image']).read_bytes(), dtype=np.uint8), cv2.IMREAD_COLOR)
        if crop is None:
            raise ValueError(f'unreadable input: {item["id"]}')
        digest = pixel_digest(crop)
        if digest != item['pixelDigest']:
            raise ValueError(f'different input pixels: {item["id"]}')
        durations, first = [], None
        for repeat in range(repeats):
            start = time.perf_counter()
            with upstream_errors(worker.logger):
                detections = worker._run_onnx_inference(np.ascontiguousarray(crop))
                clusters = module.cluster_detections_to_rich_clusters(detections)
                ok, coordinates, metadata = worker._parse_path_b_spacing_dominant(detections)
            selected = dict(zip(('x', 'y', 'z'), map(int, coordinates))) if ok else None
            raw = metadata.get('raw_text_before_cleanup', '')
            signature = (raw, selected)
            if first is None:
                first = signature
            elif signature != first:
                raise ValueError(f'non-deterministic YOLO decoding: {item["id"]}')
            durations.append((time.perf_counter()-start)*1000)
        results.append({'id': item['id'], 'pixelDigest': digest, 'selected': selected,
                        'candidates': [selected] if selected is not None else [], 'rawText': raw,
                        'totalMilliseconds': float(np.median(durations)),
                        'clusters': [cluster['word'] for cluster in clusters],
                        'parserMetadata': {key: value for key, value in metadata.items() if key != 'groups'},
                        'detections': [{'char': worker._class_id_to_char_static(int(d['class'])),
                                        'confidence': float(d['confidence']),
                                        'bbox': list(map(float, d['bbox']))} for d in detections]})
    options = worker.model.get_session_options()
    return {'results': results, 'metadata': {'initializationMilliseconds': startup_ms,
            'inferenceThreads': options.intra_op_num_threads, 'interOpThreads': options.inter_op_num_threads,
            'providers': worker.model.get_providers(), 'inputShape': worker.model.get_inputs()[0].shape,
            'configuration': 'upstream defaults, spacing-dominant parser', 'repeats': repeats}}
