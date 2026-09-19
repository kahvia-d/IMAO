# 退役的基础地图特征库（v1.0.2）

这里放的是**不再随程序发布**的基础地图特征库，2026-09-20 从
`Assets/FeaturesDatas/` 归档出来，理由与验证见 `Docs/MapRegionOnDemandHandoff.md`
（§五 第六层）。

| 文件 | 大小 | 原路径 |
|---|---|---|
| `Map_features.imf` | 127.4 MB | `Assets/FeaturesDatas/Map_features.imf` |
| `Map_visual_index.imx` | 22.9 MB | `Assets/FeaturesDatas/Map_visual_index.imx` |
| `Map_features.manifest.json` | 小 | `Assets/FeaturesDatas/Map_features.manifest.json` |
| `Map_visual_index.manifest.json` | 小 | `Assets/FeaturesDatas/Map_visual_index.manifest.json` |

## 为什么可以退役

- 13 个区域包各自带**已校准的特征**（`KuroTilePacks/<region>/features.imf`），
  配准实测覆盖基础库 88.4%，未匹配的 11.6% 是散布孔洞。
- 运行时**容忍基础库缺失**：`ResourceSnapshotContext` 只在
  `Assets/Updates/baseline-files.json` **存在时**才要求基础库文件在场；
  实机日志也印证：`stage=visual-index … ready=1 error=feature binary cannot be opened: …Map_features.imf`
  （只是记录，不是失败）。
- `out\map-test`（无基础库布局）在"13 个区域包全在"和"只留 1 个区域包"两种情况下都
  `resourcesReady: true`。

## 归档后发布链路的行为

- `scripts/Stage-UpdateResources.ps1`：只登记**存在**的基础库文件；两个都不在时
  **不生成** `baseline-files.json`，并清掉旧输出目录里遗留的那份。
- `scripts/Build-IMao.ps1`、`scripts/New-ProgramReleasePackage.ps1`：只在源里有它们时要求
  打进产物/程序包；源里没有就不要求。
- `scripts/Test-BuildPrerequisites.ps1`：两者必须**同时存在或同时不在**。

所以**后续的程序包与资源包都不会再包含这两个文件**（省 150.3 MB）。

## 需要恢复时

下面这些工具/基准依赖基础库文件在**原位**（它们不做归档路径的兼容）：

```
scripts/Build-FeatureBinary.ps1          # 由 Map_features.yml 重建 .imf
scripts/Build-VisualIndex.ps1            # 由 .imf 重建 .imx
scripts/Build-IMao-Diagnostics.ps1       # 诊断构建会读取它们
scripts/Test-Performance.ps1             # 性能基准用它做基线
IMao-Core/tools/VisualRegression         # 回放/配准工具的基准模式
```

需要用时，把本目录的四个文件拷回 `Assets/FeaturesDatas/` 即可（`.gitattributes` 已为两边
都配好 LFS）；跑完记得再移回来，避免又被打进包。

`Map_features.yml`（459.7 MB，XML 源）**没有归档**：它是重建 `.imf` 的唯一依据，
且发布暂存本来就明确排除它（`Build-IMao.ps1` 会拒绝把 base map XML 打进 release）。
