# Dreamzhou curated-location candidates（已退役）

**这个包已经删除。** 本文保留为历史记录。

退役原因：`Assets/FeaturesDatas/DreamzhouCandidate` 建于"梦州没有独立区域包"的时代，
用来补 `World` 场景里梦州那一小片地的定位。`mengzhou-kurotiles`（63 块瓦片、64750 关键点，
frame 8 / mapState 8）落地后它就被取代了。实测见 `Docs/CandidatePackRedundancy.md`。

同时删掉的还有：`Assets/FeaturesDatas/candidate-packs.json`（注册表）、
`scripts/Test-DreamzhouCandidate.ps1`、`scripts/Test-CandidateFeaturePacks.ps1`。
`CandidateFeaturePack.cpp` 里"注册表不存在就按名字加载 DreamzhouCandidate"的兜底也已移除——
它现在只是返回空列表。

---

以下为原文，描述的是已删除的实现。

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

The `(-8519, -292)` sample came from diagnostic session
`20260824-204005`. It was separately checked against the public Kuro map tiles:
9 matches landed near the expected anchor, with about 3.84 map pixels of
position error. A reference adds retrieval evidence only; the runtime still
requires similarity-transform RANSAC and the normal confidence thresholds
before it can publish a position.

The static asset check used to be:

```powershell
.\scripts\Test-DreamzhouCandidate.ps1
```

That script, and the diagnostic-session self-match variant of it, no longer
exist. The equivalent coverage question is now answered for the replacement
region pack by `Docs/CandidatePackRedundancy.md`.

These validation targets cover only their surrounding minimap-sized areas. Do
not treat a successful test as full Dreamzhou coverage. To extend coverage,
collect a clean minimap crop, independently verify its map anchor (preferably
against the public map tiles or a successful map-center match), and add it as a
separate manifest reference. OCR text alone is not sufficient validation.
