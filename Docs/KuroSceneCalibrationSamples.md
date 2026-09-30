# 新地区四点校准样本

每个新地区必须有四个位置。每个位置连续采集两张图：第一张是正常游戏画面，要求小地图和游戏内位置数字清楚；第二张是打开大地图且玩家箭头可见。两张图之间人物不能移动，也不能被界面遮挡。位置数字只用于生成本文件中的离线校准数据，运行时不会进行坐标识别。

## 快捷键采集助手

构建后的 `x64\\Release\\IMaoCaptureAssistant.exe` 可以采集这些资料，而不会向游戏发送按键、鼠标或读取游戏内存。它只截取可见且未最小化的 `Client-Win64-Shipping.exe` 窗口，使用的截图路径与覆盖层一致。

在仓库根目录打开终端后，每个采样点启动一次。最简单的是使用启动脚本；以下命令会把该点放入一个单独、带时间戳的目录，避免覆盖之前的采集：

```powershell
.\\scripts\\Start-KuroCaptureAssistant.ps1 -Scene LowerVault -Sample 01
```

也可以直接运行程序并自定义保存位置：

```powershell
.\\x64\\Release\\IMaoCaptureAssistant.exe --scene LowerVault --sample 01 --output .\\Assets\\KuroMap\\captures --max-seconds 10
```

助手运行期间可把焦点留在游戏里：

| 快捷键 | 保存内容 | 什么时候按 |
| --- | --- | --- |
| `Ctrl+Alt+F9` | `normal.png` | 正常游戏画面，小地图和位置数字清晰时。 |
| `Ctrl+Alt+F10` | `map.png` | 按 `M` 后，大地图玩家箭头清晰时。人物在两张图之间不要移动。 |
| `Ctrl+Alt+F11` | `move.mp4` | 开始/停止 15 FPS 无声录像；忘记停止时最多自动录制 `--max-seconds` 秒。 |

录像用于正常移动验证，建议单独启动一次助手并录 5–10 秒。`move.mp4` 使用 Windows 自带 H.264 编码，不需要额外下载录像工具。若快捷键无响应，通常是别的软件占用了组合键；若游戏以管理员权限运行，也请以管理员权限运行助手。

| 场景 | state | 四个采样位置 |
| --- | ---: | --- |
| `LowerVault` | 902 | 声骸站台、贵金属与艺术品藏馆、人才藏馆、费舍塔 |
| `Darkplain` | 909 | 落日堤屿、封存地、寂静断崖、恒黯之原 |
| `TimeRiftRuins` | 910 | 时隙废都、细波大道、幸缘喷泉广场、列车长咖啡厅 |
| `MengshuTianluo` | 912 | **已采集**（2026-09-30 19:03–19:06，四组）：游戏平面坐标 (-413, -209)、(-179, 281)、(409, 390)、(490, -124)。样本在 `map-regions/samples/mengshutianluo.json`，`map` 由大地图视口定位给出。 |

采集命令（`-Scene` 已接受 `MengshuTianluo`）：

```powershell
.\scripts\Start-KuroCaptureAssistant.ps1 -Scene MengshuTianluo -Sample 01
```

> **这个 frame 的原点已实测证实**：拟合结果是 `origin = (-0.127, -0.511)`、`scale = 1.20700208`、
> 最大误差 **0.534 px**——编译期占位值 `(0, 0)` 是对的（下层金库 −3.5/−2.5、黯原 −0.4/0.2 也是这个规律）。
> 还缺的**不是**校准，而是一张**参考小地图**（见下）。

将大地图箭头中心换算为本项目内部地图像素后，制作一个 JSON 文件：

### `map` 值怎么量（2026-09-30 实测确定，以前没写下来）

`map` 是**该位置在本项目内部地图像素坐标系里的坐标**。它由**大地图截图**离线匹配得到，
不需要游戏在跑：

```powershell
# 1. 让工具能看见这个地区的包：它只从 <root>/Assets/FeaturesDatas 读资源。
#    diagnosticPacks 是专门的离线通道——未验证的新包也能加载，而且只在本进程内
#    把 Scene::runtimeApproval 打开，不写任何生产审批文件。
#    （实测：用硬链接指向 out/map-regions/base-index 的退役基础图集，别去复制 150 MB。）

# 2. 写一个视口清单
#    { "viewport": true,
#      "diagnosticPacks": ["<region>"],
#      "samples": [ { "id": "p1", "image": "<大地图截图绝对路径>" }, … ] }

x64\Release\IMaoVisualRegression.exe <root> <清单.json> <报告.json>
```

