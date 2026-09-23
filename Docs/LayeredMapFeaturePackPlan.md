# 分层地图特征包落地方案（草案，2026-09-22）

依据：`Docs/LayeredMapFeatureMeasurement_20260922.md` 第七、八节（分层瓦片是 RGBA 叠加层；
实机合成 = 地表底片 × 0.20 + 当前层；小地图 = 底片 + 当前层，无其他层遮罩）。

## 零、结论（2026-09-23，两帧实机小地图验证完毕）

**最终配方：地表瓦片全部保留；每个分层各自合成一张（`地表 × 0.35` + 该层叠加），
并在同一坐标全部列进同一个包。** 今州：16 张合成图，包体 220090 关键点（现状 207205，**+6.2%**）。

两帧实机验证（`scripts/Test-LayeredTileComposite.ps1`，`referenceVerification`，门限 8px）：

| 包 | 关键点 | 地表参考（回归） | 眠龙庭 (4,−3)<br>`2598.29, 2924, −51` | 叩天关 (3,−3)<br>`2209.33, 3355, −89` |
| --- | ---: | --- | --- | --- |
| `surface`（现状） | 207205 | ✅ 84 / 79 / 2.88px | 9 / **2** / 3.68 | 3 / **0** / **判失败** |
| `multi100` | 257060 | ✅ 84 / 79 / 2.88px | 67 / 21 / 3.70 | 33 / 12 / 2.61 |
| **`multi035`** | **220090** | ✅ **84 / 79 / 2.88px** | 61 / **38** / 3.53 | 42 / **23** / 3.69 |

（单元格 = good / near-anchor / errorPx；运行时的 Marginal 档要 inliers ≥ 8、Strong 要 ≥ 12。）

三条定论：

1. **现状包在层内不可用**：眠龙庭 only **2** 个 near-anchor（连 3 票的纯平移兜底都不够），
   叩天关直接 **0 个、判定失败**。这不是理论风险，是两帧一致的实测。
2. **`multi035` 两帧都稳**（38 / 23 个 near-anchor），且**地表侧逐位不变**
   （84 / 79 / 2.88px，三个包完全一致）。
3. **必须"每层一张"，不能把各层叠成一张**：同一坐标最多有 4 个不同层的叠加。用一帧叩天关
   小地图分别锁定：

   | 候选 | good | near-anchor |
   | --- | ---: | ---: |
   | 只叠 叩天关·上层 | 2 | 0 |
   | **只叠 叩天关·中层**（玩家实际所在层） | **21** | **21** |
   | 只叠 叩天关·下层 | 6 | 0 |
   | 只叠 环木阙·下层 | 0 | 0 |
   | 四层叠成一张 | 13 | 5 |

   叠成一张时，最后绘制的那层会盖住别人的纹理（该瓦片 25% 的叠加像素被覆盖），
   代价是 near-anchor 从 21 掉到 5 —— 而"每层一张、同坐标共存"能拿回 21（`multi035` 实测 23）。
   顺带证明：**该帧玩家在叩天关·中层**（只有这一层的合成能锁住），楼层识别是可行的。

### 早期对照（同日，`both` = 把各层叠成一张后与地表并存）

| 包 | 关键点 | 地表参考 | 眠龙庭 (4,−3) |
| --- | ---: | --- | --- |
| `surface` | 207205 | ✅ 2.88px / near 79 | near **2** |
| `both100` | 223432 | ✅ 2.88px | near 13 |
| `both035` | 212388 | ✅ 2.88px | near 30 |

`surface` 包的关键点数 **207205 与已发布的 `jinzhou` 包完全一致**，控制组可信。
"把地表瓦片替换成合成图"的做法已排除：那会让地表视图在同一坐标失效。

## 七、流水线已跑通（2026-09-23 深夜）

从归档到可运行包全部走通，产物**只在 `out/`**，源码树与 `x64\Release` 未被改动：

