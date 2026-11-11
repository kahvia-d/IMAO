# English coordinate model offline evaluation

**Goal:** Add the official en_PP-OCRv5_mobile_rec as an offline candidate and compare existing paired captures, without changing the game runtime.

**Architecture:** The offline build generates a recognizer header from the existing header with one narrowly checked model-name allowance. Preprocessing, library, parser, confidence threshold and scoring stay identical. The benchmark accepts an explicit model directory, hashes its files, and records native model identity.

**Constraints:** Existing dictionary order must match inference.yml exactly; BGR 3x48x320 and CTC only. Download stays under 100 MB. No player installation or test tree changes. Labels never reach inference.

- [x] Add failing tests for explicit model selection and model identity verification; demonstrate the existing native model rejection.
- [x] Add the offline-only English model compatibility allowance and validate model configuration/dictionary before loading.
- [x] Build native adapter, run CoordinateOffline tests and real positive/negative model loads.
- [x] Replay English on 1160 captures, compare the retained current-model replay, and repeat both models on the 38-frame labeled/sign audit; preserve failures and unknown labels.
- [x] Document comparative results and limitations; review changes and commit the isolated branch.

**Result:** English contrast with readout-wide gives 18/20 XYZ, but loses negative signs in hard cases and reduces production corroborated XYZ from 11/20 to 8/20. Candidate only; no runtime replacement. See CoordinateCaptureStudy_20261014_zh-Hans.md and out/coordinate-english-comparison/comparison-report.md in the dependency checkout. 61 tests passed and the native build succeeded. Full English replay matches repeated audit decoding/selected coordinates; current audit matches the retained current-model decoding/selected coordinates.
