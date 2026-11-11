# Feature size and detector comparison (2026-10-02)

Branch: `test/feature-algo-budget-compare` (off `main` @ 48cab6f).
Nothing here changes product code; the branch carries a measurement harness
plus the numbers it produced.

## Question

A fellow developer reports that switching IMAO's SURF extraction to SIFT makes
localization accurate enough while cutting every map's feature file to "twenty
odd MB in total". This branch measures the size and the accuracy separately
instead of arguing from parameter lists.

## 1. The other project's published artefacts

`features.zip` and `assets.zip` from wuwa-map release `build-20261002-9`
(CI parameter set `siftgz_ct30_et10_ol2_s16_g150_mpc200_r75_ds1_ts7000_to512_bt5_em8_md40`):

| artefact | download | unpacked |
|---|---|---|
| `features.zip` (9 npz feature archives) | 21.6 MB | 43.40 MB |
| `assets.zip` (map images, coords, sqlite db) | 22.7 MB | ~4.2 MB of metadata |

Audited per file (`measurements/wuwa-map/features-audit.json`):

| | value |
|---|---|
| feature files | 9 |
| keypoints, total | **135,375** |
| on-disk, total | **21.70 MB** |
| logical (uncompressed) payload | 69.72 MB |
| compression ratio | **3.21x** |
| bytes per keypoint | **~168** |
| largest file | `8_siftgz.npz`, 73,761 keypoints, 12.48 MB |

## 2. IMAO's shipped packs

| | value |
|---|---|
| `.imf` files (KuroTilePacks + layered floors) | 112 |
| keypoints, total | **1,466,409** |
| on-disk, total | **755.19 MB** |
| bytes per keypoint | **540.00** (128 float32 descriptors + 28-byte keypoint, uncompressed) |
| retired base pack | 247,389 keypoints, 127.40 MB |

Every pack manifest carries `extractedKeypointCount == keypointCount`, all
fourteen packs: **nothing is filtered or capped between detection and storage**
(1,176,080 extracted = 1,176,080 kept for the tile packs).

## 3. Detector density on the identical raster

Each pack's raster is rebuilt from the tiles its own manifest names, so the same
input IMAO used goes into every configuration.

| pack | raster | IMAO SURF | SIFT (wuwa-map tuning) | ratio |
|---|---|---|---|---|
| tethys | 5120x5120 | 13,329 | 3,203 | 0.24 |
| avinoleum | 6144x6144 | 12,634 | 5,249 | 0.41 |
| blackshores | 5120x6144 | 15,168 | 4,614 | 0.30 |
| darkplain | 5120x5120 | 49,788 | 8,242 | 0.17 |
| fabricatorium | 12288x7168 | 32,194 | 9,422 | 0.29 |
| lahai | 11264x10240 | 366,000 | 40,228 | 0.11 |

SIFT finds 2.4x to 9x fewer keypoints than IMAO's SURF tuning on the same
pixels. The `grid=150, max_per_cell=200` cap in the other project's engine does
**not** bind on any of these rasters: it keeps every SIFT keypoint it detects.
The point saving there is the detector, not the cap.

Whole-pack sweep at 2x raster downscale (`measurements/sweep-sift-friend-large.json`),
which is also the memory guard for the largest maps:

| pack | raster | shipped SURF | SIFT at 1/2 scale | ratio | uint8+zlib | shipped |
|---|---|---|---|---|---|---|
| jinzhou | 9216x11264 | 220,090 | 25,340 | 0.12 | 2.70 MB | 113.34 MB |
| laguna | 5120x6144 | 84,407 | 11,219 | 0.13 | 1.19 MB | 43.47 MB |
| mengzhou | 4608x3584 | 94,641 | 8,684 | 0.09 | 0.94 MB | 48.74 MB |
| qiqiu | 3584x3584 | 101,652 | 9,829 | 0.10 | 1.09 MB | 52.35 MB |
| roysurface | 3584x5120 | 112,114 | 5,149 | 0.05 | 0.55 MB | 57.74 MB |
| mengshutianluo | 2048x2048 | 41,424 | 2,870 | 0.07 | 0.31 MB | 21.33 MB |
| tethys | 2560x2560 | 13,329 | 1,377 | 0.10 | 0.14 MB | 6.86 MB |
| timeriftruins | 1024x1024 | 2,072 | 430 | 0.21 | 0.05 MB | 1.07 MB |
| **8 packs** | | **669,729** | **64,898 (10.3x fewer)** | | **7.0 MB** | **344.9 MB** |