报告里每个样本给 `captureCorners`（**裁剪区四角在本项目地图像素里的坐标**，顺序 TL/TR/BR/BL）、
`mapX`/`mapY`（裁剪区中心的地图像素坐标）、`accepted`、`sceneId`、`matches`/`inliers`。

**游戏打开大地图时把视口居中在玩家身上**，所以 `mapX/mapY` 就是玩家的地图像素坐标——
这正是泰缇斯那次写的"四张大地图匹配得到的中心用于拟合"。要自己核对的话，箭头位置与中心的换算关系是：

```
裁剪区 = hud::kMapCenterArea = 参考布局 (left 160, top 135, right 1440, bottom 765)
        = 1280 x 630 参考单位，居中锚定；客户端缩放 = min(宽/1600, 高/900)
scalingFactor = 裁剪区宽(像素) / (corners[2].x - corners[0].x)      # 屏幕像素 / 地图像素
屏幕中心      = (客户端宽/2, 客户端高/2)                            # 对称锚定，等于裁剪区中心
mapX = centerX + (arrowScreenX - 屏幕中心.x) / scalingFactor
mapY = centerY - (arrowScreenY - 屏幕中心.y) / scalingFactor        # 地图 y 向上
```

梦枢天罗实测：四张大地图各自解出 `sceneId=9`、matches 126~368；**用中心**拟合得到
max 0.534 px，**用箭头像素质心**拟合得到 max 2.247 px。箭头质心差在 y 上系统性偏下约 2.4 屏幕像素
（图标字形的锚点与着色像素质心不重合，且 p1 的箭头还压在传送环上），所以**取视口中心**。
两条路都在 8 px 门限内，但中心那条好一倍，这个对比本身也说明"大地图确实居中在玩家身上"。

随后执行：

```json
{
  "formatVersion": 1,
  "scene": "LowerVault",
  "state": 902,
  "samples": [
    { "game": { "x": 0, "y": 0 }, "map": { "x": 0, "y": 0 }, "capture": "samples/LowerVault/01" },
    { "game": { "x": 0, "y": 0 }, "map": { "x": 0, "y": 0 }, "capture": "samples/LowerVault/02" },
    { "game": { "x": 0, "y": 0 }, "map": { "x": 0, "y": 0 }, "capture": "samples/LowerVault/03" },
    { "game": { "x": 0, "y": 0 }, "map": { "x": 0, "y": 0 }, "capture": "samples/LowerVault/04" }
  ]
}
```

随后执行：

```powershell
.\scripts\Set-KuroSceneCalibration.ps1 -Scene LowerVault -SamplesPath .\Assets\KuroMap\samples\LowerVault.json -Check
.\scripts\Set-KuroSceneCalibration.ps1 -Scene LowerVault -SamplesPath .\Assets\KuroMap\samples\LowerVault.json -Apply
```

脚本以四个样本一起求一个缩放比例和两个原点，逐点计算误差；最大误差超过 8 个内部地图像素会直接拒绝写入。通过校准只会更新地图换算参数，仍不会把区域开放给用户。每个区域还要录制一次 5–10 秒的正常移动画面，用于后续的小地图连续跟踪验证。

完成瓦片包生成、静态测试与实机验证后，保存一份如下结构的证据 JSON（截图与视频路径由 `captures` 字段引用）：

```json
{
  "formatVersion": 1,
  "scene": "LowerVault",
  "state": 902,
  "calibrationPairCount": 4,
  "checks": {
    "minimapLocalization": true,
    "continuousTracking": true,
    "gameMapMarkers": true,
    "minimapMarkers": true
  },
  "visualRegression": {
    "sampleCount": 113,
    "baselineFinalAccepted": 68,
    "currentFinalAccepted": 68,
    "baselineFalseAccepted": 2,
    "currentFalseAccepted": 2,
    "wrongSceneAccepted": 0,
    "distantWrongAccepted": 0
  },
  "captures": ["captures/LowerVault/move.mp4"]
}
```

最后才允许开放该地区：

```powershell
.\scripts\Approve-KuroSceneRelease.ps1 -Scene LowerVault -GameEvidencePath .\Assets\KuroMap\evidence\LowerVault.json -Check
.\scripts\Approve-KuroSceneRelease.ps1 -Scene LowerVault -GameEvidencePath .\Assets\KuroMap\evidence\LowerVault.json -Apply
```

批准脚本会重新检查四点校准、独立瓦片包的哈希与参考小地图误差、四项实机结果以及 113 张基线回归结果。任一项缺失或失败，该地区的原始点位仍留在仓库，但不会进入筛选、物品标注或视觉定位。
