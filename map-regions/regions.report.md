# 地图地区注册表

由 `scripts/New-MapRegionRegistry.ps1` 生成，数据源 `Assets/KuroMap`（Kuro resource version `13CCF182D6AF491CA1AC2A02754E5345`）。

瓦片换算经过地面真值验证：15 个独立小世界子区域锚点（拉海洛 9、黯原 4、泰缇斯之底 1、梦枢天罗 1）全部落在其既有包的瓦片矩形内。

| 地区 | id | 类型 | frame | mapState | 子区域 | 点数 | 瓦片窗口 | 窗口格数 | 实际有图 | 置信度 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 今州 | `jinzhou` | overworld | 8 | 1 | 11 | 9235 | x -3..15, y -14..11 | 494 | 196 | validated |
| 梦州 | `mengzhou` | overworld | 8 | 8 | 4 | 1486 | x -12..-4, y -2..4 | 63 | 63 | validated |
| 拉古那 | `laguna` | overworld | 8 | 3 | 10 | 5610 | x 6..15, y -12..-1 | 120 | 85 | validated |
| 七丘 | `qiqiu` | overworld | 8 | 4 | 2 | 1344 | x 11..18, y -18..-10 | 72 | 39 | validated |
| 冰原地表 | `roysurface` | overworld | 8 | 6 | 5 | 1160 | x -4..4, y 4..13 | 90 | 70 | validated |
| 黑海岸群岛 | `blackshores` | overworld | 8 |  | 1 | 352 | x 2..6, y -3..2 | 30 | 28 | validated |
| 拉海洛 | `lahai` | subworld | 906 | 5 | 9 | 2536 | x -7..4, y 3..13 | 132 | 110 | calibrated |
| 黯原 | `darkplain` | subworld | 909 | 7 | 4 | 702 | x -2..5, y -3..3 | 56 | 25 | calibrated |
| 下层金库 | `lowervault` | subworld | 902 | 3 | 1 | 238 | x 1..11, y -5..3 | 99 | 9 | calibrated |
| 阿维纽林 | `avinoleum` | subworld | 903 | 3 | 1 | 462 | x -3..16, y -15..4 | 400 | 36 | uncalibrated |
| 隐海试验场 | `fabricatorium` | subworld | 905 | 3 | 1 | 236 | x -5..7, y -4..4 | 117 | 84 | uncalibrated |
| 泰缇斯之底 | `tethys` | subworld | 900 |  | 1 | 415 | x -2..3, y -3..2 | 36 | 13 | calibrated |
| 时隙废都 | `timeriftruins` | subworld | 910 |  | 1 | 59 | x -2..3, y -2..3 | 36 | 4 | calibrated |
| 梦枢天罗 | `mengshutianluo` | subworld | 912 | 8 | 1 | 231 | x -1..2, y -1..2 | 16 | — | footprint-measured |

- 点位合计：**24066**（大世界 19187）
- 窗口格数合计：**1761**（点位推导的窗口含每侧 2 块覆盖边距，实测足迹的窗口不含边距）
- 实际有图瓦片：**762** 张（13 个地区记在 `tiles.manifest.json` 里；实测足迹地区的瓦片清单记在 `map-regions/footprints/`，上游缺失的格子不计入，故小于窗口格数）
- 注：**窗口格数不是下载量**。相邻地区在同一坐标平面上窗口会交叠（六个地表地区共用 frame 8），实际唯一瓦片文件数见 `map-regions/tiles/tiles.manifest.json`。
- 触发 256 上限的地区：今州(494)、阿维纽林(400)

瓦片窗口由点位分位数推导后**每侧外扩 2 块**。分位数只保证覆盖收集品所在处，最小地图匹配必须在玩家能站到的任何位置工作，所以窗口是下界加上行走边距。

## 置信度

- `validated`：frame 8（大世界）。换算用 6 个既有包的锚点/矩形做过地面真值验证。
- `calibrated`：该 frame 有通过的四点校准。
- `footprint-measured`：窗口取**实测**的上游有图瓦片包围盒（`map-regions/footprints/<id>.json`）。窗口与原点无关，因此可信；但该 frame 的 raw→game 原点仍未证实，**打包可以，开放不行**。
- `uncalibrated`：无校准，窗口只是猜测，**不得据此发布资源包**。
- `blocked`：frame 原点仍是编译期占位值 `(0, 0)`。

### 窗口已实测、但仍缺实机证据的地区

- 梦枢天罗（`mengshutianluo`, frame 912）— 实测有图 12 块 / 窗口 16 块。缺：四点校准（实机读 4 个位置坐标）与参考小地图截图；补齐前该场景保持 `approved=false`。

### 需要先补校准的地区

- 阿维纽林（`avinoleum`, frame 903）— 推导窗口 400 块，未经校准不可信
- 隐海试验场（`fabricatorium`, frame 905）— 推导窗口 117 块，未经校准不可信

### 被阻塞的地区

无。
