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

No non-16:9 big-map capture is checked in - the working tree, `out/`, the local diagnostic sessions and
the pre-rewrite history only hold 2560x1440 and 1920x1080 map frames - so the client shapes the game can
produce are covered synthetically instead, along with the one real 16:10 capture that
`black-shores-map-1920x1200.md` documents. The compass probe reads nothing but its layout box, so the test
pastes this capture's box at the size and position `hud::Layout` gives each client shape and checks the
template still verifies: 1280x800 (16:10) 0.86, 1600x1200 (4:3) 0.89, 1920x1200 (16:10) 0.90,
1920x1440 (4:3) 0.90, 2560x1600 (16:10) 1.00, 3440x1440 (21:9) 1.00, 3840x2160 0.92. On the shapes
taller than 16:9 it also asserts the opposite: a widget placed where the old per-axis scaling put the box
(5-21px away, i.e. past the +/-2 the probe searches) must *not* verify, which is the placement error that
made the map unusable there.

The worst frame without the widget scores 0.03 (the hand-holding panel over the compass); ordinary
gameplay and the marker dialog score 0.

The player's own logs back the placement rule on real clients, but only coarsely: the 2026-09-26 size
sweep ran 1600x1200 (4:3), 1920x1200 (16:10), 1920x1440 (4:3), 1920x1080 and 2560x1440, and the gold
count inside the layout box was 8.16% / 8.25% / 8.52% / 7.41% / 7.46% of that box - the same signature
the map fixtures show, so the widget is inside the box at every one of those shapes. It does *not* pin the
placement to the pixel: measured on this capture, shifting the box by the old model's error (9px at
2560x1600, 7px at 1920x1200, 17px at 1600x1200, 21px at 1920x1440) still leaves 7.33% / 4.87% / 2.94% /
3.33% gold, and only the 4:3 shapes drop near the 3% trigger. The strict position check is the template's
+/-1px tolerance, which is what the synthetic sweep above exercises.
