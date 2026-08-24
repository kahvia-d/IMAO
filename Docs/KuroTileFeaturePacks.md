# 库街区瓦片特征包

原项目的测试程序会下载库街区的大量公开瓦片、先拼成一张很大的 `Map.png`，再用 SURF 写出 `Map_features.yml`。这能工作，但临时图像很大、制作过程不可复现，也容易在下载中断时留下半成品。

本项目现在使用等价但更稳妥的逐瓦片流程：每张 1024×1024 PNG 独立提取 SURF，随后依照库街区前端公开的坐标换算和本项目 `World` 坐标变换落到同一内部坐标系，最后按全局网格保留响应最高的特征。描述子和关键点始终成对保留，避免旧测试代码排序时可能出现的错配。

## 梦州候选包

默认命令围绕已验证位置 `(-6725, -919)` 下载 `state 8` 的 5×5 瓦片，并生成 `Assets/FeaturesDatas/KuroTilePacks/Dreamzhou`。梦州和今州同属 `World`，因此无需新增场景 ID 或点位坐标换算。

```powershell
# 只下载到临时目录、校验并生成差异摘要；不写入 Assets。
.\scripts\Sync-KuroMapFeaturePack.ps1 -Check

# 完整生成并在全部校验通过后写入运行时资源。
.\scripts\Sync-KuroMapFeaturePack.ps1 -Apply

# 离线复核已生成包的清单、哈希、XML 和特征数量。
.\scripts\Test-KuroMapFeaturePack.ps1
```

可将 `-AnchorWorldX`、`-AnchorWorldY` 改为新的游戏坐标，`-TileRadius` 设为 1–4。每个包的 `manifest.json` 记录库街区资源版本、来源瓦片坐标和哈希、特征哈希与数量；下载、PNG 校验或 SURF 生成任何一步失败时，已有包不会被覆盖。

运行时只在清单、XML 和 SHA-256 都验证通过后加载该可选包。失败会写入 `kuro-tile-feature-pack` 诊断行，但不会影响原有场景。

库街区瓦片只能提供纹理与点位来源；生成 `state 8` 的梦州包并不代表其他顶层 state 已支持。其他 state 仍需各自的坐标变换和地图定位资源。
