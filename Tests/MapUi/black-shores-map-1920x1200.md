# 16:10 client map UI regression

Source: the player's own 2026-09-27 13:58:35 session (`app-init client=1920x1200`), diagnostics frame
`8_state-change-full.png`, captured 13:59:04 at the `Gameplay->BigMap` transition. The lower-right account
UID is blacked out as in the other fixtures; no detector box overlaps the redaction.

This is the only non-16:9 big-map capture in the project. It was searched for deliberately: the working
tree, every `out/` tree, the local diagnostic sessions and the pre-rewrite history hold nothing but
2560x1440 and 1920x1080 map frames.

| Assertion | Value |
| --- | --- |
| Compass box at 1920x1200 (`hud::kBigMapCompass`, scale 1.2) | `(12,62) 86x77` |
| Gold pixels inside it | 535 / 6622 = **8.08%** |
| Same measurement on the 2560x1440 captures | 8.11% / 8.18% / 8.39% |
| Mouse zoom controls | detected (`mouse=1`) |

So a real 16:10 client draws the widget exactly where `hud::Layout` puts the box, and the ROI geometry at
this size is independently confirmed by `capture-roi-verify boxes=5 bytes=336264 mismatches=0` in the same
session (the 1600x1200 session two minutes later logged `bytes=233888`, which is exactly
`4 x (154x154 + 27x24 + 72x64 + 60x410 + 140x35)` - the compass box is 72x64 there).

## What this frame also caught: the map's open animation

The frame entered `BigMap` on `rawControls=1, compassVerified=0, compassAgreement=0.488623`, so the zoom
controls carried it. Sweeping the template against the capture explains why: the widget sits about
**5 template pixels** (3.7 client pixels) away from its settled place while the map opens, and agreement
falls to **0.46 at the box's own alignment against 0.95 at the peak**.

The search radius now covers +/-2 template pixels, so this frame scores **0.676 and verifies** (the widget
really is there, just arriving), the settled frames of the same session score higher, and
`MapControllerUiTests.h` still asserts the box position rather than the verdict, because 0.676 sits close
enough to the 0.65 threshold that pinning it would turn a rounding change into a red test.

Chasing this frame also found a bug worth remembering: the search used to shift the template's *class*
along with the sample point, which compares the template against itself and measures nothing. The
reference capture scored 0.49 under it where the real match is 0.95, and every negative frame scored
5-10x higher than it should. Fixing the search dropped the worst negative from 0.18 to **0.03** and lifted
the 1920-wide fixtures from 0.85 to 0.93.
