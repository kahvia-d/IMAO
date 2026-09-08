# Gamepad map cursor detector fixtures

`GamepadMapCursorDetector::Detect(image, clientRect)` accepts a complete 8-bit BGR or BGRA game-client capture. The RECT width and height must equal the image dimensions. The returned center and radius are physical image pixels relative to the client origin. Confidence is shape/brightness support, not a calibrated probability.

The detector requires both controller RT/LT anchors and the R zoom slider from `MapUiVisualDetector`. It does not require the compass to remain uncovered: the reported assistant window covers that corner in the new real screenshot. The map's central content area is searched for a complete, thin, neutral-white ring with dark inner/outer shoulders and a mostly open interior. Fixed command bars and the right zoom strip are excluded. It never substitutes the screen center or OS mouse position; zero or multiple qualifying rings return invisible. Callers must independently bind the result to a fresh, valid presented big-map capture and its coordinate mapping.

`Tests/MapUi/controller-cursor-assistant.png` comes from the user's 2026-09-08 clipboard screenshot, originally 2560×1440. Only the lower-right account-label strip is blacked out. The map/cursor/control regions retain their original pixels. The two original positive images show the ring around image coordinate `(1279, 719)`; this is a manually inspected fixture label, not a production constant or a surveyed map/player coordinate. The ring stroke radius is approximately 38 pixels at that resolution.

Run `Tests/GamepadMapCursor/Test.ps1` from the repository to compile and execute the independent native fixture runner. It does not change CMake or the standard build directory. The existing `App/*.cpp` source glob automatically includes the detector in the production build. `PrepareFixture.ps1 -SourcePath <original screenshot>` prepares the account-redacted fixture and refuses to overwrite an existing one.

Coverage includes:

- Real assistant screenshot and existing `controller-map.png` positives, plus `controller-marker-dialog.png` and `keyboard-map-current.png` negatives.
- Image widths 1280, 1600, 1920, 2560, 3200, 3840 and 5120; BGR and BGRA. The first four widths correspond to 100/125/150/200% of a 1280-pixel image. These are controlled image resizes, not actual monitor DPI tests.
- Removed ring with other real map/teleport icons retained; half occlusion; missing controller controls; partial, filled, thick and colored circular distractors.
- A real ring patch translated away from screen center, two real-ring patches (ambiguity), and a real ring pasted into mouse-layout UI.
- Empty images, mismatched capture/client dimensions and unsupported image formats.

Results and detector timing are written to `out/gamepad-map-cursor/tests.log`; build diagnostics are in `build.log`. These checks establish behavior on the supplied screenshots and controlled transformations. They do not establish all game scenes, HDR/color modes, unusual aspect ratios, obscured rings, live controller movement, or actual DPI compatibility. Static images cannot distinguish two visually identical ring designs; a second equally qualifying ring is rejected rather than guessed.
