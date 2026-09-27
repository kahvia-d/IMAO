# Region-panel map UI regression

Source: player capture supplied 2026-09-27, 2560x1440, the open full-screen map with the region progress
panel (`下层金库 / 探索度 74%`) expanded over the left edge. The lower-right account UID is blacked out, as
in the other fixtures; no detector box overlaps the redaction.

| Fixture | Expected map controls | Expected compass |
| --- | --- | --- |
| `black-shores-map-region-panel.png` | Mouse `+/-` zoom track on the right | Verified: the panel does not cover the compass |

This capture is the reference the compiled-in compass template is generated from
(`scripts/New-MapCompassTemplate.ps1`), and it is the frame that settled how the two probes relate: the
region panel covers the left third of the screen and the compass is still drawn at its usual place, at
its usual size. Measured over the compass box, the widget's gold mask is identical to `controller-map.png`
(bounding box 344x344 at 460x412, centroid within 2 px, gold pixel count within 2%), while the region
panel's own gold emblem sits in the same box - which is why a colour count cannot decide the widget and
the template does.

`MapControllerUiTests.h` asserts, at 1280/1600/1920/2560: the compass is template-verified on this
fixture, `controller-map.png` and `keyboard-map-current.png`; it is rejected on `black-shores-gameplay.png`,
`controller-marker-dialog.png` and `controller-cursor-assistant.png`; and the region panel's gold
emblem alone is not evidence for the state machine.
