# Dreamzhou current-location candidate

`Assets/FeaturesDatas/DreamzhouCandidate` is a small, optional feature pack for
the first verified Dreamzhou location. It is deliberately separate from the
large Git-LFS `Map_features.yml` database.

The pack uses the clean minimap diagnostic sample captured at game coordinate
`(-6725, -919)`. At startup the native DLL verifies the image SHA-256, masks
the center player arrow and outer minimap UI, extracts SURF with the same
parameters as scene identification, and performs an in-memory self-match. Only
then are the resulting features appended to the existing `World` feature data.

Run the static asset check with:

```powershell
.\scripts\Test-DreamzhouCandidate.ps1
```

After a diagnostic game run, additionally validate the loader's self-match:

```powershell
.\scripts\Test-DreamzhouCandidate.ps1 `
  -DiagnosticsSession .\x64\Debug\Diagnostics\YYYYMMDD-HHMMSS
```

The first validation target is intentionally only one minimap-sized area around
the anchor. Do not treat a successful test as full Dreamzhou coverage. To
extend coverage, collect a clean `minimap-bootstrap-crop` at a location roughly
100--150 game-coordinate units away, preserve its matched OCR coordinate, and
add it as a separately validated reference in a subsequent pack revision.
