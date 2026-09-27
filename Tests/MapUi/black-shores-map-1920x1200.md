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

The frame entered `BigMap` on `rawControls=1, compassVerified=0, compassAgreement=0.488623`. Sweeping the
template against it shows why - the best agreement is **0.95 at (-1,-5) template pixels**, i.e. about
3.7 client pixels above the box's own alignment, and 0.46 at offset zero. The same session's settled
frames scored **0.906-0.918** through the live +/-1 search, and the 1600x1200 session behaved the same way
(0.390 at its transition, 0.796-0.804 once settled).

The widget is still moving while the map opens, so the compass probe is not usable for the first second
or two; the zoom controls carry those frames. The test therefore asserts the box position on this capture
rather than a template match, and the fixture doubles as the record of that window.
