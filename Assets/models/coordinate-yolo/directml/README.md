# Optional DirectML OCR runtime

Explicit experimental selection: `IMAO_OCR_BACKEND=directml`, optional `IMAO_OCR_ADAPTER` (DXGI index, default 0). CPU remains the default, using the unchanged parent-directory runtime. Initialization or inference errors fall back to CPU for the remaining runtime lifetime and are reported by `RuntimeInfo` / `ocr-backend` / `coordinate-recognition`.

- ONNX Runtime DirectML 1.24.4: https://api.nuget.org/v3-flatcontainer/microsoft.ml.onnxruntime.directml/1.24.4/microsoft.ml.onnxruntime.directml.1.24.4.nupkg
- DirectML 1.15.4 (required by that package): https://api.nuget.org/v3-flatcontainer/microsoft.ai.directml/1.15.4/microsoft.ai.directml.1.15.4.nupkg
- Only `runtimes/win-x64/native/*.dll` from ORT and `bin/x64-win/DirectML.dll` from DirectML are distributed. The DirectML archive was read using bounded HTTP ranges, retrieving only the selected Windows binary and licenses, not its Xbox binaries (~9.34 MB transferred).
- Original licenses/notices accompany the DLLs. `files.json` pins the distributed binary and license hashes.
- The model and CPU runtime digests in the parent `model.json` remain unchanged. Sequential session execution, disabled memory pattern, one OCR worker, and no repeated GPU retries limit interference, but game frame-time impact needs testing on the target PC.
