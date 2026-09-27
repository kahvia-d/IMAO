# Controller map UI regression

Source: local diagnostic session `20260908-174035`, captured 2026-09-08 at 2560x1440.

| Fixture | Original | Expected map controls |
| --- | --- | --- |
| `controller-map.png` | `2_state-change-full.png`, 17:40:41 | Controller RT/LT zoom track |
| `keyboard-map-current.png` | `3_state-change-full.png`, 17:40:45 | Mouse +/- zoom track |
| `controller-marker-dialog.png` | `1_state-change-full.png`, 17:40:38 | None; custom marker dialog covers normal controls |

The lower-right account UID is blacked out. All detector regions retain the original pixels and resolution. The screenshots are not synthetic UI or surveyed map positions.

The previous detector reported `mapControlsVisible=0` on the controller map despite an intact gold compass. The state fell to `Unknown` at 17:40:41 and returned to `BigMap` when mouse controls reappeared at 17:40:45. `MapControllerUiTests.h` checks the real images at 1280, 1600, 1920 and 2560 pixel widths, plus BGRA, missing anchors, obscured terrain, slider displacement, input-layout transitions, and fresh gamepad-context gating. Resizing/obscuring/displacing fixtures are controlled regression transformations, not additional real-game captures.

The compass is no longer a veto on that zoom strip. `controller-cursor-assistant.png` is a real capture of an open map whose hand-holding panel covers the compass while the RT/LT capsules and the slider are intact, so requiring both probes would reject an open map; the strip now stands on the two lettered capsules plus the slider, and the compass is a probe of its own. The compass itself is decided by a template match rather than the colour count - see `black-shores-map-region-panel.md`.

The controller screenshot also establishes that L3 is already assigned to the game's task/location tracking. The map-recognition fix does not establish a conflict-free controller shortcut.
