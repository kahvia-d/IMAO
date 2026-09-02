# Dreamzhou curated-location candidates

`Assets/FeaturesDatas/DreamzhouCandidate` is a small, optional feature pack for
verified Dreamzhou locations. It is deliberately separate from the large
Git-LFS `Map_features.yml` database and supplements the public Kuro tile pack
when a live minimap contains too few mutually consistent tile descriptors.

The format-2 manifest contains independently verified minimap samples captured
at game coordinates `(-6725, -919)` and `(-8519, -292)`. At startup the native
DLL verifies every image SHA-256 and dimension, masks the center player arrow
and outer minimap UI, extracts SURF with the same parameters as scene
identification, and performs an in-memory self-match for each reference. Only
then are the aggregate features appended to the existing `World` feature data.

The `(-8519, -292)` sample comes from diagnostic session
`20260824-204005`. It was separately checked against the public Kuro map tiles:
9 matches landed near the expected anchor, with about 3.84 map pixels of
position error. A reference adds retrieval evidence only; the runtime still
requires similarity-transform RANSAC and the normal confidence thresholds
before it can publish a position.

Run the static asset check with:

```powershell
.\scripts\Test-DreamzhouCandidate.ps1
```

After a diagnostic game run, additionally validate the loader's self-match:

```powershell
.\scripts\Test-DreamzhouCandidate.ps1 `
  -DiagnosticsSession .\x64\Debug\Diagnostics\YYYYMMDD-HHMMSS
```

These validation targets cover only their surrounding minimap-sized areas. Do
not treat a successful test as full Dreamzhou coverage. To extend coverage,
collect a clean minimap crop, independently verify its map anchor (preferably
against the public map tiles or a successful map-center match), and add it as a
separate manifest reference. OCR text alone is not sufficient validation.
