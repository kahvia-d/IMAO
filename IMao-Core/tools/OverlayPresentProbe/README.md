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
| 四种模式：`none` / `plain` / `dcomp` / `layered` | ImGui、标记绘制 |

**每一段用不同颜色，屏幕上一眼就能确认自己在哪一段：**

| 模式 | 颜色 | 说明 |
| --- | --- | --- |
| `none` | **无色块** | **故意什么都不显示**——这就是基准，别以为它坏了 |
| `dcomp` | **红** | 新贴法：`WS_EX_NOREDIRECTIONBITMAP` + DirectComposition |
| `layered` | **绿** | 今天的贴法：`WS_EX_LAYERED` + colorkey |
| `plain` | **蓝** | 额外参照：普通不透明窗口（可选，见 `-IncludePlain`） |

色块是**必须能看见**的。如果某一轮该看到颜色却没有，说明那一段无效——2026-09-18 12:23 那轮
`layered` 就是这种情况：它什么都没画，而 colorkey 让纯黑表面完全透明，于是那个窗口等于不在
合成里，跟 `mode=none` 是同一件事，数据不能用来代表「可见分层窗口」
（详见 `Docs/GameFrameCostAnalysis_20260918.md` 第 13 节）。

每段开始时探针会打印一行 `geometry:`，写明窗口、客户区与色块的实际位置尺寸，例如：

```text
geometry: windowRect=0,0 2560x1440 client=2560x1440 block=999,22 562x87
```

**三段这一行必须完全一致**（`block=` 部分）。如果哪一段的数字不同，那一段就不能用于对比——
这是为了把「色块位置不对」变成日志里一个可以核对的数字，而不是靠眼睛判断（2026-09-18 那轮
`layered` 的色块被放在左边、宽度为 0，就是因为它的表面尺寸没有赋值）。

> 色块尺寸会随 Windows 显示缩放比例变化（125% 缩放下 `450x70` 会画成 `562x87`），
> 位置在顶部居中，这是预期的。

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
`PresentMode` 分布。约 3 分钟。**第一段屏幕上什么都不显示，那是基准**；第二段红色、第三段绿色。

可调参数：

```powershell
.\scripts\Test-OverlayPresentPath.ps1 -PhaseSeconds 60      # 每段更长
.\scripts\Test-OverlayPresentPath.ps1 -PresentHz 0          # 按显示刷新率刷，取最大代价
.\scripts\Test-OverlayPresentPath.ps1 -IncludePlain         # 末尾额外跑一段蓝色普通窗口
.\scripts\Test-OverlayPresentPath.ps1 -SkipLayered          # 只跟 none 比
```

也可以单独手动跑探针：

```powershell
.\x64\Release\IMaoOverlayPresentProbe.exe --mode=dcomp   --hold=60   # 红
.\x64\Release\IMaoOverlayPresentProbe.exe --mode=layered --hold=60   # 绿
.\x64\Release\IMaoOverlayPresentProbe.exe --mode=plain   --hold=60   # 蓝
.\x64\Release\IMaoOverlayPresentProbe.exe --mode=none    --hold=60   # 无色块
```

- `--block=WxH`、`--alpha=0..1` 调整色块。
- `--hz=N` 控制刷新频率，**默认 30**，与真实覆盖层一致；`--hz=0` 改为用 `DwmFlush()`
  每次循环对齐一个显示帧（更严苛，用来观察最大值）。
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
| 两段 `PresentMode` 都是 `Independent Flip` | 那么"覆盖层把游戏压回合成"这个前提**不成立**，掉帧另有来源 | 不要再投入呈现路径改造，回去查别的原因 |

对齐时间戳时要记得 PresentMon 的 `--date_time` 列与本地时间的关系
（2026-09-17 那次实测是**快 8 小时**，见 `Docs/GameFrameDropAnalysis_20260917.md`）。

## 退出

探针只在三种模式之间切换，不写任何配置、不改动程序文件、不做持久化。删掉
`IMao-Core/tools/OverlayPresentProbe/` 并在 `CMakeLists.txt` 里删掉 `IMaoOverlayPresentProbe`
目标即可完全移除。
