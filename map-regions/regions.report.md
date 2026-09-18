# 地图地区注册表

由 `scripts/New-MapRegionRegistry.ps1` 生成，数据源 `Assets/KuroMap`（Kuro resource version `E62CEAC5F80745288BF76C7AD5F731C3`）。

瓦片换算经过地面真值验证：14 个独立小世界子区域锚点全部落在其既有包的瓦片矩形内。

| 地区 | id | 类型 | frame | mapState | 子区域 | 点数 | 瓦片窗口 | 瓦片数 | 置信度 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 今州 | `jinzhou` | overworld | 8 | 1 | 11 | 9234 | x -3..14, y -10..11 | 396 | validated |
| 梦州 | `mengzhou` | overworld | 8 | 8 | 4 | 1483 | x -12..-4, y -2..4 | 63 | validated |
| 拉古那 | `laguna` | overworld | 8 | 3 | 10 | 5610 | x 6..15, y -12..-1 | 120 | validated |
| 七丘 | `qiqiu` | overworld | 8 | 4 | 2 | 1345 | x 11..18, y -18..-10 | 72 | validated |
| 冰原地表 | `roysurface` | overworld | 8 | 6 | 5 | 1160 | x -4..4, y 4..13 | 90 | validated |
| 黑海岸群岛 | `blackshores` | overworld | 8 |  | 1 | 352 | x 2..6, y -3..2 | 30 | validated |
| 拉海洛 | `lahai` | subworld | 906 | 5 | 9 | 2536 | x -7..4, y 3..13 | 132 | calibrated |
| 黯原 | `darkplain` | subworld | 909 | 7 | 4 | 673 | x -2..5, y -3..3 | 56 | calibrated |
| 下层金库 | `lowervault` | subworld | 902 | 3 | 1 | 238 | — | — | blocked |
| 阿维纽林 | `avinoleum` | subworld | 903 | 3 | 1 | 462 | x -3..16, y -15..4 | 400 | uncalibrated |
| 隐海试验场 | `fabricatorium` | subworld | 905 | 3 | 1 | 236 | x -5..7, y -4..4 | 117 | uncalibrated |
| 泰缇斯之底 | `tethys` | subworld | 900 |  | 1 | 415 | x -2..3, y -3..2 | 36 | calibrated |
| 时隙废都 | `timeriftruins` | subworld | 910 |  | 1 | 59 | — | — | blocked |

- 点位合计：**23803**（大世界 19184）
- 瓦片合计：**1512**（含每侧 2 块覆盖边距；仅统计可推导窗口的地区）
- 触发 256 上限的地区：今州(396)、阿维纽林(400)

瓦片窗口由点位分位数推导后**每侧外扩 2 块**。分位数只保证覆盖收集品所在处，最小地图匹配必须在玩家能站到的任何位置工作，所以窗口是下界加上行走边距。

## 置信度

- `validated`：frame 8（大世界）。换算用 6 个既有包的锚点/矩形做过地面真值验证。
- `calibrated`：该 frame 有通过的四点校准。
- `uncalibrated`：无校准，窗口只是猜测，**不得据此发布资源包**。
- `blocked`：frame 原点仍是编译期占位值 `(0, 0)`。

### 需要先补校准的地区

- 阿维纽林（`avinoleum`, frame 903）— 推导窗口 400 块，未经校准不可信
- 隐海试验场（`fabricatorium`, frame 905）— 推导窗口 117 块，未经校准不可信

### 被阻塞的地区

- 下层金库（`lowervault`, frame 902）
- 时隙废都（`timeriftruins`, frame 910）