Adding the six packs measured at native scale (avinoleum, blackshores,
darkplain, fabricatorium, lahai, plus the layered-floor packs) the fourteen-pack
total lands in the low hundreds of thousands of keypoints, i.e. the other
project's "twenty-odd MB" band is reproducible from IMAO's own rasters - at
~114 B/keypoint, which is cheaper per keypoint than its published archives
(~168 B/keypoint).

`lowervault` is skipped by this sweep: its manifest names only `layered_*`
tiles, whose files are not in the tile bundle under that map id, so the raster
cannot be rebuilt here. The pack's own `features.imf` still exists and was
audited in section 2.

## 4. Does the sparse set still localize?

Synthetic minimap-sized crops (184 px) taken from the same raster at known
positions, matched back with a ratio test and shift consensus. The pack's
coordinate origin differs from the reconstructed raster, so the test scores the
consensus translation, not an absolute homography.

| pack | feature set | keypoints | verified | median inliers |
|---|---|---|---|---|
| tethys | SIFT tuning | 3,203 | 11/12 | 43 |
| tethys | SIFT default | 3,241 | 12/12 | 42 |
| tethys | SIFT dense (ct .01) | 6,151 | 12/12 | 116 |
| tethys | SIFT capped 50:20 | 1,813 | 11/12 | 34 |
| avinoleum | SIFT tuning | 5,249 | 8/10 | 43 |
| avinoleum | SIFT capped 50:20 | 3,093 | 8/10 | 25 |
| lahai | SIFT tuning | 40,228 | 7/8 | 19 |
| lahai | SIFT capped 50:20 | 14,542 | 6/8 | 12 |

The IMAO SURF column is **not measurable here**: OpenCV's prebuilt wheels exclude
the non-free module, and matching SIFT query descriptors against the shipped SURF
descriptors is invalid, because SURF's 128 floats are L2-normalized to unit
length while SIFT's are not. Measured on one textured crop, every cross-space L2
distance saturates at 1.414 and the ratio test degenerates to 1.000: zero
survivors out of twenty queries. A C++ probe linking `IMao-Core`'s own OpenCV
build is required for that column, and is not part of this branch.

## 5. Where the bytes actually are

For the same keypoint set, measured on disk, not modelled:

| format | bytes per keypoint |
|---|---|
| IMAO today: 128 float32 + 28-byte keypoint | **540** |
| wuwa-map: `np.savez_compressed`, float32 | **~161-168** |
| 128 uint8 descriptors + 28-byte keypoint | **156** |
| the same, zlib per block | **~110-114** |

Two things follow, and they are independent:

**Quantizing the descriptors costs nothing measurable.** Storing 128 uint8 values
instead of 128 float32 values verified identically in every tested
configuration: tethys 11/12, avinoleum 8/10 and 7/10, lahai 7/8 - same
verification counts, same median inlier counts, same zero shift error. That
alone takes the shipped 755.19 MB to roughly **218 MB** with no detector change.

**The remaining order of magnitude is the keypoint count.** The other project's
21.70 MB over nine feature files is 135,375 keypoints at ~168 B each; IMAO's
755.19 MB is 1,466,409 keypoints at 540 B each. Nothing about SIFT makes a
keypoint cheaper than SURF - the descriptors are the same width. The saving is
that there are fewer of them.

## 6. What this does not settle

* SURF's accuracy column (needs the C++ probe above), so the comparison is
  "the sparse set works" rather than "the sparse set is as good".
* Layered floors and caves: `lowervault`, `mengshutianluo` and the layered
  packs are where a 2-9x sparser set is most likely to lose recall first.
* Real minimap queries. The synthetic crops are native-resolution crops of the
  same raster, which is an easier problem than a game screenshot.
* Recall under the acceptance rules IMAO actually applies (translation votes,
  scale deviation, temporal hysteresis), not just inlier counts.

## Reproducing

```powershell
uv venv .venv-featureprobe --python 3.11
uv pip install --python .venv-featureprobe\Scripts\python.exe opencv-contrib-python-headless numpy

# sizes of IMAO's own packs and of the other project's npz archives
.venv-featureprobe\Scripts\python.exe tools\feature-probe\audit_sizes.py `
  --imf-dir Assets\FeaturesDatas\KuroTilePacks `
  --npz-dir measurements\wuwa-map\features

# detector / budget / localization comparison on one pack's raster
.venv-featureprobe\Scripts\python.exe tools\feature-probe\imao_vs_sift.py `
  --pack tethys --base-map-id 900 --queries 12 --scale 1.0 --quantize --grid 50:20

# keypoint and size estimate for every pack
.venv-featureprobe\Scripts\python.exe tools\feature-probe\sweep_all_packs.py `
  --detector sift-friend --downscale 2
```

`measurements/` is scratch data (downloaded release archives, audit JSON) and is
not committed.