```powershell
. .\scripts\Enter-DevEnvironment.ps1                     # IMAO_PADDLE_LIB / IMAO_OPENCV_DIR
pwsh -File scripts\Get-MapLayerArchive.ps1               # 162 张分层瓦片 -> map-regions/layers/
pwsh -File scripts\New-LayeredTileComposite.ps1 -RegionId jinzhou   # 16 张逐层合成（k100/k035）

& .\scripts\Sync-KuroMapFeaturePack.ps1 -Apply -PackId jinzhou -Scene World `
    -TileArchive map-regions\tiles\B50F4135DCCC4D8DA87ED33CE95EA31D `
    -MaxTiles 512 -TileMinX -3 -TileMaxX 14 -TileMinY -10 -TileMaxY 11 `
    -AnchorWorldX -96 -AnchorWorldY 1310 `
    -ReferencePath map-regions\references\jinzhou.png -ReferenceFullSnapshot `
    -LayeredCompositeDir out\map-regions\composite\jinzhou\k035 `
    -OutputRoot out\map-regions\packs -AllowMissingTiles

# features.yml -> features.imf，以及 --pack-only 复用基线词表生成 visual-index.imx
x64\Release\IMaoFeatureConverter.exe  out\map-regions\packs\jinzhou\features.yml `
    out\map-regions\packs\jinzhou\features.imf out\map-regions\packs\jinzhou\features.imf.manifest.json
x64\Release\IMaoVisualIndexBuilder.exe --pack-only out\map-regions\base-index out\map-regions\packs\jinzhou --verified

