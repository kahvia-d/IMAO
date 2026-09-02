# 库街区瓦片特征包

原项目的测试程序会下载库街区的大量公开瓦片、先拼成一张很大的 `Map.png`，再用 SURF 写出 `Map_features.yml`。这能工作，但临时图像很大、制作过程不可复现，也容易在下载中断时留下半成品。

本项目现在使用等价但更稳妥的逐瓦片流程：每张 1024×1024 PNG 独立提取 SURF，随后依照库街区前端公开的坐标换算和本项目 `World` 坐标变换落到同一内部坐标系，最后按全局网格保留响应最高的特征。描述子和关键点始终成对保留，避免旧测试代码排序时可能出现的错配。

## 注册表和各地区独立包

`Assets/FeaturesDatas/kuro-tile-packs.json` 是瓦片包注册表。运行时、视觉索引生成器、回归程序和 WinUI 打包步骤都会按它加载，而不是只写死梦州。注册表目前列出 `Dreamzhou`、`LowerVault`、`Darkplain`、`TimeRiftRuins`；后三个目录可以暂时不存在，加载器会在诊断中标记为未安装，且不会影响旧地区。

每个新地区都必须生成自己的瓦片、`manifest.json`、`features.yml`、`features.imf` 和 `visual-index.imx`，并在清单中写入场景 ID、库街区 state、坐标换算和所有瓦片/特征哈希。只有对应地区已经获得运行时开放批准，运行时才会将该包合入视觉索引。

大世界 `World` 的主特征库是 `Map_features.imf`，它来自原项目的全图特征数据，不依赖瓦片注册表。黑海岸坐标换算到内部地图坐标后仍落在这份主库的范围内，因此不应把同一片大世界瓦片再做成重复包；重复特征会拖慢匹配并增加误匹配机会。

`-SkipReferenceVerification` 只适合为主库中确实缺失、且边界已经确认的新瓦片区域临时制作补充包。它会保留完整来源哈希并在清单中明确标为“未实机校验”；标准测试仍会拒绝该包，除非显式传入 `-AllowUnverified`。这不适用于新顶层场景的发布，也不能据此宣称定位准确。

## 梦州候选包

默认命令围绕已验证位置 `(-6725, -919)` 下载 `state 8` 的 5×5 瓦片，并生成 `Assets/FeaturesDatas/KuroTilePacks/Dreamzhou`。梦州和今州同属 `World`，因此无需新增场景 ID 或点位坐标换算。

```powershell
# 只下载到临时目录、校验并生成差异摘要；不写入 Assets。
.\scripts\Sync-KuroMapFeaturePack.ps1 -Check

# 完整生成并在全部校验通过后写入运行时资源。
.\scripts\Sync-KuroMapFeaturePack.ps1 -Apply

# 离线复核已生成包的清单、哈希、XML、二进制资源和特征数量。
.\scripts\Test-KuroMapFeaturePack.ps1
```

可将 `-AnchorWorldX`、`-AnchorWorldY` 改为新的游戏坐标，`-TileRadius` 设为 1–4。上游地图边界不是正方形时，可同时指定 `-TileMinX/-TileMaxX/-TileMinY/-TileMaxY`；四项必须一起提供、必须包含锚点瓦片，且最多 256 张。每个包的 `manifest.json` 记录库街区资源版本、来源瓦片边界和哈希、特征哈希与数量；下载、PNG 校验或 SURF 生成任何一步失败时，已有包不会被覆盖。

生成新区包时必须显式给出场景、state、四点校准得到的原点/缩放和该地区的参考小地图。例如：

```powershell
.\scripts\Sync-KuroMapFeaturePack.ps1 -Apply -PackId LowerVault -Scene LowerVault -State 902 `
  -AnchorWorldX <锚点游戏X> -AnchorWorldY <锚点游戏Y> `
  -TransformOriginX <校准原点X> -TransformOriginY <校准原点Y> -TransformScale <校准缩放> `
  -ReferencePath <正常游戏小地图截图>
```

该命令会拒绝 state/场景不匹配、PNG 不合法、参考小地图误差超过 8 像素或特征数量不足的情况。实际开放仍须按四点校准和实机验证流程完成，不因静态瓦片包通过而自动开放。

生成视觉索引时，`Build-VisualIndex.ps1` 还会把 `features.yml` 转换为经过完整回读校验的 `IMAOFT01` 二进制 `features.imf`。运行时优先读取二进制，并检查文件头中的 XML 源哈希是否与包清单一致；二进制缺失或过期时会安全回退到清单与 SHA-256 均验证通过的 XML。诊断行 `kuro-tile-feature-pack` 的 `binary=1` 表示已走快速路径，加载失败不会影响原有场景。

库街区瓦片只能提供纹理与点位来源；生成 `state 8` 的梦州包并不代表其他顶层 state 已支持。其他 state 仍需各自的坐标变换和地图定位资源。
