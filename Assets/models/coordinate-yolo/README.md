# IMao coordinate YOLO candidate

`coordinate.onnx` is this project's own flight trial, trained on its own coordinate crops and labels using official generic YOLO11n initialization. It contains no other player's specialized game-coordinate weights. Training and evaluation provenance: `Docs/CoordinateFlightEffectRetrain_20261016_zh-Hans.md` and `Docs/CoordinateRegressionRecovery_20261016_zh-Hans.md`.

The application uses this single weight on CPU through ONNX Runtime. No Python installation or second fallback weight is required. `model.json` freezes the artifact digests, class ordering, input dimensions and detection thresholds. The DLL was reused from the existing local ONNX Runtime 1.31.0 distribution, with its original license and third-party notices included beside it. The C API23 headers are vendored separately under `IMao-Core/packages/onnxruntime` from [Microsoft's v1.23.2 sources](https://github.com/microsoft/onnxruntime/tree/v1.23.2/include/onnxruntime/core/session), with their MIT license.

The raw and contrast detection routes use the same weight. Mandatory XY decoding, dominant-digit agreement and observed-minus recovery are implemented in the native source. Z is optional. Position-based minus recovery is enabled only for the previously calibrated 2560×1440 layout. Other HUD sizes use proportional crops without claiming calibrated sign geometry.

This is a development integration candidate. Continuous game handoff and game frame-time acceptance are still required before release. Existing Paddle libraries and explicit legacy diagnostic entrypoints remain for compatibility; the application's coordinate preload paths select this directory.