pwsh -File scripts\Test-KuroMapFeaturePack.ps1 -PackRoot out\map-regions\packs\jinzhou
pwsh -File scripts\New-MapTestTree.ps1 -PackRegionId jinzhou
```

结果：**212 个瓦片条目（196 坐标 + 16 分层）/ 220090 关键点**，
`referenceVerification` 84 good / 79 near / **2.8834px**（与旧包逐位相同），
`visual-index.imx` 19.65 MB、词表 `8bd80ebd…`（与其它 12 个包一致）。
`Test-KuroMapFeaturePack.ps1` 通过，且 13 个已发布包重跑全部通过。

改动到的仓库脚本：

| 脚本 | 改动 |
| --- | --- |
| `Sync-KuroMapFeaturePack.ps1` | 新增 `-LayeredCompositeDir`：把逐层合成作为**额外条目**加进同一坐标；manifest 记 `layeredTileCount` |
| `Test-KuroMapFeaturePack.ps1` | 允许**同一坐标多条**（要求 sha256 互不相同），并校验条数 = 坐标数 + `layeredTileCount` |

### 走位验收的基线（旧包，2026-09-23 00:07–00:18 眠龙庭）

`%LOCALAPPDATA%\IMao-WinUI\Logs\events-20260923.jsonl` 里 **35 帧全部落在瓦片 (4,−3)**：

| 指标 | 旧包实测 |
| --- | --- |
| quality | 35/35 全是 **Marginal** |
| inliers | min 0 / max 5 / **avg 1.9** |
| inlierRatio | avg **0.029** |
| source | relative 18 / tracked 16 / readout 1 |

也就是说旧包在眠龙庭是"勉强连着"：一帧掉 2 个 inlier 就没了。新包走位后用同一份日志比对这三个数字即可判定。

### 走位验收结果（2026-09-23 00:51–00:54，新包，叩天关 + 眠龙庭）

`out\map-test` 里跑的确实是新包（`kuro-tile-feature-pack id=jinzhou-kurotiles keypoints=220090
visualIndexReady=1 binary=1`；旧包是 207205）。

| 指标 | 旧包（00:07，眠龙庭，约 13 秒） | **新包（00:51–00:54，两地，3.7 分钟）** |
| --- | --- | --- |
| 定位帧数 | 35（且只集中在 **4 秒**内） | **1557**（连续，每分钟 400–620 帧） |
| quality | Marginal 35 / Strong 0 | **Strong 1004 / Marginal 553** |
| inliers | min 0 / max 5 / **avg 1.9** | min 0 / max **37** / **avg 16.0** |
| inlierRatio | avg **0.029** | avg **0.570** |
| inliers < 8 的帧 | 100% | **4.7%** |
| `visual-localization-result` | 25 条：Marginal 17 / **Rejected 8** | 3 条：Strong 2 / Rejected 1 |
| 丢锁后的再捕获 | 8 次 Rejected，`searchTotal` 一路涨到 **4419**（退化为全图搜索）仍未锁回 | 基本不再需要：始终有锁 |

同一瓦片 (4,−3)（眠龙庭）逐帧对比：**inliers 1.9 → 13.4（7×）、ratio 0.029 → 0.508（17×）、
Strong 帧 0/35 → 403/810**。叩天关两格 (3,−3)/(3,−4) 共 747 帧，inliers avg 18.8 / 19.6，
Strong 601 帧 —— 而旧包在同一格静态查询下 near-anchor 是 0。

**验收通过。** 唯一保留：旧包那次只有约 13 秒活动量，与新包不是等长对照；但逐帧质量指标可比，
且新包长了 44 倍、帧数多 44 倍。

### 洞穴内冷启动（2026-09-23 11:13 起，最终验收）

判据是宿主装载的 jinzhou 包关键点数：`220090` = 带分层外观的测试树，`207205` = 旧的纯地表包。

| 会话（宿主装载的包） | 首次定位 | 检索范围 | 首个锁定 | 后续跟踪 |
| --- | --- | --- | --- | --- |
| 11:11:08（**207205，旧包**） | **4 分钟没定位上**（183 次检索全失败） | 全图 | 无 | **0 帧** |
| 11:13:39（220090） | **0.0 s** | 洞穴内 `searchTotal=30` | **Strong，30 内点，ratio 0.81** | 3874 帧（Strong 2667，平均 16 内点 / ratio 0.586） |
| 11:14:32（220090） | **1.0 s** | 洞穴内 | Marginal→Strong，11–29 内点 | 同上 |

链路：小地图 → 判出"眠龙庭·下层" → 把检索限定到该洞穴 → **第一次检索就 Strong** → 秒级锁定，**全程不开大地图**。

关键修复是 `a3ae91e`：固定尺度拟合失败时保留已经验证过的相似解。之前它把那个解整个丢掉、退化成
3 票平移，而发布位置的 `HasReacquisitionSupport` 要求几何解（`affine`）——洞穴里永远发布不出去。
实测证据：22 个匹配上 `estimateAffinePartial2D` 保留 **20 个内点、尺度 1.074**（期望 1.0543，差 1.9%），
而强制 1.0543 的平移拟合攒不到 ≥4 票一致。

**注意**：`x64\Release` 树里仍是旧包（207205），日常启动那棵树走的还是旧行为；要落地必须把分层包
装进正式构建/发布流程。

让工具在**层内**（今州：叩天关 3 层、环木阙 3 层、眠龙庭 2 层、寒雾深坑 1 层）也能定位小地图，
且**不改运行时接口、不改坐标换算、不动点位数据**。

## 二、已确定的事实（不再需要讨论）

| 事实 | 数值 |
| --- | --- |
| 分层瓦片形态 | RGBA 叠加层，不透明像素仅 0.1%–6.2%（眠龙庭·上层 (3,−3) 为 100% 透明） |
| 合成后每层特征 | 380（底片×0.20）～ 9215（底片原亮度），门槛 `inlier ≥ 12` |
| 分层瓦片网格 | 与地表瓦片**同一套 `x_y`**，所以合成后坐标天然正确 |
| 叠加层与 `(floorId, level)` 的对应 | 完全一致（928 个带楼层点位的 `(floorId, level)` ↔ `layer.id, floor.id`，0 孤儿键） |

## 三、方案 A（推荐）：把分层合成瓦片的特征并进现有地区包

1. **归档**（新增 `scripts/Get-MapLayerArchive.ps1`，或扩展 `Get-MapTileArchive.ps1`）
   - 下载 `mcmap/layer/<版本>/<state>/layer.json`；
   - 按 `floors[].tiles` 下载分层瓦片，保留 `<state>/<layerId>/<level>/<x>_<y>.png` 布局；
   - 今州 17 张、state 8 共 77 张（最大 134 KB），体量与现有归档比可忽略。
2. **合成**（新增 `scripts/New-LayeredTileComposite.ps1`）
   - 对每个 `(region, tile)`：`out = 地表瓦片 × k` 再 alpha-over 当前层叠加层；
   - 输出**不透明 PNG**，保持 1024×1024，文件名沿用 `<state>_<x>_<y>.png` 语义；
   - `k` 可配，先各建一套 `k = 0.20 / 0.35 / 1.00` 用于实机选优。
3. **打包**：把合成图当作普通瓦片喂给现有 `KuroMapFeatureBuilder.exe`（它只校验 sha256 与
   1024×1024，不关心来源），**与地表瓦片同时列进同一个 manifest，且同一坐标列多条** ——
   每个分层一条。这样无论玩家实际在第几层，包内都有对应外观。
   注意：**不是**用地表图替换，也**不是**把各层叠成一张（两者都已被实机数据否决，见第零节）。
5. **发布**：作为地区差分包更新，沿用现有资源包流程。

**成本**：今州 16 张合成图（4 个分层的各层 × 所在瓦片），包体 207205 → 220090 关键点（+6.2%），
相对 `features.imf` 111 MB 属于噪声级。

## 三·五、脚本

| 脚本 | 作用 |
| --- | --- |
| `scripts/Get-MapLayerArchive.ps1` | 归档 `layer.json` + 分层瓦片到 `map-regions/layers/<代次>/`（161 张 / 19 MB，已跑通） |
| `scripts/New-LayeredTileComposite.ps1` | 逐层合成：`地表 × k` 再 alpha-over 该层叠加，输出 `L<层>_F<楼层>_<state>_<x>_<y>.png` + `composite.manifest.json` |
| `scripts/Test-LayeredTileComposite.ps1` | A/B：`surface` / `multi100` / `multi035` 三个包 × 地表参考 + 任意多帧实机小地图 |
| `scripts/Measure-LayeredMapFeatures.ps1` | 逐层特征密度测量（第七节那份报告） |

> ⚠️ 合成脚本曾因硬链接写穿覆盖过 5 张归档瓦片（3,−3 / 3,−4 / 4,−3 / 4,−4 / 0,−2），已按
> `tiles.manifest.json` 的 sha256 重新下载并逐张校验恢复（762 条记录 0 不一致）。修法是
> 先写 `.tmp` 再 `Move-Item` 落位，绝不原地打开目标文件。


## 四、方案 B（暂不做）：每层一个包 + 运行时楼层状态

- 今州 9 个包，每包 1–3 张瓦片，成本同样很低；
- 但**只有当需要"自动判断玩家在第几层"时才有必要**。x/y 定位在方案 A 下已经正确
  （各层共用底片、共用网格，锁到"错误的层"仍然给出正确的 x/y）；
- 楼层身份建议走点位侧：`(floorId, level)` 过滤 + 分层入口 `FCRK`，与官方前端两条分支一致。

## 五、剩余验证

1. ~~同一坐标多套纹理会不会互相拖累匹配质量~~ **已验证：不会。**
2. ~~层内真实小地图能否锁定~~ **已验证，两帧：** near-anchor 从现状的 2 / 0（失败）
   提到 `multi035` 的 38 / 23。
3. ~~k 的取值只有一帧支撑~~ **已验证：两帧都是 0.35 优于 1.0**（38 vs 21、23 vs 12）。
4. ~~**层内连续移动是否掉锁**~~ **已验证：不掉。** 见第七节的走位验收结果：
   1557 帧连续跟踪、Strong 1004 帧、inliers avg 16.0，而旧包在层内 13 秒只跟住 4 秒。
5. **其余地区铺开**：同一机制，今州通过后再做（拉古那 7 层、七丘 7 层、冰原 5 层、梦州 6 层、
   拉海 14 层、黯原 5 层、泰缇斯 3 层、下层金库 4 层、隐海试验场 1 层）。

## 六、验收标准

- 层内参考小地图 `errorPixels ≤ 8`（现有门限）；
- 地表参考小地图误差不增大（不回归）；
- 层内连续移动 N 秒内不掉锁（N 沿用既有实机标准）；
- `scripts\Test-KuroMapFeaturePack.ps1` 通过。

## 七、不在本轮范围

- 其他地区的分层地图（拉古那 7 层、七丘 7 层、冰原 5 层、梦州 6 层、拉海 14 层、黯原 5 层、
  泰缇斯 3 层、下层金库 4 层、隐海试验场 1 层）——同一机制，今州验证通过后再铺开；
- 自动楼层识别；
- 官方前端的 `catalogRelation` / `area.json` 等其它图层接口。
