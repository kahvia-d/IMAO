# 叠加层呈现路径探针（OverlayPresentProbe）

这个小工具**不是程序的一部分**，它只回答一个问题：

> 把叠加层「贴」到游戏上的方式，从 `WS_EX_LAYERED` + `LWA_COLORKEY` 换成
> DirectComposition，能不能把游戏放回 **Independent Flip（独立翻转）**？

## 为什么需要它

2026-09-18 的四组对照实验已经确定（见 `Docs/GameFrameCostAnalysis_20260918.md` 第 12 节）：

- 一个**可见的**整屏置顶分层窗口本身就让游戏掉约 **10 fps**；
- 这个代价**与刷新次数无关**——把 present 从 12-15 次/秒降到 0.3 次/秒，帧率没有任何回升；
- 因此「缩小窗口」无效（面积不是成本项），而历史文档里那句「DirectComposition 是唯
  一有机会恢复独立翻转的路」**从未被验证过**。

完整改造覆盖层呈现方式的代价很大，而且成功率存疑，所以先用这个探针花很小的代价验证前提。
**如果探针都做不到，完整改造也做不到。**

## 它测什么、不测什么

刻意做成最小：

| 有 | 没有 |
| --- | --- |
| 与真实覆盖层同尺寸（游戏客户区）、同位置、同为鼠标穿透 + 置顶 + 不激活的窗口 | 画面采集 |
| 顶部中央一块 450×70 的圆角色块（对应真实状态条的位置与面积） | 定位、追踪 |
| 三种模式：`none` / `dcomp` / `layered` | ImGui、标记绘制 |

采集与定位**已经单独测出约 10 fps 的代价**，如果放进来会掩盖这里要测的东西，所以不放。

`dcomp` 模式与真实覆盖层的唯一区别就是它要测的那一点：

| | 真实覆盖层（`layered` 模式） | 探针 `dcomp` 模式 |
| --- | --- | --- |
| 窗口样式 | `WS_EX_LAYERED` + `SetLayeredWindowAttributes(LWA_COLORKEY)` | `WS_EX_NOREDIRECTIONBITMAP` |
| 表面 | 交换链 → 重定向位图 | 交换链 → DirectComposition visual |

## 怎么跑

**需要管理员权限**（PresentMon 要建 ETW 会话）。跑之前先把游戏开起来并保持在前台。

```powershell
# 在管理员 PowerShell 里，从仓库根目录运行
.\scripts\Test-OverlayPresentPath.ps1
```

它会按 `none → dcomp → layered` 三段各约 45 秒跑完，同时录一份 PresentMon 轨迹，结束时打印
`PresentMode` 分布。约 3 分钟。

可调参数：

```powershell
.\scripts\Test-OverlayPresentPath.ps1 -PhaseSeconds 60      # 每段更长
.\scripts\Test-OverlayPresentPath.ps1 -SkipLayered          # 只跟 none 比
```

也可以单独手动跑探针：

```powershell
.\x64\Release\IMaoOverlayPresentProbe.exe --mode=dcomp   --hold=60
.\x64\Release\IMaoOverlayPresentProbe.exe --mode=layered --hold=60
.\x64\Release\IMaoOverlayPresentProbe.exe --mode=none    --hold=60
```

- `--block=WxH`、`--alpha=0..1` 调整色块；`--no-pump` 关掉逐显示帧节流。
- 默认用 `DwmFlush()` 让每次循环对齐一个显示帧，这是**更严苛**的情况：如果代价来自合成本身，
  这样能把它放到最大。
- 色块是醒目颜色的，**必须能看见**。如果看不到色块，说明设置失败了，那这一轮数据无效
  （"没有代价"和"什么都没画"必须分得清）。

## 结果怎么读

产物：

| 文件 | 内容 |
| --- | --- |
| `out/perf/overlay-present-path.csv` | PresentMon 逐帧数据 |
| `out/perf/overlay-present-path.phases.txt` | 三段起止的本地时间戳 |
| `out/perf/overlay-present-path.presentmode.txt` | 全程 `PresentMode` 分布 |

按 `phases.txt` 的时间戳把 CSV 切成三段，然后比较 `dcomp` 与 `layered`：

| 观察到 | 含义 | 下一步 |
| --- | --- | --- |
| `dcomp` 段出现 `Hardware: Independent Flip`，`layered` 段是 `Composed: Flip` | **前提成立**，换贴法确实能恢复独立翻转 | 值得投入完整改造 |
| 两段都是 `Composed: Flip` | 换贴法**不能**恢复独立翻转 | 探针已经否决了这条路，省下完整改造的工作 |
| 两段 `PresentMode` 相同，但 `dcomp` 帧时间尾部明显更好 | 部分收益（合成更便宜但没恢复翻转） | 按收益大小决定 |

注意：对齐时间戳时要记得 PresentMon 的 `--date_time` 列与本地时间的关系
（2026-09-17 那次实测是**快 8 小时**，见 `Docs/GameFrameDropAnalysis_20260917.md`）。

## 退出

探针只在三种模式之间切换，不写任何配置、不改动程序文件、不做持久化。删掉
`IMao-Core/tools/OverlayPresentProbe/` 并在 `CMakeLists.txt` 里删掉 `IMaoOverlayPresentProbe`
目标即可完全移除。
